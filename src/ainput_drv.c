/*
 * AInput - minimal Xorg/XLibre input driver.
 *
 * This driver intentionally keeps the event path small:
 * Linux evdev -> AInput -> Xorg/XLibre input queue.
 *
 * It is not a full libinput replacement. Touchpads, tablets, gestures,
 * acceleration profiles, and advanced device quirks are outside its current
 * scope.
 */
#ifndef DRIVER_NAME
#define DRIVER_NAME "ainput"
#endif

#include <linux/input-event-codes.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <poll.h>
#include <time.h>
#ifdef AINPUT_IO_URING
#include <liburing.h>
#endif

/*
 * xorg-server.h must be included before X11 protocol headers so server-side
 * ABI definitions such as Atom layout are correct.
 */
#include <xorg/xorg-server.h>

#include <X11/X.h>
#include <X11/Xdefs.h>
#include <X11/Xatom.h>

#ifndef XA_FLOAT
#define XA_FLOAT MakeAtom("FLOAT", 5, TRUE)
#endif

#include <xorg/xf86.h>
#include <xorg/xf86Opt.h>
#include <xorg/xf86Xinput.h>
#include <xorg/xisb.h>
#include <xorg/exevents.h>
#include <xorg/xf86_OSproc.h>
#include <xorg/os.h>

#include <linux/input.h>

/*
 * Experimental Xorg/XLibre direct keyboard and relative-motion paths.
 *
 * These only work with patched Xorg/XLibre servers that export the matching
 * symbols. Keep them disabled for normal builds.
 *
 * AnbryInput-specific direct paths:
 *   make XSERVER_DIRECT=1
 */
#ifdef AINPUT_XSERVER_DIRECT
extern void QueueAInputKeyAtTime(DeviceIntPtr pDev, int keycode, int is_down,
                                 CARD32 ms);
extern void QueueAInputRelativeMotion2DRawAtTime(DeviceIntPtr pDev,
                                                 double dx, double dy,
                                                 double raw_dx, double raw_dy,
                                                 CARD32 ms);
extern void QueueAInputButtonAtTime(DeviceIntPtr pDev, int button, int is_down,
                                    CARD32 ms);
#endif

#define DRIVER_VERSION 1
#define AINPUT_VERSION_MAJOR 1
#define AINPUT_VERSION_MINOR 9
#define AINPUT_VERSION_PATCH 0

#define PROP_SENSITIVITY "AInput Sensitivity"
#define AINPUT_EVENT_BATCH 256
#define AINPUT_PENDING_EVENT_CAPACITY 256
#define AINPUT_MAX_SCROLL_STEPS_PER_REPORT 64
#define AINPUT_DEFAULT_READ_BUDGET 1
#define AINPUT_URING_DEPTH 32
#define AINPUT_URING_ONESHOT_BUFFERS 2
#define AINPUT_URING_MULTISHOT_BUFFERS 32
#define AINPUT_URING_MULTISHOT_GROUP 1
#define AINPUT_URING_DEFAULT_SQPOLL_IDLE 50
#define AINPUT_URING_RECONNECT_INTERVAL_MS 1000
#define AINPUT_DEFAULT_SENSITIVITY 1.0f
#define AINPUT_DEFAULT_DPI 1000.0f
#define AINPUT_DEFAULT_LAYOUT "us"
#define AINPUT_BUTTON_COUNT 32
#define AINPUT_FIRST_AUX_BUTTON 8
#define AINPUT_LAST_AUX_BUTTON \
    (AINPUT_FIRST_AUX_BUTTON + BTN_JOYSTICK - BTN_SIDE - 1)

/* ------------------------------------------------------------------ */
/* evdev bit helpers                                                  */
/* ------------------------------------------------------------------ */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((x) / BITS_PER_LONG + 1)
#define BIT_IS_SET(arr, bit) \
    (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)
#define BIT_SET(arr, bit) \
    ((arr)[(bit) / BITS_PER_LONG] |= 1UL << ((bit) % BITS_PER_LONG))
#define BIT_CLEAR(arr, bit) \
    ((arr)[(bit) / BITS_PER_LONG] &= ~(1UL << ((bit) % BITS_PER_LONG)))

typedef enum
{
    DEV_KEYBOARD,
    DEV_MOUSE
} ADevType;

typedef enum {
    AINPUT_PENDING_KEY,
    AINPUT_PENDING_BUTTON,
    AINPUT_PENDING_SCROLL_V,
    AINPUT_PENDING_SCROLL_H,
    AINPUT_PENDING_SCROLL_V_HI_RES,
    AINPUT_PENDING_SCROLL_H_HI_RES,
} AInputPendingType;

typedef struct {
    int value;
    unsigned short code;
    unsigned char type;
    unsigned char is_down;
} AInputPendingEvent;
_Static_assert(AINPUT_BUTTON_COUNT < MAX_BUTTONS,
               "button count must fit the X server button map");
_Static_assert(AINPUT_LAST_AUX_BUTTON <= AINPUT_BUTTON_COUNT,
               "evdev mouse buttons must fit the advertised X button map");

/* Buttons 4-7 are reserved for vertical and horizontal wheel clicks. */
static const int ainput_tracked_mouse_buttons[] = {
    1, 2, 3,
    8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
};

typedef struct
{
    /* Keep state touched for every relative report at the front. */
    ValuatorMask *motion_mask;
    double effective_sensitivity;
    int64_t acc_x, acc_y;
    unsigned int read_budget;
    int sync_dropped;
    int read_registered;
    int last_read_error;
    int deferred_read_error;
    int use_io_uring;
    int has_wheel_hi_res;
    int has_hwheel_hi_res;
    ADevType type;
    unsigned int pending_event_count;
    int is_absolute;

    int64_t wheel_v_hi_res, wheel_h_hi_res;
    int has_abs_event;
    unsigned int resync_query_failures;
    unsigned int pending_overflows;
    unsigned int scroll_clamps;

    AInputPendingEvent pending_events[AINPUT_PENDING_EVENT_CAPACITY];

    unsigned long key_state[NBITS(KEY_MAX)];

    int abs_x, abs_y;
    int min_x, max_x;
    int min_y, max_y;
    int has_last_abs; /* Per-axis baseline bits: X=1, Y=2. */
    int last_abs_x, last_abs_y;

    const char *xkb_layout, *xkb_variant;
    float sensitivity, dpi, reference_dpi;
    Atom prop_sensitivity;
    int fd, initializing_property;
    OsTimerPtr reconnect_timer;
    OsTimerPtr resume_timer;
    unsigned int reconnect_attempts;

#ifdef AINPUT_IO_URING
    /* The evdev fd stays private; Xorg watches the pollable ring fd. */
    struct io_uring ring;
    int ring_ready;
    int shutdown_failed;
    int watch_fd;
    int chain_inflight;
    int linked_fallback;
    int fixed_file;
    int fixed_buffers;
    struct io_uring_buf_ring *buffer_ring;
    struct input_event (*multishot_buffers)[AINPUT_EVENT_BATCH];
    int multishot_requested;
    int multishot_active;
    int held_buffer;
    int sqpoll_requested;
    int sqpoll_active;
    int sqpoll_idle;
    int sqpoll_cpu;
    int uring_debug;
    uint64_t uring_submit_calls;
    uint64_t uring_sqes_submitted;
    uint64_t uring_cqes_seen;
    uint64_t uring_bytes;
    uint64_t uring_rearms;
    uint64_t uring_eagain;
    uint64_t uring_enobufs;
    uint64_t uring_errors;
    uint64_t uring_cancel_calls;
    unsigned int uring_max_cq_ready;
    unsigned int uring_kernel_overflow;
#endif

    /* io_uring alternates buffers so rearming cannot race event parsing. */
    struct input_event event_buffer[AINPUT_URING_ONESHOT_BUFFERS]
                                   [AINPUT_EVENT_BATCH];
} AInputPriv;

static void ainput_register_ring_watch(InputInfoPtr pInfo);
static void ainput_unregister_ring_watch(InputInfoPtr pInfo);
static void ainput_uring_stop(InputInfoPtr pInfo);

static inline void ainput_accumulate(int64_t *total, int value)
{
#if defined(__GNUC__) || defined(__clang__)
    int64_t result;

    if (__builtin_add_overflow(*total, (int64_t)value, &result))
        result = value > 0 ? INT64_MAX : INT64_MIN;
    *total = result;
#else
    if (value > 0 && *total > INT64_MAX - (int64_t)value)
        *total = INT64_MAX;
    else if (value < 0 && *total < INT64_MIN - (int64_t)value)
        *total = INT64_MIN;
    else
        *total += (int64_t)value;
#endif
}

static inline int ainput_fd_is_server_managed(const InputInfoPtr pInfo)
{
    return pInfo && (pInfo->flags & XI86_SERVER_FD);
}

static int ainput_ioctl_retry(int fd, unsigned long request, void *data)
{
    int result;

    do
        result = ioctl(fd, request, data);
    while (result < 0 && errno == EINTR);

    return result;
}

static inline CARD32 ainput_server_timestamp(void)
{
    return GetTimeInMillis();
}

static inline void ainput_update_effective_sensitivity(AInputPriv *priv)
{
    if (priv->is_absolute || priv->dpi <= 0.0f || priv->reference_dpi <= 0.0f)
        priv->effective_sensitivity = (double)priv->sensitivity;
    else
        priv->effective_sensitivity = (double)priv->sensitivity * ((double)priv->reference_dpi / (double)priv->dpi);
}

static void ainput_apply_sensitivity(AInputPriv *priv, float new_sens)
{
    priv->sensitivity = new_sens;
    ainput_update_effective_sensitivity(priv);
}

static inline void ainput_post_relative_motion(DeviceIntPtr dev,
                                               ValuatorMask *mask,
                                               double dx, double dy,
                                               double raw_dx, double raw_dy,
                                               CARD32 ms)
{
#ifdef AINPUT_XSERVER_DIRECT
    /* The server helper builds its fixed two-axis mask without allocation. */
    (void)mask;
    QueueAInputRelativeMotion2DRawAtTime(dev, dx, dy, raw_dx, raw_dy, ms);
#else
    (void)ms;
    valuator_mask_set_unaccelerated(mask, 0, dx, raw_dx);
    valuator_mask_set_unaccelerated(mask, 1, dy, raw_dy);

    QueuePointerEvents(dev, MotionNotify, 0, POINTER_RELATIVE, mask);
#endif
}

static inline void ainput_post_button(InputInfoPtr pInfo, int button,
                                      int is_down, CARD32 ms)
{
#ifdef AINPUT_XSERVER_DIRECT
    QueueAInputButtonAtTime(pInfo->dev, button, is_down, ms);
#else
    (void)ms;
    QueuePointerEvents(pInfo->dev, is_down ? ButtonPress : ButtonRelease,
                       button, 0, NULL);
#endif
}

