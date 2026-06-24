#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#define MAX_SAMPLES 200000
#define TS_QUEUE_SIZE 8192
#define DEFAULT_WARMUP_SAMPLES 128

typedef enum {
    MODE_RAW,
    MODE_MOTION,
} MeasureMode;

typedef struct {
    double values[MAX_SAMPLES];
    size_t count;
    size_t dropped;
} Stats;

typedef struct {
    double values[TS_QUEUE_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    size_t overflow;
} TimestampQueue;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static double input_event_ms(const struct input_event *ev)
{
    return (double)ev->time.tv_sec * 1000.0 + (double)ev->time.tv_usec / 1000.0;
}

static void queue_push(TimestampQueue *queue, double value)
{
    if (queue->count == TS_QUEUE_SIZE) {
        queue->head = (queue->head + 1) % TS_QUEUE_SIZE;
        queue->count--;
        queue->overflow++;
    }

    queue->values[queue->tail] = value;
    queue->tail = (queue->tail + 1) % TS_QUEUE_SIZE;
    queue->count++;
}

static bool queue_pop(TimestampQueue *queue, double *value)
{
    if (queue->count == 0)
        return false;

    *value = queue->values[queue->head];
    queue->head = (queue->head + 1) % TS_QUEUE_SIZE;
    queue->count--;
    return true;
}

static void stats_add(Stats *stats, double value)
{
    if (stats->count == MAX_SAMPLES) {
        stats->dropped++;
        return;
    }

    stats->values[stats->count++] = value;
}

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;

    if (da < db)
        return -1;
    if (da > db)
        return 1;
    return 0;
}

static double percentile(const Stats *stats, double pct)
{
    size_t index;

    if (stats->count == 0)
        return NAN;

    index = (size_t)ceil((pct / 100.0) * (double)stats->count);
    if (index == 0)
        index = 1;
    if (index > stats->count)
        index = stats->count;

    return stats->values[index - 1];
}

static void print_stats(Stats *stats, const TimestampQueue *queue)
{
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;

    if (stats->count == 0) {
        printf("No matched samples. Move the mouse while the tool is running.\n");
        printf("Unmatched evdev events still queued: %zu\n", queue->count);
        return;
    }

    qsort(stats->values, stats->count, sizeof(stats->values[0]), compare_double);

    min = stats->values[0];
    max = stats->values[stats->count - 1];

    for (size_t i = 0; i < stats->count; i++)
        sum += stats->values[i];

    printf("samples: %zu\n", stats->count);
    printf("mean_ms: %.3f\n", sum / (double)stats->count);
    printf("min_ms: %.3f\n", min);
    printf("p50_ms: %.3f\n", percentile(stats, 50.0));
    printf("p90_ms: %.3f\n", percentile(stats, 90.0));
    printf("p95_ms: %.3f\n", percentile(stats, 95.0));
    printf("p99_ms: %.3f\n", percentile(stats, 99.0));
    printf("max_ms: %.3f\n", max);
    printf("unmatched_evdev_queued: %zu\n", queue->count);
    printf("queue_overflow: %zu\n", queue->overflow);
    printf("sample_overflow: %zu\n", stats->dropped);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s --event /dev/input/eventX [--device-id N] [--mode raw|motion] [--samples N] [--seconds N]\n"
            "          [--warmup N]\n"
            "\n"
            "Examples:\n"
            "  sudo %s --event /dev/input/event5 --device-id 11 --mode raw --samples 2000\n"
            "  sudo %s --event /dev/input/event5 --device-id 11 --mode motion --seconds 10\n",
            argv0, argv0, argv0);
}

static bool parse_long(const char *value, long *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;

    *out = parsed;
    return true;
}

static int open_evdev(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    unsigned int clock_id = CLOCK_MONOTONIC;

    if (fd < 0) {
        perror("open evdev");
        return -1;
    }

    if (ioctl(fd, EVIOCSCLOCKID, &clock_id) < 0)
        perror("EVIOCSCLOCKID");

    return fd;
}

static bool setup_xi2(Display **display_out, int *xi_opcode_out, MeasureMode mode)
{
    Display *display = XOpenDisplay(NULL);
    int event = 0;
    int error = 0;
    int major = 2;
    int minor = 0;
    Window root;
    XIEventMask mask;
    unsigned char bits[(XI_LASTEVENT + 7) / 8] = {0};

    if (!display) {
        fprintf(stderr, "Failed to open X display. Is DISPLAY set?\n");
        return false;
    }

    if (!XQueryExtension(display, "XInputExtension", xi_opcode_out, &event, &error)) {
        fprintf(stderr, "XInput2 extension not available.\n");
        XCloseDisplay(display);
        return false;
    }

    if (XIQueryVersion(display, &major, &minor) != Success) {
        fprintf(stderr, "XI2 query failed.\n");
        XCloseDisplay(display);
        return false;
    }

    root = DefaultRootWindow(display);
    mask.deviceid = XIAllDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;

    if (mode == MODE_RAW)
        XISetMask(bits, XI_RawMotion);
    else
        XISetMask(bits, XI_Motion);

    XISelectEvents(display, root, &mask, 1);
    XFlush(display);

    *display_out = display;
    return true;
}

static bool raw_event_has_motion(const XIRawEvent *event)
{
    int value_index = 0;

    for (int i = 0; i < event->valuators.mask_len * 8; i++) {
        if (!XIMaskIsSet(event->valuators.mask, i))
            continue;

        if (i < 2 && event->raw_values && event->raw_values[value_index] != 0.0)
            return true;

        value_index++;
    }

    return false;
}

