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
#include <linux/input-event-codes.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>

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

#include <linux/input.h>

/*
 * Experimental Xorg/XLibre direct keyboard path.
 *
 * These only work with patched Xorg/XLibre servers that export the matching
 * symbols. Keep them disabled for normal builds.
 *
 * AnbryInput-specific direct key path:
 *   make XSERVER_DIRECT=1
 */
#ifdef AINPUT_XSERVER_DIRECT
extern void QueueAInputKey(DeviceIntPtr pDev, int keycode, int is_down);
extern void QueueAInputRelativeMotion2DRaw(DeviceIntPtr pDev,
                                           double dx, double dy,
                                           double raw_dx, double raw_dy);
#endif

#define DRIVER_NAME "ainput"
#define DRIVER_VERSION 1
#define AINPUT_VERSION_MAJOR 1
#define AINPUT_VERSION_MINOR 7
#define AINPUT_VERSION_PATCH 2

#define PROP_SENSITIVITY "AInput Sensitivity"
#define AINPUT_EVENT_BATCH 256
#define AINPUT_DEFAULT_READ_BUDGET 1
#define AINPUT_DEFAULT_SENSITIVITY 1.0f
#define AINPUT_DEFAULT_DPI 1000.0f
#define AINPUT_DEFAULT_LAYOUT "us"

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

typedef struct
{
    /* Keep the relative-motion hot state together. */
    ValuatorMask *motion_mask;
    double effective_sensitivity;
    int acc_x, acc_y;
    int scroll_pending;
    int sync_dropped;
    unsigned int read_budget;
    int last_read_error;
    int is_absolute, has_abs_event;

    int wheel_v_steps, wheel_h_steps;
    int wheel_v_hi_res, wheel_h_hi_res;
    int wheel_v_hi_res_frame, wheel_h_hi_res_frame;
    unsigned int resync_query_failures;

    unsigned long key_state[NBITS(KEY_MAX)];

    int abs_x, abs_y;
    int min_x, max_x;
    int min_y, max_y;
    int has_last_abs;
    int last_abs_x, last_abs_y;

    const char *xkb_layout, *xkb_variant;
    float sensitivity, dpi, reference_dpi;
    Atom prop_sensitivity;
    ADevType type;
    int fd, initializing_property;
} AInputPriv;

static inline int ainput_fd_is_server_managed(const InputInfoPtr pInfo)
{
    return pInfo && (pInfo->flags & XI86_SERVER_FD);
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

static inline void ainput_post_relative_motion(AInputPriv *priv,
                                               DeviceIntPtr dev,
                                               ValuatorMask *mask,
                                               double dx, double dy,
                                               double raw_dx, double raw_dy,
                                               const struct input_event *ev)
{
#ifdef AINPUT_XSERVER_DIRECT
    if (!priv->is_absolute)
    {
        QueueAInputRelativeMotion2DRaw(dev, dx, dy, raw_dx, raw_dy);
        return;
    }
#endif

    (void)priv;
    (void)ev;

    valuator_mask_zero(mask);
    valuator_mask_set_unaccelerated(mask, 0, dx, raw_dx);
    valuator_mask_set_unaccelerated(mask, 1, dy, raw_dy);

    QueuePointerEvents(dev, MotionNotify, 0, POINTER_RELATIVE, mask);
}

static inline void ainput_post_button(InputInfoPtr pInfo, int button, int is_down)
{
    QueuePointerEvents(pInfo->dev, is_down ? ButtonPress : ButtonRelease,
                       button, 0, NULL);
}

static inline void
ainput_post_scroll_steps(InputInfoPtr pInfo, int value, int horizontal)
{
    int button;
    int steps = value < 0 ? -value : value;

    if (!steps)
        return;

    button = horizontal ? (value > 0 ? 7 : 6) : (value > 0 ? 4 : 5);
    while (steps--) {
        ainput_post_button(pInfo, button, 1);
        ainput_post_button(pInfo, button, 0);
    }
}

static inline void
ainput_post_hi_res_scroll(InputInfoPtr pInfo, int *remainder,
                          int value, int horizontal)
{
    *remainder += value;
    while (*remainder >= 120) {
        ainput_post_scroll_steps(pInfo, 1, horizontal);
        *remainder -= 120;
    }
    while (*remainder <= -120) {
        ainput_post_scroll_steps(pInfo, -1, horizontal);
        *remainder += 120;
    }
}

static inline void
ainput_flush_scroll_axis(InputInfoPtr pInfo, int *steps, int *hi_res_frame,
                         int *remainder, int horizontal)
{
    if (*steps) {
        /*
         * REL_WHEEL represents the same movement as REL_WHEEL_HI_RES.
         * Prefer its discrete count when both occur in one report.
        */
        ainput_post_scroll_steps(pInfo, *steps, horizontal);
        *remainder = 0;
    }
    else if (*hi_res_frame) {
        ainput_post_hi_res_scroll(pInfo, remainder, *hi_res_frame,
                                  horizontal);
    }

    *steps = 0;
    *hi_res_frame = 0;
}

static inline void ainput_post_key(InputInfoPtr pInfo, int key_code, int is_down,
                                   const struct input_event *ev)
{
#ifdef AINPUT_XSERVER_DIRECT
    if (ev)
    {
        QueueAInputKey(pInfo->dev, key_code, is_down);
        return;
    }
#else
    (void)ev;
#endif
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
    case BTN_SIDE:   return 8;
    case BTN_EXTRA:  return 9;
    case BTN_TOUCH:  return 1;
    default:         return 0;
    }
}