static inline void
ainput_post_scroll_steps(InputInfoPtr pInfo, int64_t value, int horizontal,
                         CARD32 ms)
{
    AInputPriv *priv = pInfo->private;
    int button;
    uint64_t steps;

    if (value == 0)
        return;

    button = horizontal ? (value > 0 ? 7 : 6) : (value > 0 ? 4 : 5);
    steps = value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;

    /*
     * Real wheels produce small counts. Bound hostile or corrupted uinput
     * reports so one value cannot monopolize Xorg's serialized input thread.
     */
    if (steps > AINPUT_MAX_SCROLL_STEPS_PER_REPORT)
    {
        priv->scroll_clamps++;
        if (priv->scroll_clamps == 1 ||
            (priv->scroll_clamps & (priv->scroll_clamps - 1U)) == 0)
            xf86Msg(X_WARNING,
                    "%s: scroll report exceeds %u steps; clamping "
                    "to protect the input thread (clamp %u)\n",
                    pInfo->name, AINPUT_MAX_SCROLL_STEPS_PER_REPORT,
                    priv->scroll_clamps);
        steps = AINPUT_MAX_SCROLL_STEPS_PER_REPORT;
    }

    while (steps--) {
        ainput_post_button(pInfo, button, 1, ms);
        ainput_post_button(pInfo, button, 0, ms);
    }
}

static inline void
ainput_post_hi_res_scroll(InputInfoPtr pInfo, int64_t *remainder,
                          int64_t value, int horizontal, CARD32 ms)
{
    int64_t total;
    int64_t steps;

#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_add_overflow(*remainder, value, &total))
        total = value > 0 ? INT64_MAX : INT64_MIN;
#else
    total = *remainder;
    if (value > 0 && total > INT64_MAX - value)
        total = INT64_MAX;
    else if (value < 0 && total < INT64_MIN - value)
        total = INT64_MIN;
    else
        total += value;
#endif

    steps = total / 120;
    *remainder = total % 120;
    ainput_post_scroll_steps(pInfo, steps, horizontal, ms);
}

static inline void ainput_post_key(InputInfoPtr pInfo, int key_code, int is_down,
                                   int hardware_event, CARD32 ms)
{
#ifdef AINPUT_XSERVER_DIRECT
    if (hardware_event) {
        QueueAInputKeyAtTime(pInfo->dev, key_code, is_down, ms);
        return;
    }
#else
    (void)hardware_event;
#endif
    (void)ms;
    QueueKeyboardEvents(pInfo->dev, is_down ? KeyPress : KeyRelease, key_code);
}

static inline void ainput_track_key(AInputPriv *priv, unsigned int code, int is_down)
{
    if (code > KEY_MAX)
        return;

    if (is_down)
        BIT_SET(priv->key_state, code);
    else
        BIT_CLEAR(priv->key_state, code);
}

static int ainput_mouse_button_for_code(unsigned int code)
{
    switch (code)
    {
    case BTN_LEFT:   return 1;
    case BTN_MIDDLE: return 2;
    case BTN_RIGHT:  return 3;
    case BTN_TOUCH:  return 1;
    case BTN_STYLUS: return 2;
    case BTN_STYLUS2: return 3;
    default:
        if (code >= (unsigned int)BTN_SIDE &&
            code < (unsigned int)BTN_JOYSTICK)
            return AINPUT_FIRST_AUX_BUTTON +
                   (int)(code - (unsigned int)BTN_SIDE);
        return 0;
    }
}

static int ainput_mouse_button_is_down(const unsigned long state[NBITS(KEY_MAX)],
                                       int button)
{
    switch (button)
    {
    case 1: return BIT_IS_SET(state, BTN_LEFT) || BIT_IS_SET(state, BTN_TOUCH);
    case 2: return BIT_IS_SET(state, BTN_MIDDLE) ||
                   BIT_IS_SET(state, BTN_STYLUS);
    case 3: return BIT_IS_SET(state, BTN_RIGHT) ||
                   BIT_IS_SET(state, BTN_STYLUS2);
    default:
        if (button >= AINPUT_FIRST_AUX_BUTTON &&
            button <= AINPUT_LAST_AUX_BUTTON)
        {
            unsigned int code = (unsigned int)BTN_SIDE +
                                (unsigned int)(button -
                                               AINPUT_FIRST_AUX_BUTTON);
            return BIT_IS_SET(state, code);
        }
        return 0;
    }
}

static int ainput_queue_pending_event(InputInfoPtr pInfo,
                                      AInputPendingType type,
                                      unsigned int code, int value)
{
    AInputPriv *priv = pInfo->private;
    if (priv->pending_event_count == AINPUT_PENDING_EVENT_CAPACITY)
    {
        priv->pending_overflows++;
        if (priv->pending_overflows == 1 ||
            (priv->pending_overflows & (priv->pending_overflows - 1U)) == 0)
            xf86Msg(X_WARNING,
                    "%s: too many discrete events in one evdev report; "
                    "discarding and resynchronizing (overflow %u)\n",
                    pInfo->name, priv->pending_overflows);
        return 0;
    }

    priv->pending_events[priv->pending_event_count++] = (AInputPendingEvent) {
        .value = value,
        .code = (unsigned short)code,
        .type = (unsigned char)type,
        .is_down = value != 0,
    };
    return 1;
}

static void ainput_flush_pending_keys(InputInfoPtr pInfo, CARD32 report_ms)
{
    AInputPriv *priv = pInfo->private;
    for (unsigned int i = 0; i < priv->pending_event_count; i++) {
        AInputPendingEvent pending = priv->pending_events[i];
        unsigned int code = pending.code;
        int is_down = pending.is_down;

        if (BIT_IS_SET(priv->key_state, code) != (unsigned long)is_down) {
            ainput_post_key(pInfo, (int)code + 8, is_down, 1, report_ms);
            ainput_track_key(priv, code, is_down);
        }
    }
    priv->pending_event_count = 0;
}

static void ainput_flush_pending_pointer(InputInfoPtr pInfo, CARD32 report_ms)
{
    AInputPriv *priv = pInfo->private;
    for (unsigned int i = 0; i < priv->pending_event_count; i++) {
        AInputPendingEvent pending = priv->pending_events[i];
        if (pending.type == AINPUT_PENDING_BUTTON) {
            int button = ainput_mouse_button_for_code(pending.code);
            int was_down = ainput_mouse_button_is_down(priv->key_state,
                                                        button);
            ainput_track_key(priv, pending.code, pending.is_down);
            if (was_down != ainput_mouse_button_is_down(priv->key_state,
                                                         button))
                ainput_post_button(pInfo, button, !was_down, report_ms);
        }
        else if (pending.type == AINPUT_PENDING_SCROLL_V)
            ainput_post_scroll_steps(pInfo, pending.value, 0, report_ms);
        else if (pending.type == AINPUT_PENDING_SCROLL_H)
            ainput_post_scroll_steps(pInfo, pending.value, 1, report_ms);
        else if (pending.type == AINPUT_PENDING_SCROLL_V_HI_RES)
            ainput_post_hi_res_scroll(pInfo, &priv->wheel_v_hi_res,
                                      pending.value, 0, report_ms);
        else if (pending.type == AINPUT_PENDING_SCROLL_H_HI_RES)
            ainput_post_hi_res_scroll(pInfo, &priv->wheel_h_hi_res,
                                      pending.value, 1, report_ms);
    }
    priv->pending_event_count = 0;
}

static void ainput_begin_resync(AInputPriv *priv)
{
    priv->sync_dropped = 1;
    priv->acc_x = 0;
    priv->acc_y = 0;
    priv->wheel_v_hi_res = 0;
    priv->wheel_h_hi_res = 0;
    priv->has_abs_event = 0;
    priv->has_last_abs = 0;
    priv->pending_event_count = 0;
}

static void ainput_release_tracked_state(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;

    if (priv->type == DEV_KEYBOARD)
    {
        for (unsigned int code = 0; code <= KEY_MAX && code + 8 <= 255; code++)
            if (BIT_IS_SET(priv->key_state, code))
                ainput_post_key(pInfo, (int)code + 8, 0, 0,
                                GetTimeInMillis());
    }
    else
    {
        for (size_t i = 0;
             i < sizeof(ainput_tracked_mouse_buttons) /
                 sizeof(ainput_tracked_mouse_buttons[0]);
             i++)
        {
            int button = ainput_tracked_mouse_buttons[i];

            if (ainput_mouse_button_is_down(priv->key_state, button))
                ainput_post_button(pInfo, button, 0, GetTimeInMillis());
        }
    }

    memset(priv->key_state, 0, sizeof(priv->key_state));
}

static void ainput_resync_state(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    unsigned long current[NBITS(KEY_MAX)] = {0};
    int result;

    result = ainput_ioctl_retry(priv->fd, EVIOCGKEY(sizeof(current)), current);

    if (result < 0)
    {
        int error = errno;

        priv->resync_query_failures++;
        if (priv->resync_query_failures == 1 ||
            (priv->resync_query_failures & 255U) == 0)
            xf86Msg(X_WARNING,
                    "%s: state query after SYN_DROPPED failed "
                    "(errno=%d: %s); releasing tracked keys/buttons and "
                    "resuming input (failure %u)\n",
                    pInfo->name, error, strerror(error),
                    priv->resync_query_failures);

        ainput_release_tracked_state(pInfo);
        priv->has_last_abs = 0;
        priv->sync_dropped = 0;
        return;
    }

    if (priv->type == DEV_KEYBOARD)
    {
        for (unsigned int code = 0; code <= KEY_MAX && code + 8 <= 255; code++)
        {
            int was_down = BIT_IS_SET(priv->key_state, code);
            int is_down = BIT_IS_SET(current, code);

            if (was_down != is_down)
                ainput_post_key(pInfo, (int)code + 8, is_down, 0,
                                GetTimeInMillis());
        }
    }
    else
    {
        for (size_t i = 0;
             i < sizeof(ainput_tracked_mouse_buttons) /
                 sizeof(ainput_tracked_mouse_buttons[0]);
             i++)
        {
            int button = ainput_tracked_mouse_buttons[i];
            int was_down = ainput_mouse_button_is_down(priv->key_state, button);
            int is_down = ainput_mouse_button_is_down(current, button);

            if (was_down != is_down)
                ainput_post_button(pInfo, button, is_down,
                                   GetTimeInMillis());
        }
    }

    memcpy(priv->key_state, current, sizeof(priv->key_state));

    if (priv->is_absolute)
    {
        struct input_absinfo abs_x;
        struct input_absinfo abs_y;
        int x_result;
        int y_result;

        x_result = ainput_ioctl_retry(priv->fd, EVIOCGABS(ABS_X), &abs_x);
        y_result = ainput_ioctl_retry(priv->fd, EVIOCGABS(ABS_Y), &abs_y);

        if (x_result >= 0 && y_result >= 0)
        {
            priv->abs_x = priv->last_abs_x = abs_x.value;
            priv->abs_y = priv->last_abs_y = abs_y.value;
            priv->has_last_abs = 3;
        }
    }

    priv->resync_query_failures = 0;
    priv->sync_dropped = 0;
}