static bool device_event_has_motion(const XIDeviceEvent *event)
{
    int value_index = 0;

    for (int i = 0; i < event->valuators.mask_len * 8; i++) {
        if (!XIMaskIsSet(event->valuators.mask, i))
            continue;

        if (i < 2 && event->valuators.values && event->valuators.values[value_index] != 0.0)
            return true;

        value_index++;
    }

    return false;
}

static void drain_evdev(int fd)
{
    struct input_event events[64];

    while (read(fd, events, sizeof(events)) > 0)
        ;
}

static void drain_x_events(Display *display)
{
    while (XPending(display) > 0) {
        XEvent event;

        XNextEvent(display, &event);
        if (event.xcookie.type == GenericEvent && XGetEventData(display, &event.xcookie))
            XFreeEventData(display, &event.xcookie);
    }
}

int main(int argc, char **argv)
{
    const char *event_path = NULL;
    long device_id = -1;
    long max_samples = 2000;
    long seconds = 0;
    MeasureMode mode = MODE_RAW;
    int evdev_fd;
    Display *display = NULL;
    int xi_opcode = 0;
    int x_fd;
    struct pollfd fds[2];
    TimestampQueue queue = {0};
    Stats stats = {0};
    double deadline = 0.0;
    bool saw_rel = false;
    double last_syn_ts = 0.0;
    bool samples_set = false;
    long warmup_samples = DEFAULT_WARMUP_SAMPLES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--event") == 0 && i + 1 < argc) {
            event_path = argv[++i];
        } else if (strcmp(argv[i], "--device-id") == 0 && i + 1 < argc) {
            if (!parse_long(argv[++i], &device_id)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            if (!parse_long(argv[++i], &max_samples) || max_samples <= 0 || max_samples > MAX_SAMPLES) {
                usage(argv[0]);
                return 2;
            }
            samples_set = true;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            if (!parse_long(argv[++i], &seconds) || seconds < 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            if (!parse_long(argv[++i], &warmup_samples) || warmup_samples < 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode_name = argv[++i];
            if (strcmp(mode_name, "raw") == 0)
                mode = MODE_RAW;
            else if (strcmp(mode_name, "motion") == 0)
                mode = MODE_MOTION;
            else {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!event_path) {
        usage(argv[0]);
        return 2;
    }

    evdev_fd = open_evdev(event_path);
    if (evdev_fd < 0)
        return 1;

    if (!setup_xi2(&display, &xi_opcode, mode)) {
        close(evdev_fd);
        return 1;
    }

    x_fd = ConnectionNumber(display);
    fds[0].fd = evdev_fd;
    fds[0].events = POLLIN;
    fds[1].fd = x_fd;
    fds[1].events = POLLIN;

    drain_evdev(evdev_fd);
    drain_x_events(display);

    if (seconds > 0)
        deadline = now_ms() + (double)seconds * 1000.0;

    if (seconds > 0 && !samples_set)
        max_samples = MAX_SAMPLES;

    printf("Measuring %s latency. Move the mouse now.\n", mode == MODE_RAW ? "XI_RawMotion" : "XI_Motion");
    if (seconds > 0 && samples_set)
        printf("Stop condition: %ld samples or %ld seconds\n", max_samples, seconds);
    else if (seconds > 0)
        printf("Stop condition: %ld seconds\n", seconds);
    else
        printf("Stop condition: %ld samples\n", max_samples);
    printf("Warmup: %ld matched samples discarded\n", warmup_samples);
    fflush(stdout);

    while (stats.count < (size_t)max_samples) {
        int rc;

        if (deadline > 0.0 && now_ms() >= deadline)
            break;

        rc = poll(fds, 2, 100);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            struct input_event ev;

            while (read(evdev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y) && ev.value != 0) {
                    saw_rel = true;
                    last_syn_ts = input_event_ms(&ev);
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && saw_rel) {
                    double ts = input_event_ms(&ev);
                    if (ts <= 0.0)
                        ts = last_syn_ts;
                    queue_push(&queue, ts);
                    saw_rel = false;
                }
            }
        }

        while (XPending(display) > 0) {
            XEvent event;

            XNextEvent(display, &event);

            if (event.xcookie.type != GenericEvent || event.xcookie.extension != xi_opcode)
                continue;

            if (!XGetEventData(display, &event.xcookie))
                continue;

            if (mode == MODE_RAW && event.xcookie.evtype == XI_RawMotion) {
                XIRawEvent *raw = event.xcookie.data;
                double evdev_ts;

                if ((device_id < 0 || raw->sourceid == device_id || raw->deviceid == device_id) &&
                    raw_event_has_motion(raw) &&
                    queue_pop(&queue, &evdev_ts)) {
                    if (warmup_samples > 0)
                        warmup_samples--;
                    else
                        stats_add(&stats, now_ms() - evdev_ts);
                }
            } else if (mode == MODE_MOTION && event.xcookie.evtype == XI_Motion) {
                XIDeviceEvent *motion = event.xcookie.data;
                double evdev_ts;

                if ((device_id < 0 || motion->sourceid == device_id || motion->deviceid == device_id) &&
                    device_event_has_motion(motion) &&
                    queue_pop(&queue, &evdev_ts)) {
                    if (warmup_samples > 0)
                        warmup_samples--;
                    else
                        stats_add(&stats, now_ms() - evdev_ts);
                }
            }

            XFreeEventData(display, &event.xcookie);
        }
    }

    print_stats(&stats, &queue);

    XCloseDisplay(display);
    close(evdev_fd);
    return 0;
}