static int ainput_mouse_button_is_down(const unsigned long state[NBITS(KEY_MAX)],
                                       int button)
{
    switch (button)
    {
    case 1: return BIT_IS_SET(state, BTN_LEFT) || BIT_IS_SET(state, BTN_TOUCH);
    case 2: return BIT_IS_SET(state, BTN_MIDDLE);
    case 3: return BIT_IS_SET(state, BTN_RIGHT);
    case 8: return BIT_IS_SET(state, BTN_SIDE);
    case 9: return BIT_IS_SET(state, BTN_EXTRA);
    default: return 0;
    }
}

static void ainput_begin_resync(AInputPriv *priv)
{
    priv->sync_dropped = 1;
    priv->acc_x = 0;
    priv->acc_y = 0;
    priv->wheel_v_steps = 0;
    priv->wheel_h_steps = 0;
    priv->wheel_v_hi_res = 0;
    priv->wheel_h_hi_res = 0;
    priv->wheel_v_hi_res_frame = 0;
    priv->wheel_h_hi_res_frame = 0;
    priv->scroll_pending = 0;
    priv->has_abs_event = 0;
}

static void ainput_release_tracked_state(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;

    if (priv->type == DEV_KEYBOARD)
    {
        for (unsigned int code = 0; code <= KEY_MAX && code + 8 <= 255; code++)
            if (BIT_IS_SET(priv->key_state, code))
                ainput_post_key(pInfo, (int)code + 8, 0, NULL);
    }
    else
    {
        static const int buttons[] = {1, 2, 3, 8, 9};

        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
            if (ainput_mouse_button_is_down(priv->key_state, buttons[i]))
                ainput_post_button(pInfo, buttons[i], 0);
    }

    memset(priv->key_state, 0, sizeof(priv->key_state));
}

static void ainput_resync_state(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    unsigned long current[NBITS(KEY_MAX)] = {0};
    int result;

    do
        result = ioctl(pInfo->fd, EVIOCGKEY(sizeof(current)), current);
    while (result < 0 && errno == EINTR);

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
                ainput_post_key(pInfo, (int)code + 8, is_down, NULL);
        }
    }
    else
    {
        static const int buttons[] = {1, 2, 3, 8, 9};

        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        {
            int button = buttons[i];
            int was_down = ainput_mouse_button_is_down(priv->key_state, button);
            int is_down = ainput_mouse_button_is_down(current, button);

            if (was_down != is_down)
                ainput_post_button(pInfo, button, is_down);
        }
    }

    memcpy(priv->key_state, current, sizeof(priv->key_state));

    if (priv->is_absolute)
    {
        struct input_absinfo abs_x;
        struct input_absinfo abs_y;

        if (ioctl(pInfo->fd, EVIOCGABS(ABS_X), &abs_x) == 0 &&
            ioctl(pInfo->fd, EVIOCGABS(ABS_Y), &abs_y) == 0)
        {
            priv->abs_x = priv->last_abs_x = abs_x.value;
            priv->abs_y = priv->last_abs_y = abs_y.value;
            priv->has_last_abs = 1;
        }
    }

    priv->sync_dropped = 0;
}