static int ainput_change_property(DeviceIntPtr dev, Atom property, XIPropertyValuePtr val, BOOL checkonly)
{
    InputInfoPtr pInfo;
    AInputPriv *priv;
    float new_sens;

    if (!dev)
        return BadValue;

    pInfo = dev->public.devicePrivate;
    if (!pInfo || !pInfo->private)
        return BadValue;

    priv = pInfo->private;

    if (property != priv->prop_sensitivity)
        return Success;

    if (!val || !val->data)
        return BadValue;

    if (val->type != XA_FLOAT || val->format != 32 || val->size != 1)
    {
        xf86Msg(
            X_WARNING,
            "AINPUT [%s]: invalid sensitivity property type=%lu format=%d size=%ld\n",
            pInfo->name,
            (unsigned long)val->type,
            val->format,
            val->size);
        return BadMatch;
    }

    memcpy(&new_sens, val->data, sizeof(float));

    if (!isfinite(new_sens) || new_sens <= 0.0f || new_sens > 100000.0f)
        return BadValue;

    if (checkonly || priv->initializing_property)
        return Success;

    ainput_apply_sensitivity(priv, new_sens);

    xf86Msg(
        X_INFO,
        "AINPUT [%s]: sensitivity = %.3f effective = %.3f dpi = %.1f reference_dpi = %.1f\n",
        pInfo->name,
        new_sens,
        priv->effective_sensitivity,
        priv->dpi,
        priv->reference_dpi);

    return Success;
}

#ifdef AINPUT_IO_URING
enum {
    AINPUT_URING_POLL = 1,
    AINPUT_URING_READ_0 = 2,
    AINPUT_URING_READ_1 = 3,
    AINPUT_URING_READ_MULTISHOT = 4,
};

static const char *ainput_uring_cqe_name(uint64_t type)
{
    switch (type) {
    case AINPUT_URING_POLL: return "poll";
    case AINPUT_URING_READ_0: return "read-0";
    case AINPUT_URING_READ_1: return "read-1";
    case AINPUT_URING_READ_MULTISHOT: return "multishot";
    default: return "unknown";
    }
}

static void ainput_uring_debug_state(InputInfoPtr pInfo, const char *phase)
{
    AInputPriv *priv = pInfo->private;
    unsigned int sq_ready = 0;
    unsigned int cq_ready = 0;

    if (!priv->uring_debug)
        return;
    if (priv->ring_ready) {
        sq_ready = io_uring_sq_ready(&priv->ring);
        cq_ready = io_uring_cq_ready(&priv->ring);
    }
    xf86Msg(X_INFO,
            "%s: io_uring debug %s: ring=%s watch=%s fd=%d ring_fd=%d "
            "sq_ready=%u cq_ready=%u chain=%d multishot=%d held=%d "
            "submit_calls=%llu sqes=%llu cqes=%llu bytes=%llu rearms=%llu "
            "eagain=%llu enobufs=%llu errors=%llu cancels=%llu max_cq=%u "
            "kernel_overflow=%u\n",
            pInfo->name, phase,
            priv->ring_ready ? "ready" : "off",
            priv->read_registered ? "registered" : "off",
            priv->fd, priv->ring_ready ? priv->ring.ring_fd : -1,
            sq_ready, cq_ready, priv->chain_inflight,
            priv->multishot_active, priv->held_buffer,
            (unsigned long long)priv->uring_submit_calls,
            (unsigned long long)priv->uring_sqes_submitted,
            (unsigned long long)priv->uring_cqes_seen,
            (unsigned long long)priv->uring_bytes,
            (unsigned long long)priv->uring_rearms,
            (unsigned long long)priv->uring_eagain,
            (unsigned long long)priv->uring_enobufs,
            (unsigned long long)priv->uring_errors,
            (unsigned long long)priv->uring_cancel_calls,
            priv->uring_max_cq_ready, priv->uring_kernel_overflow);
}

static void ainput_uring_debug_cqe(InputInfoPtr pInfo, uint64_t type,
                                   int result, unsigned int flags)
{
    AInputPriv *priv = pInfo->private;
    uint64_t count = priv->uring_cqes_seen;

    if (!priv->uring_debug)
        return;
    if (result >= 0 && count > 16 && (count & (count - 1)) != 0)
        return;
    xf86Msg(result < 0 ? X_WARNING : X_INFO,
            "%s: io_uring debug CQE #%llu type=%s(%llu) res=%d flags=0x%x "
            "more=%s buffer=%s bid=%u\n",
            pInfo->name, (unsigned long long)count,
            ainput_uring_cqe_name(type), (unsigned long long)type,
            result, flags,
            (flags & IORING_CQE_F_MORE) ? "yes" : "no",
            (flags & IORING_CQE_F_BUFFER) ? "yes" : "no",
            flags >> IORING_CQE_BUFFER_SHIFT);
}

static int ainput_uring_submit_oneshot(InputInfoPtr pInfo,
                                       unsigned int buffer_index)
{
    AInputPriv *priv = pInfo->private;
    struct io_uring_sqe *poll_sqe = NULL;
    struct io_uring_sqe *read_sqe;
    int fd = priv->fixed_file ? 0 : priv->fd;
    unsigned int submissions = priv->linked_fallback ? 2U : 1U;
    int result;

    if (priv->chain_inflight)
        return -EBUSY;
    if (io_uring_sq_space_left(&priv->ring) < submissions)
        return -ENOSPC;

    if (priv->linked_fallback)
        poll_sqe = io_uring_get_sqe(&priv->ring);
    read_sqe = io_uring_get_sqe(&priv->ring);
    if ((priv->linked_fallback && !poll_sqe) || !read_sqe)
        return -ENOSPC;

    if (poll_sqe) {
        io_uring_prep_poll_add(poll_sqe, fd, POLLIN);
        poll_sqe->user_data = AINPUT_URING_POLL;
        poll_sqe->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
        if (priv->fixed_file)
            poll_sqe->flags |= IOSQE_FIXED_FILE;
    }

    if (priv->fixed_buffers)
        io_uring_prep_read_fixed(read_sqe, fd,
                                 priv->event_buffer[buffer_index],
                                 sizeof(priv->event_buffer[buffer_index]),
                                 UINT64_MAX, (int)buffer_index);
    else
        io_uring_prep_read(read_sqe, fd, priv->event_buffer[buffer_index],
                           sizeof(priv->event_buffer[buffer_index]),
                           UINT64_MAX);
    read_sqe->user_data = AINPUT_URING_READ_0 + buffer_index;
    if (priv->fixed_file)
        read_sqe->flags |= IOSQE_FIXED_FILE;

    result = io_uring_submit(&priv->ring);
    if (result < 0) {
        priv->uring_errors++;
        if (priv->uring_debug)
            xf86Msg(X_WARNING,
                    "%s: io_uring debug one-shot submit failed: %s\n",
                    pInfo->name, strerror(-result));
        return result;
    }
    priv->uring_submit_calls++;
    priv->uring_sqes_submitted += (unsigned int)result;
    if (priv->chain_inflight == 0 && priv->uring_submit_calls > 1)
        priv->uring_rearms++;

    if (!priv->sqpoll_active && (unsigned int)result != submissions) {
        xf86Msg(X_WARNING,
                "%s: io_uring_submit returned %d for %u one-shot SQEs "
                "(sqpoll=%s)\n",
                pInfo->name, result, submissions,
                priv->sqpoll_active ? "yes" : "no");
        return -EIO;
    }
    priv->chain_inflight = 1;
    return Success;
}

static int ainput_uring_submit_multishot(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct io_uring_sqe *sqe;
    int fd = priv->fixed_file ? 0 : priv->fd;
    int result;

    if (priv->multishot_active)
        return -EBUSY;
    if (io_uring_sq_space_left(&priv->ring) < 1)
        return -ENOSPC;

    sqe = io_uring_get_sqe(&priv->ring);
    if (!sqe)
        return -ENOSPC;
    io_uring_prep_read_multishot(sqe, fd, 0, UINT64_MAX,
                                 AINPUT_URING_MULTISHOT_GROUP);
    sqe->user_data = AINPUT_URING_READ_MULTISHOT;
    if (priv->fixed_file)
        sqe->flags |= IOSQE_FIXED_FILE;

    result = io_uring_submit(&priv->ring);
    if (result < 0) {
        priv->uring_errors++;
        if (priv->uring_debug)
            xf86Msg(X_WARNING,
                    "%s: io_uring debug multishot submit failed: %s\n",
                    pInfo->name, strerror(-result));
        return result;
    }
    priv->uring_submit_calls++;
    priv->uring_sqes_submitted += (unsigned int)result;
    if (priv->uring_submit_calls > 1)
        priv->uring_rearms++;
    if (!priv->sqpoll_active && result != 1) {
        xf86Msg(X_WARNING,
                "%s: io_uring_submit returned %d for one multishot SQE "
                "(sqpoll=%s)\n",
                pInfo->name, result,
                priv->sqpoll_active ? "yes" : "no");
        return -EIO;
    }
    priv->multishot_active = 1;
    return Success;
}

static inline void ainput_uring_recycle_buffer(AInputPriv *priv)
{
    int bid = priv->held_buffer;
    int mask;

    if (bid < 0 || !priv->buffer_ring)
        return;

    mask = io_uring_buf_ring_mask(AINPUT_URING_MULTISHOT_BUFFERS);
    io_uring_buf_ring_add(
        priv->buffer_ring, priv->multishot_buffers[bid],
        sizeof(priv->event_buffer[0]), (unsigned short)bid, mask, 0);
    io_uring_buf_ring_advance(priv->buffer_ring, 1);
    priv->held_buffer = -1;
}

