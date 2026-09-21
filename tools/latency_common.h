#include "latency_match.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define MAX_SAMPLES 200000
#ifdef AINPUT_KEYBOARD_BENCHMARK
#define IS_KEYBOARD 1
#define COOKED_NAME "key"
#define DEFAULT_WARMUP 64
#else
#define IS_KEYBOARD 0
#define COOKED_NAME "motion"
#define DEFAULT_WARMUP 128
#endif

typedef struct {
  double values[MAX_SAMPLES];
  bool after_idle[MAX_SAMPLES];
  size_t count;
} Stats;
typedef struct {
  double evdev_monotonic_ms;
  double received_monotonic_ms;
  uint32_t xserver_time_ms;
} AInputStageSample;
static double now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    perror("clock_gettime");
    exit(1);
  }
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
static double event_ms(const struct input_event *ev) {
  return ev->time.tv_sec * 1000.0 + ev->time.tv_usec / 1000.0;
}
static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
static void print_stats(Stats *s, const char *prefix) {
  double sum = 0;
  printf("%ssamples: %zu\n", prefix, s->count);
  if (!s->count)
    return;
  qsort(s->values, s->count, sizeof(double), compare_double);
  for (size_t i = 0; i < s->count; i++)
    sum += s->values[i];
  printf("%smean_ms: %.6f\n%smin_ms: %.6f\n", prefix, sum / s->count, prefix,
         s->values[0]);
  const unsigned int pct[] = {50, 90, 95, 99};
  for (size_t i = 0; i < 4; i++) {
    size_t at = (s->count * pct[i] + 99) / 100 - 1;
    printf("%sp%u_ms: %.6f\n", prefix, pct[i], s->values[at]);
  }
  printf("%smax_ms: %.6f\n", prefix, s->values[s->count - 1]);
}
static void save_samples_prompt(Stats *s, const AInputStageSample *stages) {
  char answer[16];
  printf("Save all matched latency samples to a file? [y/N]: ");
  fflush(stdout);
  if (!fgets(answer, sizeof(answer), stdin))
    return;
  if (answer[0] != 'y' && answer[0] != 'Y')
    return;

  char path[512];
  printf("Filename [default: latency_samples.csv]: ");
  fflush(stdout);
  if (!fgets(path, sizeof(path), stdin))
    return;
  size_t len = strlen(path);
  while (len && (path[len - 1] == '\n' || path[len - 1] == '\r'))
    path[--len] = 0;
  if (!len)
    strcpy(path, "latency_samples.csv");

  FILE *f = fopen(path, "w");
  if (!f) {
    perror("fopen");
    return;
  }
  fprintf(f, "index,latency_ms,after_idle,evdev_monotonic_ms,"
             "xserver_time_ms,received_monotonic_ms\n");
  for (size_t i = 0; i < s->count; i++)
    fprintf(f, "%zu,%.6f,%d,%.6f,%" PRIu32 ",%.6f\n", i, s->values[i],
            s->after_idle[i] ? 1 : 0, stages[i].evdev_monotonic_ms,
            stages[i].xserver_time_ms, stages[i].received_monotonic_ms);
  fclose(f);
  printf("Saved %zu samples to %s\n", s->count, path);
}
static bool number(const char *s, long *out) {
  char *end;
  errno = 0;
  *out = strtol(s, &end, 10);
  return !errno && end != s && !*end && *out >= 0;
}
static bool same_node(const char *a, const char *b) {
  struct stat x, y;
  return !stat(a, &x) && !stat(b, &y) && S_ISCHR(x.st_mode) &&
         S_ISCHR(y.st_mode) && x.st_rdev == y.st_rdev;
}
static int select_device(Display *d, const char *path, long requested) {
  int count = 0, chosen = -1;
  XIDeviceInfo *info = XIQueryDevice(d, XIAllDevices, &count);
  Atom node = XInternAtom(d, "Device Node", True);
  for (int i = 0; info && i < count; i++) {
    if (!info[i].enabled ||
        (info[i].use != XIFloatingSlave &&
         info[i].use != (IS_KEYBOARD ? XISlaveKeyboard : XISlavePointer)))
      continue;
    if (requested > 0 && info[i].deviceid != requested)
      continue;
    bool matches = false;
    if (node != None) {
      Atom type;
      int format;
      unsigned long n, remaining;
      unsigned char *value = NULL;
      if (XIGetProperty(d, info[i].deviceid, node, 0, 1024, False, XA_STRING,
                        &type, &format, &n, &remaining, &value) == Success &&
          type == XA_STRING && format == 8 && n && !remaining) {
        char *copy = calloc(n + 1, 1);
        if (copy) {
          memcpy(copy, value, n);
          matches = same_node(path, copy);
          free(copy);
        }
      }
      if (value)
        XFree(value);
    }
    /* Explicit IDs remain usable for drivers without a Device Node property. */
    if (matches || requested > 0) {
      if (chosen != -1) {
        chosen = -1;
        break;
      }
      chosen = info[i].deviceid;
    }
  }
  if (info)
    XIFreeDeviceInfo(info);
  return chosen;
}
static bool xy(const XIValuatorState *v, const double *values, double *x,
               double *y) {
  size_t index = 0;
  *x = *y = 0;
  if (!values)
    return false;
  for (int axis = 0; axis < v->mask_len * 8; axis++)
    if (XIMaskIsSet(v->mask, axis)) {
      if (axis == 0)
        *x = values[index];
      if (axis == 1)
        *y = values[index];
      index++;
    }
  return *x != 0 || *y != 0;
}
static bool push_sample(AInputSampleQueue *queue, AInputSample sample,
                        bool sequence, size_t *index, const char *source) {
  if (sequence) {
    AInputSample expected = ainput_sequence_sample(*index);
    if (sample.x != expected.x || sample.y != expected.y) {
      fprintf(stderr,
              "sequence_mismatch: source=%s index=%zu "
              "expected_x=%.17g expected_y=%.17g "
              "received_x=%.17g received_y=%.17g\n",
              source, *index, expected.x, expected.y, sample.x, sample.y);
      return false;
    }
    (*index)++;
  }
  if (!ainput_sample_push(queue, sample)) {
    fprintf(stderr, "queue_overflow: source=%s capacity=%d\n", source,
            AINPUT_MATCH_CAPACITY);
    return false;
  }
  return true;
}
static void usage(const char *name) {
  fprintf(stderr,
          "Usage: %s --event PATH [--device-id N] [--mode raw|%s] "
          "[--samples N] [--seconds N] [--warmup N] [--idle-gap-ms N] "
          "[--sequence]\n",
          name, COOKED_NAME);
}
int main(int argc, char **argv) {
  const char *path = NULL;
  long requested = -1, samples = 2000, seconds = 0, warmup = DEFAULT_WARMUP,
       idle_gap = 0;
  bool raw = true, sequence = false, samples_set = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--sequence")) {
      sequence = true;
      continue;
    }
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 2;
    }
    const char *option = argv[i++], *value = argv[i];
    if (!strcmp(option, "--event"))
      path = value;
    else if (!strcmp(option, "--mode")) {
      if (!strcmp(value, "raw"))
        raw = true;
      else if (!strcmp(value, COOKED_NAME))
        raw = false;
      else {
        usage(argv[0]);
        return 2;
      }
    } else {
      long *dest = NULL;
      if (!strcmp(option, "--device-id"))
        dest = &requested;
      else if (!strcmp(option, "--samples")) {
        dest = &samples;
        samples_set = true;
      } else if (!strcmp(option, "--seconds"))
        dest = &seconds;
      else if (!strcmp(option, "--warmup"))
        dest = &warmup;
      else if (!strcmp(option, "--idle-gap-ms"))
        dest = &idle_gap;
      if (!dest || !number(value, dest)) {
        usage(argv[0]);
        return 2;
      }
    }
  }
  if (!path || samples < 1 || samples > MAX_SAMPLES ||
      (sequence && (!raw || IS_KEYBOARD))) {
    usage(argv[0]);
    return 2;
  }
  if (seconds && !samples_set)
    samples = MAX_SAMPLES;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    perror("open evdev");
    return 1;
  }
  unsigned int clock_id = CLOCK_MONOTONIC;
  if (ioctl(fd, EVIOCSCLOCKID, &clock_id) < 0) {
    perror("EVIOCSCLOCKID");
    close(fd);
    return 1;
  }
  Display *d = XOpenDisplay(NULL);
  if (!d) {
    fprintf(stderr, "Cannot open display\n");
    close(fd);
    return 1;
  }
  int opcode, event, error, major = 2, minor = 0;
  if (!XQueryExtension(d, "XInputExtension", &opcode, &event, &error) ||
      XIQueryVersion(d, &major, &minor) != Success) {
    XCloseDisplay(d);
    close(fd);
    return 1;
  }
  int id = select_device(d, path, requested);
  if (id < 0) {
    fprintf(stderr, "Select one physical device with --device-id\n");
    XCloseDisplay(d);
    close(fd);
    return 1;
  }
  unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
  if (IS_KEYBOARD) {
    XISetMask(bits, raw ? XI_RawKeyPress : XI_KeyPress);
    XISetMask(bits, raw ? XI_RawKeyRelease : XI_KeyRelease);
  } else
    XISetMask(bits, raw ? XI_RawMotion : XI_Motion);
  XIEventMask mask = {.deviceid = id, .mask_len = sizeof(bits), .mask = bits};
  XISelectEvents(d, DefaultRootWindow(d), &mask, 1);
  XSync(d, False);
  /* Start with stationary input; controlled generator starts after this. */
  struct input_event batch[64];
  while (read(fd, batch, sizeof(batch)) > 0) {
  }
  while (XPending(d)) {
    XEvent e;
    XNextEvent(d, &e);
    if (e.type == GenericEvent && XGetEventData(d, &e.xcookie))
      XFreeEventData(d, &e.xcookie);
  }
  AInputSampleQueue hardware = {0}, observed = {0};
  Stats stats = {0}, idle_stats = {0};
  AInputStageSample *stages = calloc((size_t)samples, sizeof(*stages));
  if (!stages) {
    perror("calloc samples");
    XCloseDisplay(d);
    close(fd);
    return 1;
  }
  AInputSample keys[256];
  size_t nkeys = 0, hw_index = 0, xi_index = 0;
  int64_t dx = 0, dy = 0;
  double previous = 0, deadline = seconds ? now_ms() + seconds * 1000.0 : 0;
  bool valid = true;
  struct pollfd fds[2] = {{.fd = fd, .events = POLLIN},
                          {.fd = ConnectionNumber(d), .events = POLLIN}};
  printf(
      "device_id: %d\nassociation: %s\nWarmup: %ld matched samples discarded\n",
      id,
      sequence               ? "controlled-unique-sequence"
      : (raw || IS_KEYBOARD) ? "value-checked-estimate"
                             : "FIFO-motion-estimate",
      warmup);
  puts("Ready. Start input now.");
  fflush(stdout);
  while (valid && stats.count < (size_t)samples &&
         (!deadline || now_ms() < deadline)) {
    int rc = poll(fds, 2, XPending(d) ? 0 : 100);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      valid = false;
      break;
    }
    if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
      valid = false;
      break;
    }

    if (fds[0].revents & POLLIN)
      for (int budget = 0; budget < 8 && valid; budget++) {
        ssize_t len = read(fd, batch, sizeof(batch));
        if (len < 0 && errno == EINTR) {
          budget--;
          continue;
        }
        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        if (len <= 0 || (size_t)len % sizeof(batch[0])) {
          valid = false;
          break;
        }
        for (size_t i = 0; i < (size_t)len / sizeof(batch[0]) && valid; i++) {
          struct input_event *e = &batch[i];
          if (e->type == EV_SYN && e->code == SYN_DROPPED) {
            valid = false;
            break;
          }
          if (!IS_KEYBOARD && e->type == EV_REL &&
              (e->code == REL_X || e->code == REL_Y)) {
            int64_t *total = e->code == REL_X ? &dx : &dy;
            if (__builtin_add_overflow(*total, (int64_t)e->value, total))
              valid = false;
          }
          if (IS_KEYBOARD && e->type == EV_KEY && e->code <= 247 &&
              (e->value == 0 || e->value == 1)) {
            if (nkeys == 256) {
              valid = false;
              break;
            }
            keys[nkeys++] = (AInputSample){.x = e->code + 8, .y = e->value};
          }
          if (e->type == EV_SYN && e->code == SYN_REPORT) {
            double ts = event_ms(e);
            bool idle =
                idle_gap > 0 && previous > 0 && ts - previous >= idle_gap;
            if (IS_KEYBOARD) {
              for (size_t k = 0; k < nkeys; k++) {
                keys[k].time = ts;
                keys[k].after_idle = idle;
                if (!ainput_sample_push(&hardware, keys[k]))
                  valid = false;
              }
              if (nkeys)
                previous = ts;
              nkeys = 0;
            } else if (dx || dy) {
              AInputSample sample = {.time = ts,
                                     .x = (double)dx,
                                     .y = (double)dy,
                                     .after_idle = idle};
              if (!push_sample(&hardware, sample, sequence, &hw_index, "evdev"))
                valid = false;
              previous = ts;
            }
            dx = dy = 0;
          }
        }
        if (len < (ssize_t)sizeof(batch))
          break;
      }
    while (valid && XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      double received = now_ms();
      if (e.type != GenericEvent || e.xcookie.extension != opcode ||
          !XGetEventData(d, &e.xcookie))
        continue;
      AInputSample sample = {.time = received};
      bool relevant = false;
      int type = e.xcookie.evtype;
      if (raw && ((!IS_KEYBOARD && type == XI_RawMotion) ||
                  (IS_KEYBOARD &&
                   (type == XI_RawKeyPress || type == XI_RawKeyRelease)))) {
        XIRawEvent *r = e.xcookie.data;
        if (r->deviceid == id && r->sourceid == id &&
            (!IS_KEYBOARD || !(r->flags & XIKeyRepeat))) {
          sample.server_time = r->time;
          if (IS_KEYBOARD) {
            sample.x = r->detail;
            sample.y = type == XI_RawKeyPress;
            relevant = true;
          } else
            relevant = xy(&r->valuators, r->raw_values, &sample.x, &sample.y);
        }
      } else if (!raw && ((!IS_KEYBOARD && type == XI_Motion) ||
                          (IS_KEYBOARD &&
                           (type == XI_KeyPress || type == XI_KeyRelease)))) {
        XIDeviceEvent *r = e.xcookie.data;
        if (r->deviceid == id && r->sourceid == id &&
            (!IS_KEYBOARD || !(r->flags & XIKeyRepeat))) {
          sample.server_time = r->time;
          if (IS_KEYBOARD) {
            sample.x = r->detail;
            sample.y = type == XI_KeyPress;
            relevant = true;
          } else
            relevant =
                xy(&r->valuators, r->valuators.values, &sample.x, &sample.y);
        }
      }
      if (relevant &&
          !push_sample(&observed, sample, sequence, &xi_index, "xi2"))
        valid = false;
      XFreeEventData(d, &e.xcookie);
    }
    while (valid && stats.count < (size_t)samples) {
      double latency;
      bool idle;
      double hardware_time, observed_time;
      uint32_t server_time;
      int matched = ainput_sample_match(
          &hardware, &observed, raw || IS_KEYBOARD, &latency, &idle,
          &hardware_time, &observed_time, &server_time);
      if (matched < 0) {
        AInputSample hw = hardware.items[hardware.head];
        AInputSample xi = observed.items[observed.head];
        if (xi.time < hw.time)
          fprintf(stderr,
                  "clock_order_error: evdev_ms=%.6f xi_receive_ms=%.6f\n",
                  hw.time, xi.time);
        else
          fprintf(stderr,
                  "coordinate_or_order_mismatch: "
                  "evdev_x=%.17g evdev_y=%.17g "
                  "xi_x=%.17g xi_y=%.17g\n",
                  hw.x, hw.y, xi.x, xi.y);
        valid = false;
        break;
      }
      if (!matched)
        break;
      if (warmup)
        warmup--;
      else {
        stats.after_idle[stats.count] = idle;
        stats.values[stats.count] = latency;
        stages[stats.count] =
            (AInputStageSample){hardware_time, observed_time, server_time};
        stats.count++;
        if (idle)
          idle_stats.values[idle_stats.count++] = latency;
      }
    }
    /* A missing counterpart cannot silently hang a samples-only run. */
    double now = now_ms();
    if (hardware.count && now - hardware.items[hardware.head].time > 1000) {
      fprintf(stderr,
              "missing_counterpart: source=xi2 evdev_queued=%zu "
              "xi_queued=%zu oldest_age_ms=%.3f\n",
              hardware.count, observed.count,
              now - hardware.items[hardware.head].time);
      valid = false;
    } else if (observed.count &&
               now - observed.items[observed.head].time > 1000) {
      fprintf(stderr,
              "missing_counterpart: source=evdev evdev_queued=%zu "
              "xi_queued=%zu oldest_age_ms=%.3f\n",
              hardware.count, observed.count,
              now - observed.items[observed.head].time);
      valid = false;
    }
  }
  if (stats.count < (size_t)samples &&
      (hardware.count || observed.count || nkeys || dx || dy))
    valid = false;
  if (!stats.count)
    valid = false;
  if (valid) {
    puts("measurement_valid: yes");
    save_samples_prompt(&stats, stages);
    print_stats(&stats, "");
    if (idle_gap)
      print_stats(&idle_stats, "idle_");
    printf("unmatched_evdev_queued: %zu\nunmatched_xi_queued: "
           "%zu\nqueue_overflow: 0\nsample_overflow: 0\n",
           hardware.count, observed.count);
    if (!sequence)
      puts("Qualification: estimate; repeated signatures or motion filtering "
           "can hide loss. Do not use alone to establish small performance "
           "gains.");
  } else
    fprintf(stderr,
            "measurement_valid: no (loss, mismatch, clock/order error, "
            "overflow, disconnect or no samples); percentiles suppressed\n");
  free(stages);
  XCloseDisplay(d);
  close(fd);
  return valid ? 0 : 1;
}