static void ainput_apply_sensitivity_all(float new_sens)
{
    InputInfoPtr pInfo;
    AInputPriv *priv;

    for (pInfo = xf86FirstLocalDevice(); pInfo; pInfo = pInfo->next)
    {
        if (!pInfo->drv || !pInfo->drv->driverName ||
            strcmp(pInfo->drv->driverName, DRIVER_NAME) != 0 ||
            !pInfo->private)
            continue;

        priv = pInfo->private;
        if (priv->type == DEV_MOUSE)
            ainput_apply_sensitivity(priv, new_sens);
    }
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

    if (checkonly)
        return Success;

    if (priv->initializing_property)
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

    ainput_apply_sensitivity_all(new_sens);

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

static void ainput_report_read_end(InputInfoPtr pInfo, ssize_t len)
{
    AInputPriv *priv = pInfo->private;
    int error;

    if (len > 0 ||
        (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
        return;

    error = (len == 0) ? ENODEV : errno;
    if (error == priv->last_read_error)
        return;

    priv->last_read_error = error;
    xf86Msg(X_WARNING,
            "%s: input device stopped producing events (fd=%d, errno=%d: %s)\n",
            pInfo->name, pInfo->fd, error, strerror(error));
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
    int result;

    if (reads != priv->read_budget)
        return;

    do
        result = poll(&poll_fd, 1, 0);
    while (result < 0 && errno == EINTR);

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
            (result > 0 && (poll_fd.revents & POLLIN)) ? "yes" : "no");
}
#endif

static void ainput_read_keyboard(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct input_event events[AINPUT_EVENT_BATCH];
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        len = read(pInfo->fd, events, sizeof(events));
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        size_t count = (size_t)len / sizeof(events[0]);
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += count;
#endif

        for (size_t i = 0; i < count; i++)
        {
            const struct input_event *ev = &events[i];

            if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
            {
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
            {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT)
                    ainput_resync_state(pInfo);
                continue;
            }

            if (ev->type != EV_KEY || ev->value == 2)
                continue;

            int x11_keycode = ev->code + 8;
            if (x11_keycode >= 8 && x11_keycode <= 255)
            {
                ainput_post_key(pInfo, x11_keycode, ev->value != 0, ev);
                ainput_track_key(priv, ev->code, ev->value != 0);
            }
        }

        if ((size_t)len < sizeof(events))
            break;
    }

#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_report_read_end(pInfo, len);
}

static void ainput_read_relative_mouse(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    DeviceIntPtr dev = pInfo->dev;
    ValuatorMask *motion_mask = priv->motion_mask;
    struct input_event events[AINPUT_EVENT_BATCH];
    double sens = priv->effective_sensitivity;
    int acc_x = priv->acc_x;
    int acc_y = priv->acc_y;
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        len = read(pInfo->fd, events, sizeof(events));
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        size_t count = (size_t)len / sizeof(events[0]);
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += count;
#endif

        for (size_t i = 0; i < count; i++)
        {
            const struct input_event *ev = &events[i];

            if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
            {
                acc_x = 0;
                acc_y = 0;
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
            {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT)
                    ainput_resync_state(pInfo);
                continue;
            }

            switch (ev->type)
            {
            case EV_REL:
                switch (ev->code)
                {
                case REL_X:
                    acc_x += ev->value;
                    break;
                case REL_Y:
                    acc_y += ev->value;
                    break;
                case REL_WHEEL:
                    priv->wheel_v_steps += ev->value;
                    priv->scroll_pending = 1;
                    break;
                case REL_HWHEEL:
                    priv->wheel_h_steps += ev->value;
                    priv->scroll_pending = 1;
                    break;
                case REL_WHEEL_HI_RES:
                    priv->wheel_v_hi_res_frame += ev->value;
                    priv->scroll_pending = 1;
                    break;
                case REL_HWHEEL_HI_RES:
                    priv->wheel_h_hi_res_frame += ev->value;
                    priv->scroll_pending = 1;
                    break;
                }
                break;

            case EV_KEY:
            {
                int button = ainput_mouse_button_for_code(ev->code);

                if (button > 0)
                {
                    ainput_post_button(pInfo, button, ev->value != 0);
                    ainput_track_key(priv, ev->code, ev->value != 0);
                }
                break;
            }

            case EV_SYN:
                if (ev->code != SYN_REPORT)
                    break;

                if (priv->scroll_pending)
                {
                    ainput_flush_scroll_axis(pInfo,
                                             &priv->wheel_v_steps,
                                             &priv->wheel_v_hi_res_frame,
                                             &priv->wheel_v_hi_res, 0);
                    ainput_flush_scroll_axis(pInfo,
                                             &priv->wheel_h_steps,
                                             &priv->wheel_h_hi_res_frame,
                                             &priv->wheel_h_hi_res, 1);
                    priv->scroll_pending = 0;
                }

                if (acc_x != 0 || acc_y != 0)
                {
                    ainput_post_relative_motion(priv, dev, motion_mask,
                                                (double)acc_x * sens,
                                                (double)acc_y * sens,
                                                (double)acc_x,
                                                (double)acc_y,
                                                ev);
                    acc_x = 0;
                    acc_y = 0;
                }
                break;
            }
        }

        if ((size_t)len < sizeof(events))
            break;
    }

    priv->acc_x = acc_x;
    priv->acc_y = acc_y;
#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_report_read_end(pInfo, len);
}