static void ainput_uring_stop(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct io_uring_buf_ring *buffer_ring = priv->buffer_ring;
    int free_result = 0;

    ainput_uring_debug_state(pInfo, "stop-begin");
    if (priv->ring_ready) {
        int result = 0;
        for (unsigned int attempt = 0; io_uring_sq_ready(&priv->ring); attempt++) {
            if (attempt == 100) { result = -ETIMEDOUT; break; }
            result = io_uring_submit(&priv->ring);
            if (result < 0 && result != -EINTR)
                break;
            if (io_uring_sq_ready(&priv->ring)) {
                struct timespec pause = { .tv_nsec = 1000000 };
                nanosleep(&pause, NULL);
            }
        }
        struct io_uring_sync_cancel_reg cancel = {
            .flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL,
            .timeout = { .tv_sec = 0, .tv_nsec = 100000000 },
        };
        if (result >= 0 || result == -EINTR) {
            priv->uring_cancel_calls++;
            do result = io_uring_register_sync_cancel(&priv->ring, &cancel);
            while (result == -EINTR);
        }
        if (priv->uring_debug)
            xf86Msg(result < 0 && result != -ENOENT ? X_WARNING : X_INFO,
                    "%s: io_uring debug cancel result=%d (%s), "
                    "sq_ready=%u cq_ready=%u\n",
                    pInfo->name, result,
                    result < 0 ? strerror(-result) : "success",
                    io_uring_sq_ready(&priv->ring),
                    io_uring_cq_ready(&priv->ring));
        if (result < 0 && result != -ENOENT) {
            priv->shutdown_failed = 1;
            xf86Msg(X_ERROR, "%s: io_uring cancellation failed (%s); "
                    "device retired, buffers retained until server exit\n",
                    pInfo->name, strerror(-result));
            priv->uring_errors++;
            ainput_uring_debug_state(pInfo, "stop-retained");
            return;
        }
        priv->shutdown_failed = 0;
        if (buffer_ring) {
            free_result = io_uring_free_buf_ring(
                &priv->ring, buffer_ring,
                AINPUT_URING_MULTISHOT_BUFFERS,
                AINPUT_URING_MULTISHOT_GROUP);
            if (free_result == 0)
                buffer_ring = NULL;
        }
        io_uring_queue_exit(&priv->ring);
        priv->ring_ready = 0;
    }
    if (buffer_ring) {
        munmap(buffer_ring,
               AINPUT_URING_MULTISHOT_BUFFERS *
                   sizeof(struct io_uring_buf));
        xf86Msg(X_WARNING,
                "%s: buffer-ring unregister failed during shutdown: %s\n",
                pInfo->name, strerror(-free_result));
    }
    priv->chain_inflight = 0;
    priv->linked_fallback = 0;
    priv->fixed_file = 0;
    priv->fixed_buffers = 0;
    priv->buffer_ring = NULL;
    priv->multishot_active = 0;
    priv->deferred_read_error = 0;
    priv->held_buffer = -1;
    free(priv->multishot_buffers);
    priv->multishot_buffers = NULL;
    priv->sqpoll_active = 0;
    priv->watch_fd = -1;
    ainput_uring_debug_state(pInfo, "stop-complete");
}

/*
 * xf86AddEnabledDevice registers the fd currently stored in pInfo. Keep the
 * server/logind-owned evdev fd there outside this short call, while directing
 * Xorg's input thread to the pollable ring fd. The callback reads completions
 * through priv and never treats pInfo->fd as the ring.
 */
static void ainput_register_ring_watch(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    int device_fd;

    if (!priv->use_io_uring) {
        xf86AddEnabledDevice(pInfo);
        priv->read_registered = 1;
        return;
    }

    device_fd = pInfo->fd;
    pInfo->fd = priv->watch_fd;
    xf86AddEnabledDevice(pInfo);
    pInfo->fd = device_fd;
    priv->read_registered = 1;
    ainput_uring_debug_state(pInfo, "watch-registered");
}

static void ainput_unregister_ring_watch(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    int device_fd;

    if (!priv->read_registered)
        return;
    if (!priv->use_io_uring) {
        xf86RemoveEnabledDevice(pInfo);
        priv->read_registered = 0;
        return;
    }
    device_fd = pInfo->fd;
    pInfo->fd = priv->watch_fd;
    xf86RemoveEnabledDevice(pInfo);
    pInfo->fd = device_fd;
    priv->read_registered = 0;
    ainput_uring_debug_state(pInfo, "watch-unregistered");
}

static int ainput_uring_start(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct io_uring_params params = {0};
    struct iovec buffers[2] = {
        { priv->event_buffer[0], sizeof(priv->event_buffer[0]) },
        { priv->event_buffer[1], sizeof(priv->event_buffer[1]) },
    };
    int buffer_ring_error = 0;
    int result;

    if (priv->ring_ready)
        return priv->shutdown_failed ? BadValue : Success;

    if (priv->uring_debug)
        xf86Msg(X_INFO,
                "%s: io_uring debug start: requested multishot=%s "
                "sqpoll=%s idle=%d cpu=%d evdev_fd=%d\n",
                pInfo->name,
                priv->multishot_requested ? "yes" : "no",
                priv->sqpoll_requested ? "yes" : "no",
                priv->sqpoll_idle, priv->sqpoll_cpu, priv->fd);

    if (priv->sqpoll_requested) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = (unsigned int)priv->sqpoll_idle;
        if (priv->sqpoll_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = (unsigned int)priv->sqpoll_cpu;
        }
    }
    result = io_uring_queue_init_params(AINPUT_URING_DEPTH, &priv->ring,
                                        &params);
    if (result < 0 && priv->sqpoll_requested) {
        xf86Msg(X_WARNING,
                "%s: SQPOLL unavailable (%s); continuing without it\n",
                pInfo->name, strerror(-result));
        memset(&params, 0, sizeof(params));
        result = io_uring_queue_init_params(AINPUT_URING_DEPTH, &priv->ring,
                                            &params);
    }
    if (result < 0)
        goto failure;
    priv->ring_ready = 1;
    priv->sqpoll_active = !!(params.flags & IORING_SETUP_SQPOLL);
    if (priv->uring_debug)
        xf86Msg(X_INFO,
                "%s: io_uring debug queue initialized: ring_fd=%d "
                "flags=0x%x features=0x%x sq_entries=%u cq_entries=%u\n",
                pInfo->name, priv->ring.ring_fd, params.flags,
                params.features, params.sq_entries, params.cq_entries);

    struct io_uring_sync_cancel_reg cancel = {
        .flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL,
        .timeout = { .tv_sec = 0, .tv_nsec = 100000000 },
    };
    do result = io_uring_register_sync_cancel(&priv->ring, &cancel);
    while (result == -EINTR);
    if (result < 0 && result != -ENOENT) {
        io_uring_queue_exit(&priv->ring); // No request has been submitted.
        priv->ring_ready = 0;
        goto failure;
    }

    result = io_uring_register_files(&priv->ring, &priv->fd, 1);
    if (result == 0)
        priv->fixed_file = 1;
    else
        xf86Msg(X_WARNING,
                "%s: fixed-file registration unavailable (%s)\n",
                pInfo->name, strerror(-result));

    result = io_uring_register_buffers(&priv->ring, buffers, 2);
    if (result == 0)
        priv->fixed_buffers = 1;
    else
        xf86Msg(X_WARNING,
                "%s: fixed-buffer registration unavailable (%s)\n",
                pInfo->name, strerror(-result));

    if (priv->multishot_requested) {
        int mask = io_uring_buf_ring_mask(
            AINPUT_URING_MULTISHOT_BUFFERS);

        int allocation_result = posix_memalign(
            (void **)&priv->multishot_buffers, 64,
            AINPUT_URING_MULTISHOT_BUFFERS *
                sizeof(priv->multishot_buffers[0]));

        if (allocation_result == 0)
            priv->buffer_ring = io_uring_setup_buf_ring(
                &priv->ring, AINPUT_URING_MULTISHOT_BUFFERS,
                AINPUT_URING_MULTISHOT_GROUP, 0, &buffer_ring_error);
        else
            buffer_ring_error = -allocation_result;
        if (priv->buffer_ring) {
            io_uring_buf_ring_init(priv->buffer_ring);
            for (int bid = 0; bid < AINPUT_URING_MULTISHOT_BUFFERS;
                 bid++)
                io_uring_buf_ring_add(
                    priv->buffer_ring, priv->multishot_buffers[bid],
                    sizeof(priv->event_buffer[0]),
                    (unsigned short)bid, mask, bid);
            io_uring_buf_ring_advance(
                priv->buffer_ring, AINPUT_URING_MULTISHOT_BUFFERS);
        } else {
            free(priv->multishot_buffers);
            priv->multishot_buffers = NULL;
            xf86Msg(X_WARNING,
                    "%s: READ_MULTISHOT buffer ring unavailable (%s); "
                    "using direct one-shot reads\n",
                    pInfo->name, strerror(-buffer_ring_error));
        }
    }

    if (priv->buffer_ring)
        result = ainput_uring_submit_multishot(pInfo);
    else
        result = ainput_uring_submit_oneshot(pInfo, 0);

    if (result < 0)
        goto failure;

    priv->watch_fd = priv->ring.ring_fd;
    xf86Msg(X_INFO,
            "%s: io_uring %s read active, fixed_file=%s "
            "fixed_buffers=%s no_sqarray=%s sqpoll=%s "
            "sqpoll_idle_ms=%d sqpoll_cpu=%d server_fd=%s\n",
            pInfo->name,
            priv->buffer_ring ? "multishot" : "direct",
            priv->fixed_file ? "yes" : "no",
            priv->fixed_buffers ? "yes" : "no",
            (params.flags & IORING_SETUP_NO_SQARRAY) ? "yes" : "no",
            priv->sqpoll_active ? "yes" : "no",
            priv->sqpoll_idle, priv->sqpoll_cpu,
            ainput_fd_is_server_managed(pInfo) ? "yes" : "no");
    ainput_uring_debug_state(pInfo, "start-complete");
    return Success;

failure:
    priv->uring_errors++;
    xf86Msg(X_ERROR, "%s: io_uring setup failed: %s\n", pInfo->name,
            strerror(-result));
    ainput_uring_stop(pInfo);
    return BadValue;
}

static inline ssize_t ainput_read_event_batch_uring(
    InputInfoPtr pInfo, struct input_event **events_out)
{
    AInputPriv *priv = pInfo->private;
    struct io_uring_cqe *cqe;
    int saved_error = 0;
    int peek_result;

    if (priv->uring_debug && priv->ring.cq.koverflow &&
        *priv->ring.cq.koverflow != priv->uring_kernel_overflow) {
        priv->uring_kernel_overflow = *priv->ring.cq.koverflow;
        xf86Msg(X_ERROR,
                "%s: io_uring completion queue overflow count changed to %u\n",
                pInfo->name, priv->uring_kernel_overflow);
    }
    ainput_uring_recycle_buffer(priv);
    if (priv->deferred_read_error) {
        errno = priv->deferred_read_error;
        return -1;
    }

    for (;;) {
        do {
            peek_result = io_uring_peek_cqe(&priv->ring, &cqe);
        } while (peek_result == -EINTR);
        if (peek_result != 0)
            break;
        int result = cqe->res;
        uint64_t type = cqe->user_data;
        unsigned int flags = cqe->flags;
        unsigned int cq_ready = priv->uring_debug ?
            io_uring_cq_ready(&priv->ring) : 0;

        priv->uring_cqes_seen++;
        if (result > 0)
            priv->uring_bytes += (unsigned int)result;
        else if (result < 0) {
            priv->uring_errors++;
            if (result == -EAGAIN)
                priv->uring_eagain++;
            if (result == -ENOBUFS)
                priv->uring_enobufs++;
        }
        if (cq_ready > priv->uring_max_cq_ready)
            priv->uring_max_cq_ready = cq_ready;
        ainput_uring_debug_cqe(pInfo, type, result, flags);
        io_uring_cqe_seen(&priv->ring, cqe);
        if (type == AINPUT_URING_POLL) {
            if (result < 0 && !saved_error)
                saved_error = -result;
            continue;
        }
        if (type == AINPUT_URING_READ_0 || type == AINPUT_URING_READ_1) {
            unsigned int completed =
                (unsigned int)(type - AINPUT_URING_READ_0);
            int read_result = result;

            if (priv->uring_debug && !priv->chain_inflight)
                xf86Msg(X_WARNING,
                        "%s: io_uring debug invariant: one-shot CQE without "
                        "an in-flight chain\n", pInfo->name);
            priv->chain_inflight = 0;
            if (read_result < 0) {
                if (read_result == -EAGAIN) {
                    if (!priv->linked_fallback) {
                        priv->linked_fallback = 1;
                        xf86Msg(X_WARNING,
                                "%s: direct io_uring read returned EAGAIN; "
                                "using linked poll/read fallback\n",
                                pInfo->name);
                    }
                    result = ainput_uring_submit_oneshot(pInfo, completed);
                    if (result < 0)
                        saved_error = -result;
                    if (saved_error)
                        break;
                    errno = EAGAIN;
                    return -1;
                }
                if (!saved_error || saved_error == ECANCELED) {
                    xf86Msg(X_WARNING,
                            "%s: io_uring one-shot read CQE failed: %s\n",
                            pInfo->name, strerror(-read_result));
                    saved_error = -read_result;
                }
                break;
            }
            if ((size_t)read_result > sizeof(priv->event_buffer[completed])) {
                saved_error = EOVERFLOW;
                break;
            }
            if ((size_t)read_result % sizeof(struct input_event) != 0) {
                saved_error = EPROTO;
                break;
            }

            *events_out = priv->event_buffer[completed];
            if (read_result == 0)
                return 0;

            result = ainput_uring_submit_oneshot(pInfo, completed ^ 1U);
            if (result < 0) {
                priv->deferred_read_error = -result;
            }
            return read_result;
        }
        if (type == AINPUT_URING_READ_MULTISHOT) {
            unsigned int bid;

            priv->multishot_active =
                !!(flags & IORING_CQE_F_MORE);
            if (result < 0) {
                int multishot_error = -result;

                if (result == -ENOBUFS) {
                    result = ainput_uring_submit_multishot(pInfo);
                    if (result < 0)
                        saved_error = -result;
                    if (!saved_error)
                        continue;
                    break;
                }
                if (result == -EAGAIN || result == -EINVAL ||
                    result == -EOPNOTSUPP || result == -ENOSYS) {
                    int free_result = io_uring_free_buf_ring(
                        &priv->ring, priv->buffer_ring,
                        AINPUT_URING_MULTISHOT_BUFFERS,
                        AINPUT_URING_MULTISHOT_GROUP);

                    if (free_result < 0) {
                        saved_error = -free_result;
                        break;
                    }
                    priv->buffer_ring = NULL;
                    free(priv->multishot_buffers);
                    priv->multishot_buffers = NULL;
                    result = ainput_uring_submit_oneshot(pInfo, 0);
                    if (result < 0) {
                        saved_error = -result;
                        break;
                    }
                    xf86Msg(X_WARNING,
                            "%s: READ_MULTISHOT unavailable (%s); "
                            "using direct one-shot reads\n",
                            pInfo->name, strerror(multishot_error));
                    continue;
                }
                xf86Msg(X_WARNING,
                        "%s: io_uring multishot read CQE failed: %s\n",
                        pInfo->name, strerror(-result));
                saved_error = -result;
                break;
            }
            if (result == 0)
                return 0;
            if (!(flags & IORING_CQE_F_BUFFER)) {
                xf86Msg(X_ERROR,
                        "%s: io_uring multishot CQE has no provided buffer "
                        "flag (res=%d flags=0x%x)\n",
                        pInfo->name, result, flags);
                saved_error = EPROTO;
                break;
            }
            bid = flags >> IORING_CQE_BUFFER_SHIFT;
            if (bid >= AINPUT_URING_MULTISHOT_BUFFERS) {
                saved_error = EOVERFLOW;
                break;
            }
            if ((size_t)result >
                sizeof(priv->multishot_buffers[bid])) {
                saved_error = EOVERFLOW;
                break;
            }
            if ((size_t)result % sizeof(struct input_event) != 0) {
                saved_error = EPROTO;
                break;
            }

            if (!priv->multishot_active) {
                int submit_result = ainput_uring_submit_multishot(pInfo);

                if (submit_result < 0)
                    priv->deferred_read_error = -submit_result;
            }
            priv->held_buffer = (int)bid;
            *events_out = priv->multishot_buffers[bid];
            return result;
        }
        xf86Msg(X_ERROR,
                "%s: io_uring CQE has unknown user_data=%llu res=%d "
                "flags=0x%x\n",
                pInfo->name, (unsigned long long)type, result, flags);
        saved_error = EPROTO;
        break;
    }

    if (peek_result != -EAGAIN && peek_result < 0 && !saved_error)
        saved_error = -peek_result;

    if (saved_error) {
        errno = saved_error;
        return -1;
    }
    errno = EAGAIN;
    return -1;
}
#else
static inline void ainput_uring_recycle_buffer(AInputPriv *priv) { (void)priv; }
static inline void ainput_uring_stop(InputInfoPtr pInfo) { (void)pInfo; }
static int ainput_uring_start(InputInfoPtr pInfo) { (void)pInfo; return BadValue; }
static void ainput_register_ring_watch(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    xf86AddEnabledDevice(pInfo);
    priv->read_registered = 1;
}
static void ainput_unregister_ring_watch(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    if (priv->read_registered)
        xf86RemoveEnabledDevice(pInfo);
    priv->read_registered = 0;
}
#endif

static inline ssize_t ainput_read_event_batch_read(
    InputInfoPtr pInfo, struct input_event **events_out)
{
    AInputPriv *priv = pInfo->private;
    ssize_t len;

    *events_out = priv->event_buffer[0];
    do
        len = read(pInfo->fd, *events_out,
                   sizeof(priv->event_buffer[0]));
    while (len < 0 && errno == EINTR);

    if (len > 0 && (size_t)len % sizeof(struct input_event) != 0)
    {
        errno = EPROTO;
        return -1;
    }

    return len;
}
static inline ssize_t ainput_read_event_batch(
    InputInfoPtr pInfo, struct input_event **events_out)
{
#ifdef AINPUT_IO_URING
    AInputPriv *priv = pInfo->private;
    if (priv->use_io_uring)
        return ainput_read_event_batch_uring(pInfo, events_out);
#endif
    return ainput_read_event_batch_read(pInfo, events_out);
}

static inline int ainput_event_source_drained(AInputPriv *priv, ssize_t len)
{
#ifdef AINPUT_IO_URING
    if (priv->use_io_uring) {
    (void)len;
        return io_uring_cq_ready(&priv->ring) == 0;
    }
#endif

    return (size_t)len < sizeof(priv->event_buffer[0]);
}

static int ainput_discard_backlog(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    for (unsigned int i = 0; i < 8; i++) {
        struct input_event *events;
        ssize_t len = ainput_read_event_batch_read(pInfo, &events);
        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ainput_begin_resync(priv);
            return 1;
        }
        if (len <= 0) {
            if (len == 0) errno = ENODEV;
            return -1;
        }
    }
    return 0;
}

#if !defined(AINPUT_UNIT_TEST) || defined(AINPUT_TEST_TIMERS)
static int ainput_open_device(InputInfoPtr pInfo, AInputPriv *priv);
static void ainput_report_read_end(InputInfoPtr pInfo, ssize_t len);

static CARD32 ainput_resume_timer(OsTimerPtr timer, CARD32 now, void *arg)
{
    InputInfoPtr pInfo = arg;
    AInputPriv *priv = pInfo->private;
    int drained;
    (void)timer;
    (void)now;
    if (!pInfo->dev->public.on || priv->read_registered)
        return 0;
    if (!pInfo->dev->enabled)
        return 1;
    drained = ainput_discard_backlog(pInfo);
    if (drained == 0)
        return 1;
    if (drained < 0) {
        ainput_report_read_end(pInfo, -1);
        return 0;
    }
    ainput_resync_state(pInfo);
    if (priv->use_io_uring && ainput_uring_start(pInfo) != Success) {
#ifdef AINPUT_IO_URING
        if (priv->shutdown_failed)
            return 0;
#endif
        xf86Msg(X_WARNING, "%s: falling back to the read backend\n", pInfo->name);
        priv->use_io_uring = 0;
    }
    priv->last_read_error = 0;
    ainput_register_ring_watch(pInfo);
    return 0;
}

static int ainput_schedule_resume(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    priv->last_read_error = 0;
    priv->resume_timer = TimerSet(priv->resume_timer, 0, 1,
                                  ainput_resume_timer, pInfo);
    return priv->resume_timer != NULL;
}

static void ainput_cancel_resume(AInputPriv *priv)
{
    if (priv->resume_timer)
        TimerFree(priv->resume_timer);
    priv->resume_timer = NULL;
}

static void ainput_defer_resync(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;

    if (!priv->sync_dropped)
        return;

    if (priv->read_registered)
        ainput_unregister_ring_watch(pInfo);
    if (priv->use_io_uring)
        ainput_uring_stop(pInfo);
#ifdef AINPUT_IO_URING
    if (priv->shutdown_failed)
        return;
#endif
    if (!ainput_schedule_resume(pInfo))
        xf86Msg(X_ERROR, "%s: failed to schedule evdev resynchronization\n",
                pInfo->name);
}

static CARD32 ainput_reconnect_timer(OsTimerPtr timer, CARD32 now, void *arg)
{
    InputInfoPtr pInfo = arg;
    AInputPriv *priv;

    (void)timer;
    (void)now;
    if (!pInfo || !pInfo->private || !pInfo->dev)
        return 0;

    priv = pInfo->private;
    if (!pInfo->dev->public.on || priv->read_registered || priv->fd >= 0)
        return 0;

    if (ainput_open_device(pInfo, priv) != Success) {
        priv->reconnect_attempts++;
        if (priv->reconnect_attempts == 1 ||
            (priv->reconnect_attempts & (priv->reconnect_attempts - 1U)) == 0)
            xf86Msg(X_INFO,
                    "%s: waiting for input device to reappear "
                    "(attempt %u)\n",
                    pInfo->name, priv->reconnect_attempts);
        return AINPUT_URING_RECONNECT_INTERVAL_MS;
    }

    if (!ainput_schedule_resume(pInfo)) {
        close(priv->fd);
        priv->fd = pInfo->fd = -1;
        return AINPUT_URING_RECONNECT_INTERVAL_MS;
    }

    priv->last_read_error = 0;
    xf86Msg(X_INFO, "%s: input device reconnected after %u attempt%s\n",
            pInfo->name, priv->reconnect_attempts + 1,
            priv->reconnect_attempts ? "s" : "");
    priv->reconnect_attempts = 0;
    return 0;
}