static void ainput_read_absolute_mouse(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    DeviceIntPtr dev = pInfo->dev;
    ValuatorMask *motion_mask = priv->motion_mask;
    struct input_event events[AINPUT_EVENT_BATCH];
    ssize_t len = 0;
#ifdef AINPUT_READ_BUDGET_DEBUG
    unsigned int debug_reads = 0;
    size_t debug_events = 0;
#endif

    for (unsigned int batch = 0; batch < priv->read_budget; batch++)
    {
        len = read(pInfo->fd, events, sizeof(events));
        if (len <= 0)
            break;

        priv->last_read_error = 0;
        size_t count = (size_t)len / sizeof(events[0]);
#ifdef AINPUT_READ_BUDGET_DEBUG
        debug_reads++;
        debug_events += count;
#endif

        for (size_t i = 0; i < count; i++)
        {
            const struct input_event *ev = &events[i];

            if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
            {
                ainput_begin_resync(priv);
                continue;
            }

            if (priv->sync_dropped)
            {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT)
                    ainput_resync_state(pInfo);
                continue;
            }

            switch (ev->type)
            {
            case EV_REL:
                switch (ev->code)
                {
                    case REL_X:
                        priv->acc_x += ev->value;
                        break;
                    case REL_Y:
                        priv->acc_y += ev->value;
                        break;
                    case REL_WHEEL:
                        priv->wheel_v_steps += ev->value;
                        priv->scroll_pending = 1;
                        break;

                    case REL_HWHEEL:
                        priv->wheel_h_steps += ev->value;
                        priv->scroll_pending = 1;
                        break;

                    case REL_WHEEL_HI_RES:
                        priv->wheel_v_hi_res_frame += ev->value;
                        priv->scroll_pending = 1;
                        break;

                    case REL_HWHEEL_HI_RES:
                        priv->wheel_h_hi_res_frame += ev->value;
                        priv->scroll_pending = 1;
                        break;
                }
                break;

            case EV_ABS:
                switch (ev->code)
                {
                case ABS_X:
                    priv->abs_x = ev->value;
                    priv->has_abs_event = 1;
                    break;
                case ABS_Y:
                    priv->abs_y = ev->value;
                    priv->has_abs_event = 1;
                    break;
                }
                break;

            case EV_KEY:
            {
                int button = ainput_mouse_button_for_code(ev->code);

                if (button > 0)
                {
                    ainput_post_button(pInfo, button, ev->value != 0);
                    ainput_track_key(priv, ev->code, ev->value != 0);
                }
                break;
            }

            case EV_SYN:
                if (ev->code != SYN_REPORT)
                    break;

                if (priv->scroll_pending)
                {
                    ainput_flush_scroll_axis(pInfo,
                                             &priv->wheel_v_steps,
                                             &priv->wheel_v_hi_res_frame,
                                             &priv->wheel_v_hi_res, 0);
                    ainput_flush_scroll_axis(pInfo,
                                             &priv->wheel_h_steps,
                                             &priv->wheel_h_hi_res_frame,
                                             &priv->wheel_h_hi_res, 1);
                    priv->scroll_pending = 0;
                }

                double sens = priv->effective_sensitivity;

                if (priv->acc_x != 0 || priv->acc_y != 0)
                {
                    double dx = (double)priv->acc_x * sens;
                    double dy = (double)priv->acc_y * sens;

                    ainput_post_relative_motion(priv, dev, motion_mask, dx, dy,
                                                (double)priv->acc_x,
                                                (double)priv->acc_y,
                                                ev);

                    priv->acc_x = 0;
                    priv->acc_y = 0;
                }

                if (priv->has_abs_event)
                {
                    if (!priv->has_last_abs)
                    {
                        priv->last_abs_x = priv->abs_x;
                        priv->last_abs_y = priv->abs_y;
                        priv->has_last_abs = 1;
                        priv->has_abs_event = 0;
                        continue;
                    }

                    int delta_x = priv->abs_x - priv->last_abs_x;
                    int delta_y = priv->abs_y - priv->last_abs_y;

                    priv->last_abs_x = priv->abs_x;
                    priv->last_abs_y = priv->abs_y;

                    if (delta_x == 0 && delta_y == 0)
                    {
                        priv->has_abs_event = 0;
                        break;
                    }

                    double step_x = (double)delta_x * sens;
                    double step_y = (double)delta_y * sens;

                    ainput_post_relative_motion(priv, dev, motion_mask,
                                                step_x, step_y,
                                                (double)delta_x,
                                                (double)delta_y,
                                                ev);

                    priv->has_abs_event = 0;
                }
                break;
            }
        }

        if ((size_t)len < sizeof(events))
            break;
    }