static void ainput_schedule_reconnect(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;

    if (!pInfo->dev || !pInfo->dev->public.on)
        return;

    priv->reconnect_timer = TimerSet(
        priv->reconnect_timer, 0, AINPUT_URING_RECONNECT_INTERVAL_MS,
        ainput_reconnect_timer, pInfo);

    if (!priv->reconnect_timer)
        xf86Msg(X_ERROR, "%s: failed to create reconnect timer\n",
                pInfo->name);
}

static void ainput_cancel_reconnect(AInputPriv *priv)
{
    if (!priv->reconnect_timer)
        return;
    TimerFree(priv->reconnect_timer);
    priv->reconnect_timer = NULL;
    priv->reconnect_attempts = 0;
}
#endif

#if defined(AINPUT_UNIT_TEST) && !defined(AINPUT_TEST_TIMERS)
static void ainput_defer_resync(InputInfoPtr pInfo)
{
    ainput_resync_state(pInfo);
}
#endif

static void ainput_report_read_end(InputInfoPtr pInfo, ssize_t len)
{
    AInputPriv *priv = pInfo->private;
    int error;

    if (len > 0 ||
        (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        return;

    error = (len == 0) ? ENODEV : errno;
    if (error == priv->last_read_error)
        return;

    priv->last_read_error = error;
    xf86Msg(X_WARNING,
            "%s: input device stopped producing events (fd=%d, errno=%d: %s)\n",
            pInfo->name, priv->fd, error, strerror(error));

    if (priv->read_registered)
    {
        ainput_release_tracked_state(pInfo);
        ainput_begin_resync(priv);
        priv->sync_dropped = 0;
        ainput_unregister_ring_watch(pInfo);
    }

    if (priv->use_io_uring)
        ainput_uring_stop(pInfo);
#ifdef AINPUT_IO_URING
    if (priv->shutdown_failed)
        return;
#endif
    if (!ainput_fd_is_server_managed(pInfo) && priv->fd >= 0)
    {
        close(priv->fd);
        pInfo->fd = -1;
        priv->fd = -1;
#if !defined(AINPUT_UNIT_TEST) || defined(AINPUT_TEST_TIMERS)
        ainput_schedule_reconnect(pInfo);
#endif
    }
}

static void ainput_finish_read_callback(InputInfoPtr pInfo, ssize_t len)
{
    AInputPriv *priv = pInfo->private;

    if (priv->deferred_read_error) {
        int error = priv->deferred_read_error;

        priv->deferred_read_error = 0;
        errno = error;
        ainput_report_read_end(pInfo, -1);
    }
    else if (len <= 0)
        ainput_report_read_end(pInfo, len);
}

#ifdef AINPUT_READ_BUDGET_DEBUG
static void ainput_debug_read_budget(InputInfoPtr pInfo,
                                     unsigned int reads, size_t events)
{
    AInputPriv *priv = pInfo->private;
    struct pollfd poll_fd = {
        .fd = pInfo->fd,
        .events = POLLIN,
    };
    int more_events;
    int result;

    if (reads != priv->read_budget)
        return;

#ifdef AINPUT_IO_URING
    if (priv->use_io_uring) {
        result = 0;
        more_events = io_uring_cq_ready(&priv->ring) > 0;
    } else
#endif
    {
        do
            result = poll(&poll_fd, 1, 0);
        while (result < 0 && errno == EINTR);
        more_events = result > 0 && (poll_fd.revents & POLLIN);
    }

    if (result < 0)
    {
        xf86Msg(X_WARNING,
                "%s: ReadBudget debug poll failed (fd=%d, errno=%d: %s)\n",
                pInfo->name, pInfo->fd, errno, strerror(errno));
        return;
    }

    xf86Msg(X_INFO,
            "%s: ReadBudget debug: reads=%u/%u events=%zu more_events=%s\n",
            pInfo->name, reads, priv->read_budget, events,
            more_events ? "yes" : "no");
}
#endif

static void ainput_read_keyboard(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct input_event *events = NULL;
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        size_t event_count;
        const struct input_event *ev;
        const struct input_event *end;

        len = ainput_read_event_batch(pInfo, &events);
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        event_count = (size_t)len / sizeof(*events);
        ev = events;
        end = events + event_count;
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += event_count;
#endif

        for (; ev < end; ev++)
        {
            if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
            {
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
                continue;

            if (ev->type == EV_SYN && ev->code == SYN_REPORT)
            {
                ainput_flush_pending_keys(pInfo, ainput_server_timestamp());
                continue;
            }

            if (ev->type != EV_KEY || ev->value == 2)
                continue;

            int x11_keycode = ev->code + 8;
            if (x11_keycode >= 8 && x11_keycode <= 255)
            {
                if (!ainput_queue_pending_event(pInfo, AINPUT_PENDING_KEY,
                                                ev->code, ev->value != 0))
                    ainput_begin_resync(priv);
            }
        }

        ainput_uring_recycle_buffer(priv);
        if (ainput_event_source_drained(priv, len))
            break;
    }

    if (priv->sync_dropped) {
        ainput_defer_resync(pInfo);
        return;
    }

#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_finish_read_callback(pInfo, len);
}

static void ainput_read_relative_mouse(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    DeviceIntPtr dev = pInfo->dev;
    ValuatorMask *motion_mask = priv->motion_mask;
    struct input_event *events = NULL;
    double sens = priv->effective_sensitivity;
    int64_t acc_x = priv->acc_x;
    int64_t acc_y = priv->acc_y;
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        size_t event_count;
        const struct input_event *ev;
        const struct input_event *end;

        len = ainput_read_event_batch(pInfo, &events);
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        event_count = (size_t)len / sizeof(*events);
        ev = events;
        end = events + event_count;
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += event_count;
#endif

        for (; ev < end; ev++)
        {
            if (ev->type == EV_REL)
            {
                if (priv->sync_dropped)
                    continue;

                switch (ev->code)
                {
                case REL_X:
                    ainput_accumulate(&acc_x, ev->value);
                    break;
                case REL_Y:
                    ainput_accumulate(&acc_y, ev->value);
                    break;
                case REL_WHEEL:
                    if (!priv->has_wheel_hi_res) {
                        if (!ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_V, 0, ev->value)) {
                            acc_x = acc_y = 0;
                            ainput_begin_resync(priv);
                        }
                    }
                    break;
                case REL_HWHEEL:
                    if (!priv->has_hwheel_hi_res) {
                        if (!ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_H, 0, ev->value)) {
                            acc_x = acc_y = 0;
                            ainput_begin_resync(priv);
                        }
                    }
                    break;
                case REL_WHEEL_HI_RES:
                    if (priv->has_wheel_hi_res) {
                        if (!ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_V_HI_RES, 0,
                                ev->value)) {
                            acc_x = acc_y = 0;
                            ainput_begin_resync(priv);
                        }
                    }
                    break;
                case REL_HWHEEL_HI_RES:
                    if (priv->has_hwheel_hi_res) {
                        if (!ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_H_HI_RES, 0,
                                ev->value)) {
                            acc_x = acc_y = 0;
                            ainput_begin_resync(priv);
                        }
                    }
                    break;
                }
                continue;
            }

            if (ev->type == EV_KEY)
            {
                if (priv->sync_dropped)
                    continue;

                /* EV_KEY value 2 is repeat, not another state transition. */
                if (ev->value == 2)
                    continue;

                int button = ainput_mouse_button_for_code(ev->code);

                if (button > 0)
                {
                    if (!ainput_queue_pending_event(
                            pInfo, AINPUT_PENDING_BUTTON, ev->code,
                            ev->value != 0))
                    {
                        acc_x = 0;
                        acc_y = 0;
                        ainput_begin_resync(priv);
                    }
                }
                continue;
            }

            if (ev->type != EV_SYN)
                continue;

            if (ev->code == SYN_DROPPED)
            {
                acc_x = 0;
                acc_y = 0;
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
                continue;

            if (ev->code != SYN_REPORT)
                continue;

            CARD32 report_ms = ainput_server_timestamp();

            if (acc_x != 0 || acc_y != 0)
            {
                ainput_post_relative_motion(dev, motion_mask,
                                            (double)acc_x * sens,
                                            (double)acc_y * sens,
                                            (double)acc_x,
                                            (double)acc_y,
                                            report_ms);
                acc_x = 0;
                acc_y = 0;
            }

            ainput_flush_pending_pointer(pInfo, report_ms);
        }

        ainput_uring_recycle_buffer(priv);
        if (ainput_event_source_drained(priv, len))
            break;
    }

    priv->acc_x = acc_x;
    priv->acc_y = acc_y;
    if (priv->sync_dropped) {
        ainput_defer_resync(pInfo);
        return;
    }
#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_finish_read_callback(pInfo, len);
}

static void ainput_read_absolute_mouse(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    DeviceIntPtr dev = pInfo->dev;
    ValuatorMask *motion_mask = priv->motion_mask;
    struct input_event *events = NULL;
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        size_t event_count;
        const struct input_event *ev;
        const struct input_event *end;

        len = ainput_read_event_batch(pInfo, &events);
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        event_count = (size_t)len / sizeof(*events);
        ev = events;
        end = events + event_count;
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += event_count;
#endif

        for (; ev < end; ev++)
        {
            if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
            {
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
                continue;

            switch (ev->type)
            {
            case EV_REL:
                switch (ev->code)
                {
                    case REL_X:
                        ainput_accumulate(&priv->acc_x, ev->value);
                        break;
                    case REL_Y:
                        ainput_accumulate(&priv->acc_y, ev->value);
                        break;
                    case REL_WHEEL:
                        if (!priv->has_wheel_hi_res &&
                            !ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_V, 0, ev->value))
                            ainput_begin_resync(priv);
                        break;

                    case REL_HWHEEL:
                        if (!priv->has_hwheel_hi_res &&
                            !ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_H, 0, ev->value))
                            ainput_begin_resync(priv);
                        break;

                    case REL_WHEEL_HI_RES:
                        if (priv->has_wheel_hi_res &&
                            !ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_V_HI_RES, 0,
                                ev->value))
                            ainput_begin_resync(priv);
                        break;

                    case REL_HWHEEL_HI_RES:
                        if (priv->has_hwheel_hi_res &&
                            !ainput_queue_pending_event(
                                pInfo, AINPUT_PENDING_SCROLL_H_HI_RES, 0,
                                ev->value))
                            ainput_begin_resync(priv);
                        break;
                }
                break;

            case EV_ABS:
                switch (ev->code)
                {
                case ABS_X:
                    priv->abs_x = ev->value;
                    priv->has_abs_event |= 1;
                    break;
                case ABS_Y:
                    priv->abs_y = ev->value;
                    priv->has_abs_event |= 2;
                    break;
                }
                break;

            case EV_KEY:
            {
                if (ev->value == 2)
                    break;

                int button = ainput_mouse_button_for_code(ev->code);

                if (button > 0)
                {
                    if (!ainput_queue_pending_event(
                            pInfo, AINPUT_PENDING_BUTTON, ev->code,
                            ev->value != 0))
                        ainput_begin_resync(priv);
                }
                break;
            }

            case EV_SYN:
                if (ev->code != SYN_REPORT)
                    break;

                double sens = priv->effective_sensitivity;
                CARD32 report_ms = ainput_server_timestamp();

                if (priv->acc_x != 0 || priv->acc_y != 0)
                {
                    double dx = (double)priv->acc_x * sens;
                    double dy = (double)priv->acc_y * sens;

                    ainput_post_relative_motion(dev, motion_mask, dx, dy,
                                                (double)priv->acc_x,
                                                (double)priv->acc_y,
                                                report_ms);

                    priv->acc_x = 0;
                    priv->acc_y = 0;
                }

                if (priv->has_abs_event)
                {
                    int valid = priv->has_last_abs & priv->has_abs_event;
                    int64_t delta_x = (valid & 1) ?
                        (int64_t)priv->abs_x - priv->last_abs_x : 0;
                    int64_t delta_y = (valid & 2) ?
                        (int64_t)priv->abs_y - priv->last_abs_y : 0;

                    if (priv->has_abs_event & 1)
                        priv->last_abs_x = priv->abs_x;
                    if (priv->has_abs_event & 2)
                        priv->last_abs_y = priv->abs_y;
                    priv->has_last_abs |= priv->has_abs_event;
                    priv->has_abs_event = 0;

                    if (delta_x != 0 || delta_y != 0)
                        ainput_post_relative_motion(dev, motion_mask,
                                                    (double)delta_x * sens,
                                                    (double)delta_y * sens,
                                                    (double)delta_x,
                                                    (double)delta_y,
                                                    report_ms);
                }

                ainput_flush_pending_pointer(pInfo, report_ms);
                break;
            }
        }

        ainput_uring_recycle_buffer(priv);
        if (ainput_event_source_drained(priv, len))
            break;
    }

    if (priv->sync_dropped) {
        ainput_defer_resync(pInfo);
        return;
    }

#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_finish_read_callback(pInfo, len);
}

static int ainput_read_evbits(int fd, unsigned long evbits[NBITS(EV_MAX)])
{
    memset(evbits, 0, sizeof(unsigned long) * NBITS(EV_MAX));
    return ainput_ioctl_retry(
        fd, EVIOCGBIT(0, sizeof(unsigned long) * NBITS(EV_MAX)), evbits);
}

static void ainput_detect_scroll_axes(
    AInputPriv *priv, const unsigned long evbits[NBITS(EV_MAX)])
{
    unsigned long relbits[NBITS(REL_MAX)] = {0};

    if (priv->type != DEV_MOUSE || !BIT_IS_SET(evbits, EV_REL) ||
        ainput_ioctl_retry(priv->fd, EVIOCGBIT(EV_REL, sizeof(relbits)),
                           relbits) < 0)
        return;

    priv->has_wheel_hi_res = BIT_IS_SET(relbits, REL_WHEEL_HI_RES);
    priv->has_hwheel_hi_res = BIT_IS_SET(relbits, REL_HWHEEL_HI_RES);
}

static void ainput_detect_absolute_axes(
    AInputPriv *priv, const unsigned long evbits[NBITS(EV_MAX)])
{
    unsigned long absbits[NBITS(ABS_MAX)] = {0};
    unsigned long relbits[NBITS(REL_MAX)] = {0};
    struct input_absinfo abs_x;
    struct input_absinfo abs_y;

    if (priv->type != DEV_MOUSE || !BIT_IS_SET(evbits, EV_ABS))
        return;

    if (BIT_IS_SET(evbits, EV_REL))
    {
        if (ainput_ioctl_retry(priv->fd,
                               EVIOCGBIT(EV_REL, sizeof(relbits)),
                               relbits) < 0)
            return;
        if (BIT_IS_SET(relbits, REL_X) || BIT_IS_SET(relbits, REL_Y))
            return;
    }

    if (ainput_ioctl_retry(priv->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)),
                           absbits) < 0)
        return;
    if (!BIT_IS_SET(absbits, ABS_X) || !BIT_IS_SET(absbits, ABS_Y))
        return;

    if (ainput_ioctl_retry(priv->fd, EVIOCGABS(ABS_X), &abs_x) < 0 ||
        ainput_ioctl_retry(priv->fd, EVIOCGABS(ABS_Y), &abs_y) < 0)
        return;

    priv->is_absolute = 1;
    priv->min_x = abs_x.minimum;
    priv->max_x = abs_x.maximum;
    priv->min_y = abs_y.minimum;
    priv->max_y = abs_y.maximum;
    /*
     * EVIOCGBIT returns a positive byte count on success. EVIOCGABS also
     * gives us the current position, so retain it as the initial baseline:
     * an evdev report may contain only the axis that changed.
     */
    priv->abs_x = priv->last_abs_x = abs_x.value;
    priv->abs_y = priv->last_abs_y = abs_y.value;
    priv->has_last_abs = 3;
}

#ifndef AINPUT_UNIT_TEST
static int ainput_device_init(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    AInputPriv *priv = pInfo->private;

    if (priv->type == DEV_KEYBOARD)
    {
        const char *layout = priv->xkb_layout ? priv->xkb_layout : "us";
        const char *variant = priv->xkb_variant ? priv->xkb_variant : NULL;

        XkbRMLVOSet rmlvo = {
            .rules = "evdev",
            .model = "pc105",
            .layout = (char *)layout,
            .variant = (char *)variant,
            .options = NULL};

        if (!InitKeyboardDeviceStruct(dev, &rmlvo, NULL, NULL))
            return BadValue;
    }
    else
    {
        BYTE map[AINPUT_BUTTON_COUNT + 1] = {0};
        for (int i = 1; i <= AINPUT_BUTTON_COUNT; i++)
            map[i] = (BYTE)i;

        Atom btn_labels[AINPUT_BUTTON_COUNT] = {0};
        Atom axes_labels[2] = {0};
        axes_labels[0] = MakeAtom(priv->is_absolute ? "Abs X" : "Rel X", 5, TRUE);
        axes_labels[1] = MakeAtom(priv->is_absolute ? "Abs Y" : "Rel Y", 5, TRUE);

        if (!InitPointerDeviceStruct((DevicePtr)dev, map,
                                     AINPUT_BUTTON_COUNT, btn_labels,
                                     (PtrCtrlProcPtr)NoopDDA, GetMotionHistorySize(), 2, axes_labels))
            return BadValue;

        if (!InitPointerAccelerationScheme(dev, PtrAccelNoOp))
            xf86Msg(X_WARNING, "%s: failed to initialize pointer acceleration.\n", pInfo->name);

        int mode = priv->is_absolute ? Absolute : Relative;
        xf86InitValuatorAxisStruct(dev, 0, axes_labels[0],
                                   priv->is_absolute ? priv->min_x : 0,
                                   priv->is_absolute ? priv->max_x : 0, 1, 0, 1, mode);
        xf86InitValuatorAxisStruct(dev, 1, axes_labels[1],
                                   priv->is_absolute ? priv->min_y : 0,
                                   priv->is_absolute ? priv->max_y : 0, 1, 0, 1, mode);

#ifndef AINPUT_XSERVER_DIRECT
        if (!priv->motion_mask)
        {
            priv->motion_mask = valuator_mask_new(2);
            if (!priv->motion_mask)
                return BadAlloc;
        }
#endif

        int property_result;

        priv->prop_sensitivity = MakeAtom(PROP_SENSITIVITY,
                                          strlen(PROP_SENSITIVITY), TRUE);
        if (XIRegisterPropertyHandler(dev, ainput_change_property,
                                      NULL, NULL) == 0)
            return BadAlloc;

        float init_val = priv->sensitivity;

        priv->initializing_property = 1;
        property_result = XIChangeDeviceProperty(
            dev, priv->prop_sensitivity, XA_FLOAT, 32,
            PropModeReplace, 1, &init_val, FALSE);
        priv->initializing_property = 0;
        if (property_result != Success)
            return property_result;

        property_result = XISetDevicePropertyDeletable(
            dev, priv->prop_sensitivity, FALSE);
        if (property_result != Success)
            return property_result;
    }
    return Success;
}

static int ainput_device_close(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    AInputPriv *priv = pInfo ? pInfo->private : NULL;
    int server_managed = ainput_fd_is_server_managed(pInfo);

    if (priv && priv->read_registered)
        ainput_unregister_ring_watch(pInfo);

    if (priv && priv->motion_mask)
        valuator_mask_free(&priv->motion_mask);

    if (priv) {
        ainput_cancel_resume(priv);
        ainput_cancel_reconnect(priv);
    }
    if (priv && priv->use_io_uring) {
        ainput_uring_stop(pInfo);
    }

    if (priv && priv->fd != -1 && !server_managed)
    {
        xf86CloseSerial(priv->fd);
        pInfo->fd = -1;
    }

    if (priv)
    {
        priv->fd = -1;
        priv->read_registered = 0;
    }

    return Success;
}

static int ainput_open_device(InputInfoPtr pInfo, AInputPriv *priv);

static int ainput_control(DeviceIntPtr dev, int what)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    AInputPriv *priv = pInfo->private;

    switch (what)
    {
    case DEVICE_INIT:
        return ainput_device_init(dev);

    case DEVICE_ON:
#ifdef AINPUT_IO_URING
        if (priv->shutdown_failed)
            return BadValue;
#endif
        if (ainput_fd_is_server_managed(pInfo)) {
            if (pInfo->fd < 0) {
                xf86Msg(X_ERROR,
                        "%s: server did not provide an input fd at DEVICE_ON\n",
                        pInfo->name);
                dev->public.on = FALSE;
                return BadValue;
            }

            priv->fd = pInfo->fd;
        } else if (pInfo->fd == -1 &&
            ainput_open_device(pInfo, priv) != Success)
        {
            dev->public.on = FALSE;
            return BadValue;
        }

        if (pInfo->fd != -1 && !priv->read_registered) {
            ainput_cancel_reconnect(priv);
            priv->fd = pInfo->fd;
            if (!ainput_schedule_resume(pInfo))
                return BadAlloc;
        }
        dev->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
        if (priv->read_registered)
            ainput_unregister_ring_watch(pInfo);
        ainput_cancel_resume(priv);
        ainput_cancel_reconnect(priv);
        if (priv->use_io_uring) {
            ainput_uring_stop(pInfo);
        }

        memset(priv->key_state, 0, sizeof(priv->key_state));
        ainput_begin_resync(priv);
        priv->sync_dropped = 0;
        if (ainput_fd_is_server_managed(pInfo))
            priv->fd = -1;
        dev->public.on = FALSE;
        return Success;

    case DEVICE_CLOSE:
        return ainput_device_close(dev);
    }
    return BadValue;
}