#ifdef AINPUT_READ_BUDGET_DEBUG
    ainput_debug_read_budget(pInfo, debug_reads, debug_events);
#endif
    ainput_report_read_end(pInfo, len);
}

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
        BYTE map[32] = {0};
        for (int i = 1; i < 32; i++)
            map[i] = i;

        Atom btn_labels[32] = {0};
        Atom axes_labels[2] = {0};
        axes_labels[0] = MakeAtom(priv->is_absolute ? "Abs X" : "Rel X", 5, TRUE);
        axes_labels[1] = MakeAtom(priv->is_absolute ? "Abs Y" : "Rel Y", 5, TRUE);

        if (!InitPointerDeviceStruct((DevicePtr)dev, map, 32, btn_labels,
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

        if (!priv->motion_mask)
        {
            priv->motion_mask = valuator_mask_new(2);
            if (!priv->motion_mask)
                return BadAlloc;
        }

        priv->prop_sensitivity = MakeAtom(PROP_SENSITIVITY, strlen(PROP_SENSITIVITY), TRUE);
        XIRegisterPropertyHandler(dev, ainput_change_property, NULL, NULL);

        float init_val = priv->sensitivity;

        priv->initializing_property = 1;
        XIChangeDeviceProperty(dev, priv->prop_sensitivity, XA_FLOAT, 32,
                               PropModeReplace, 1, &init_val, FALSE);
        priv->initializing_property = 0;

        XISetDevicePropertyDeletable(dev, priv->prop_sensitivity, FALSE);
    }
    return Success;
}