static float ainput_positive_real_option(InputInfoPtr pInfo, const char *name, float fallback)
{
    float value = (float)xf86SetRealOption(pInfo->options, name, fallback);

    return (!isfinite(value) || value <= 0.0f) ? fallback : value;
}

static int ainput_open_device(InputInfoPtr pInfo, AInputPriv *priv)
{
    if (pInfo->fd >= 0)
    {
        priv->fd = pInfo->fd;
        return Success;
    }

    const char *path = xf86FindOptionValue(pInfo->options, "Device");
    if (!path)
    {
        xf86Msg(X_ERROR, "%s: no 'Device' option found and server fd is invalid.\n", pInfo->name);
        return BadValue;
    }

    priv->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (priv->fd < 0)
    {
#ifdef AINPUT_IO_URING
        if (priv->reconnect_timer)
            return BadValue;
#endif
        xf86Msg(X_ERROR, "%s: failed to open %s (errno: %d)\n", pInfo->name, path, errno);
        return BadValue;
    }

    pInfo->fd = priv->fd;
    return Success;
}

static void ainput_read_options(InputInfoPtr pInfo, AInputPriv *priv)
{
    const char *backend;
    int read_budget;

    priv->xkb_layout = xf86FindOptionValue(pInfo->options, "xkb_layout");
    priv->xkb_variant = xf86FindOptionValue(pInfo->options, "xkb_variant");

    read_budget = xf86SetIntOption(pInfo->options, "ReadBudget",
                                   AINPUT_DEFAULT_READ_BUDGET);
    if (read_budget != 1 && read_budget != 2 &&
        read_budget != 4 && read_budget != 8)
    {
        xf86Msg(X_WARNING,
                "%s: invalid ReadBudget=%d; using %d\n",
                pInfo->name, read_budget, AINPUT_DEFAULT_READ_BUDGET);
        read_budget = AINPUT_DEFAULT_READ_BUDGET;
    }
    priv->read_budget = (unsigned int)read_budget;

    priv->sensitivity = ainput_positive_real_option(
        pInfo, "Sensitivity", AINPUT_DEFAULT_SENSITIVITY);

    priv->dpi = ainput_positive_real_option(
        pInfo, "DPI", AINPUT_DEFAULT_DPI);

    priv->reference_dpi = ainput_positive_real_option(
        pInfo, "ReferenceDPI", AINPUT_DEFAULT_DPI);

    backend = xf86FindOptionValue(pInfo->options, "Backend");
    if (!backend || strcasecmp(backend, "read") == 0)
        priv->use_io_uring = 0;
    else if (strcasecmp(backend, "io_uring") == 0 ||
             strcasecmp(backend, "uring") == 0)
        priv->use_io_uring = 1;
    else {
        xf86Msg(X_WARNING,
                "%s: unknown Backend \"%s\"; using read\n",
                pInfo->name, backend);
        priv->use_io_uring = 0;
    }
#ifdef AINPUT_IO_URING
    priv->uring_debug = xf86SetBoolOption(
        pInfo->options, "IoUringDebug", FALSE);
    priv->multishot_requested = xf86SetBoolOption(
        pInfo->options, "ReadMultishot", FALSE);
    priv->sqpoll_requested = xf86SetBoolOption(
        pInfo->options, "SQPoll", FALSE);
    priv->sqpoll_idle = xf86SetIntOption(
        pInfo->options, "SQPollIdle", AINPUT_URING_DEFAULT_SQPOLL_IDLE);
    if (priv->sqpoll_idle < 1 || priv->sqpoll_idle > 60000) {
        xf86Msg(X_WARNING,
                "%s: invalid SQPollIdle=%d; using %d ms\n",
                pInfo->name, priv->sqpoll_idle,
                AINPUT_URING_DEFAULT_SQPOLL_IDLE);
        priv->sqpoll_idle = AINPUT_URING_DEFAULT_SQPOLL_IDLE;
    }
    priv->sqpoll_cpu = xf86SetIntOption(pInfo->options, "SQPollCPU", -1);
    if (priv->sqpoll_cpu < -1) {
        xf86Msg(X_WARNING,
                "%s: invalid SQPollCPU=%d; disabling SQ affinity\n",
                pInfo->name, priv->sqpoll_cpu);
        priv->sqpoll_cpu = -1;
    }
#else
    if (priv->use_io_uring) {
        xf86Msg(X_WARNING, "%s: io_uring not compiled in; using read\n", pInfo->name);
        priv->use_io_uring = 0;
    }
#endif
}

static ADevType ainput_detect_type(InputInfoPtr pInfo,
                                   const unsigned long evbits[NBITS(EV_MAX)])
{
    const char *type_str = xf86FindOptionValue(pInfo->options, "Type");

    if (type_str)
        return (strcasecmp(type_str, "mouse") == 0) ? DEV_MOUSE : DEV_KEYBOARD;

    if (pInfo->attrs && (pInfo->attrs->flags & ATTR_POINTER))
        return DEV_MOUSE;

    if (BIT_IS_SET(evbits, EV_REL) || BIT_IS_SET(evbits, EV_ABS))
        return DEV_MOUSE;

    return DEV_KEYBOARD;
}

static void ainput_setup_info(InputInfoPtr pInfo, AInputPriv *priv)
{
    pInfo->private = priv;

    if (priv->type == DEV_KEYBOARD)
        pInfo->read_input = ainput_read_keyboard;
    else if (priv->is_absolute)
        pInfo->read_input = ainput_read_absolute_mouse;
    else
        pInfo->read_input = ainput_read_relative_mouse;

    pInfo->device_control = ainput_control;
    pInfo->flags |= XI86_ALWAYS_CORE;
    pInfo->type_name = (priv->type == DEV_MOUSE) ? XI_MOUSE : XI_KEYBOARD;
}

static void ainput_log_event_functions(void)
{
    static int logged;

    if (logged)
        return;
    logged = 1;

#ifdef AINPUT_XSERVER_DIRECT
    xf86Msg(X_INFO,
            "AInput event functions: key=QueueAInputKeyAtTime "
            "motion=QueueAInputRelativeMotion2DRawAtTime "
            "button=QueueAInputButtonAtTime\n");
#else
    xf86Msg(X_INFO,
            "AInput event functions: key=QueueKeyboardEvents "
            "motion=QueuePointerEvents button=QueuePointerEvents\n");
#endif
}

static void ainput_log_pre_init(InputInfoPtr pInfo, const AInputPriv *priv)
{
    ainput_log_event_functions();

    if (priv->type == DEV_MOUSE)
    {
        xf86Msg(
            X_INFO,
            "%s: AInput mouse initialized, sensitivity=%.3f dpi=%.1f reference_dpi=%.1f effective=%.3f read_budget=%u\n",
            pInfo->name,
            priv->sensitivity,
            priv->dpi,
            priv->reference_dpi,
            priv->effective_sensitivity,
            priv->read_budget);
    }
    else
    {
        xf86Msg(
            X_INFO,
            "%s: AInput keyboard initialized, layout='%s', variant='%s', read_budget=%u\n",
            pInfo->name,
            priv->xkb_layout ? priv->xkb_layout : AINPUT_DEFAULT_LAYOUT,
            priv->xkb_variant ? priv->xkb_variant : "default",
            priv->read_budget);
    }

    xf86Msg(X_INFO, "%s: input backend=%s\n", pInfo->name,
            priv->use_io_uring ? "io_uring" : "read");
}

static int ainput_pre_init(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    AInputPriv *priv = calloc(1, sizeof(AInputPriv));

    (void)drv;
    (void)flags;
    if (!priv)
        return BadAlloc;

    priv->fd = -1;
#ifdef AINPUT_IO_URING
    priv->held_buffer = -1;
    priv->watch_fd = -1;
#endif

    xf86CollectInputOptions(pInfo, NULL);
    ainput_read_options(pInfo, priv);

    if (priv->use_io_uring && !ainput_fd_is_server_managed(pInfo) &&
        !xf86FindOptionValue(pInfo->options, "Device"))
    {
        xf86Msg(X_ERROR, "%s: ainput_uring requires Option \"Device\".\n",
                pInfo->name);
        free(priv);
        return BadValue;
    }

    if (ainput_open_device(pInfo, priv) != Success)
    {
        free(priv);
        return BadValue;
    }

    unsigned long evbits[NBITS(EV_MAX)] = {0};
    if (ainput_read_evbits(priv->fd, evbits) < 0)
        xf86Msg(X_WARNING,
                "%s: failed to query evdev capabilities (errno=%d: %s); "
                "using configured/server device type\n",
                pInfo->name, errno, strerror(errno));

    priv->type = ainput_detect_type(pInfo, evbits);
    ainput_detect_scroll_axes(priv, evbits);
    ainput_detect_absolute_axes(priv, evbits);

    ainput_update_effective_sensitivity(priv);

    ainput_setup_info(pInfo, priv);
    xf86ProcessCommonOptions(pInfo, pInfo->options);
    ainput_log_pre_init(pInfo, priv);

    return Success;
}

static void ainput_uninit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    AInputPriv *priv = pInfo->private;

    (void)drv;
    if (priv)
    {
        if (priv->read_registered)
            ainput_unregister_ring_watch(pInfo);

        if (priv->motion_mask)
            valuator_mask_free(&priv->motion_mask);

        ainput_cancel_resume(priv);
        ainput_cancel_reconnect(priv);
        if (priv->use_io_uring) {
            ainput_uring_stop(pInfo);
        }
        if (priv->fd >= 0 && !ainput_fd_is_server_managed(pInfo))
            close(priv->fd);

#ifdef AINPUT_IO_URING
        if (!priv->shutdown_failed)
#endif
            free(priv);
        pInfo->private = NULL;
    }

    xf86DeleteInput(pInfo, flags);
}

static InputDriverRec AINPUT_DRIVER = {
    .driverVersion = DRIVER_VERSION,
    .driverName = DRIVER_NAME,
    .PreInit = ainput_pre_init,
    .UnInit = ainput_uninit,
    .module = NULL,
    .default_options = NULL,
    .capabilities = XI86_DRV_CAP_SERVER_FD
};

static XF86ModuleVersionInfo ainput_version_info = {
    DRIVER_NAME, MODULEVENDORSTRING, MODINFOSTRING1, MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    AINPUT_VERSION_MAJOR, AINPUT_VERSION_MINOR, AINPUT_VERSION_PATCH,
    ABI_CLASS_XINPUT, ABI_XINPUT_VERSION, MOD_CLASS_XINPUT,
    {0, 0, 0, 0}};

static void *ainput_setup(void *module, void *options, int *errmaj, int *errmin)
{
    (void)options;
    (void)errmaj;
    (void)errmin;
    xf86AddInputDriver(&AINPUT_DRIVER, module, 0);
    return module;
}

_X_EXPORT XF86ModuleData ainputModuleData = {
    .vers = &ainput_version_info,
    .setup = ainput_setup,
    .teardown = NULL,
};
#endif