static int ainput_device_close(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    AInputPriv *priv = pInfo ? pInfo->private : NULL;
    int server_managed = ainput_fd_is_server_managed(pInfo);

    if (priv && priv->motion_mask)
        valuator_mask_free(&priv->motion_mask);

    if (pInfo && pInfo->fd != -1 && !server_managed)
    {
        xf86CloseSerial(pInfo->fd);
        pInfo->fd = -1;
    }

    if (priv)
        priv->fd = -1;

    return Success;
}

static int ainput_control(DeviceIntPtr dev, int what)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    AInputPriv *priv = pInfo->private;

    switch (what)
    {
    case DEVICE_INIT:
        return ainput_device_init(dev);

    case DEVICE_ON:
        if (pInfo->fd != -1)
        {
            priv->fd = pInfo->fd;
            priv->last_read_error = 0;
            xf86AddEnabledDevice(pInfo);
        }
        dev->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
        if (pInfo->fd != -1)
            xf86RemoveEnabledDevice(pInfo);
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

    return (isnan(value) || value <= 0.0f) ? fallback : value;
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

    priv->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (priv->fd < 0)
    {
        xf86Msg(X_ERROR, "%s: failed to open %s (errno: %d)\n", pInfo->name, path, errno);
        return BadValue;
    }

    pInfo->fd = priv->fd;
    return Success;
}

static void ainput_read_options(InputInfoPtr pInfo, AInputPriv *priv)
{
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
}

static void ainput_read_evbits(int fd, unsigned long evbits[NBITS(EV_MAX)])
{
    memset(evbits, 0, sizeof(unsigned long) * NBITS(EV_MAX));
    ioctl(fd, EVIOCGBIT(0, sizeof(unsigned long) * NBITS(EV_MAX)), evbits);
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

static void ainput_detect_absolute_axes(AInputPriv *priv,
                                        const unsigned long evbits[NBITS(EV_MAX)])
{
    unsigned long absbits[NBITS(ABS_MAX)] = {0};
    struct input_absinfo abs_x;
    struct input_absinfo abs_y;

    if (priv->type != DEV_MOUSE || !BIT_IS_SET(evbits, EV_ABS))
        return;

    unsigned long relbits[NBITS(REL_MAX)] = {0};
    if (BIT_IS_SET(evbits, EV_REL))
    {
        ioctl(priv->fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
        if (BIT_IS_SET(relbits, REL_X) || BIT_IS_SET(relbits, REL_Y))
            return;
    }

    ioctl(priv->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    if (!BIT_IS_SET(absbits, ABS_X) || !BIT_IS_SET(absbits, ABS_Y))
        return;

    if (ioctl(priv->fd, EVIOCGABS(ABS_X), &abs_x) != 0 ||
        ioctl(priv->fd, EVIOCGABS(ABS_Y), &abs_y) != 0)
        return;

    priv->is_absolute = 1;
    priv->min_x = abs_x.minimum;
    priv->max_x = abs_x.maximum;
    priv->min_y = abs_y.minimum;
    priv->max_y = abs_y.maximum;
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

static void ainput_log_pre_init(InputInfoPtr pInfo, const AInputPriv *priv)
{
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
}

static int ainput_pre_init(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    AInputPriv *priv = calloc(1, sizeof(AInputPriv));
    if (!priv)
        return BadAlloc;

    priv->fd = -1;

    xf86CollectInputOptions(pInfo, NULL);
    ainput_read_options(pInfo, priv);

    if (ainput_open_device(pInfo, priv) != Success)
    {
        free(priv);
        return BadValue;
    }

    unsigned long evbits[NBITS(EV_MAX)] = {0};
    ainput_read_evbits(priv->fd, evbits);

    priv->type = ainput_detect_type(pInfo, evbits);
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

    if (priv)
    {
        if (priv->motion_mask)
            valuator_mask_free(&priv->motion_mask);

        if (priv->fd >= 0 && !ainput_fd_is_server_managed(pInfo))
            close(priv->fd);

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
    xf86AddInputDriver(&AINPUT_DRIVER, module, 0);
    return module;
}

_X_EXPORT XF86ModuleData ainputModuleData = {
    .vers = &ainput_version_info,
    .setup = ainput_setup,
    .teardown = NULL,
};
