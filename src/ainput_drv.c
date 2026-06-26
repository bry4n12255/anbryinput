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
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

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
 * Experimental Xorg/XLibre fast path.
 *
 * These only work with patched Xorg/XLibre servers that export the matching
 * symbol. Keep them disabled for normal builds.
 *
 * Enable the safer fast path with:
 *   make XSERVER_FAST_REL2D=1
 *
 * Enable the more aggressive direct path with:
 *   make XSERVER_DIRECT_REL2D=1
 */
#ifdef AINPUT_XSERVER_DIRECT_REL2D
extern void QueueAInputRelativeMotion2D(DeviceIntPtr pDev, double dx, double dy);
#endif

#ifdef AINPUT_XSERVER_FAST_REL2D
extern void QueuePointerRelativeMotion2D(DeviceIntPtr pDev, double dx, double dy);
#endif

#define DRIVER_NAME "ainput"
#define DRIVER_VERSION 1.1

#define PROP_SENSITIVITY "AInput Sensitivity"
#define AINPUT_EVENT_BATCH 16
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

typedef enum
{
    DEV_KEYBOARD,
    DEV_MOUSE
} ADevType;

typedef struct
{
    const char *xkb_layout, *xkb_variant;
    ValuatorMask *motion_mask;
    double effective_sensitivity;

    float sensitivity, dpi, reference_dpi;

    Atom prop_sensitivity;
    ADevType type;

    int fd, initializing_property;
    int acc_x, acc_y;
    int is_absolute;
    int has_abs_event;
    int abs_x, abs_y;
    int min_x, max_x;
    int min_y, max_y;

    int has_last_abs;
    int last_abs_x, last_abs_y;
} AInputPriv;

static void ainput_fd_handler(int fd, int ready, void *data)
{
    InputInfoPtr pInfo = data;
    if (pInfo && pInfo->read_input)
        pInfo->read_input(pInfo);
}

static inline void ainput_update_effective_sensitivity(AInputPriv *priv)
{
    if (priv->is_absolute || priv->dpi <= 0.0f || priv->reference_dpi <= 0.0f)
        priv->effective_sensitivity = (double)priv->sensitivity;
    else
        priv->effective_sensitivity = (double)priv->sensitivity *
                                      ((double)priv->reference_dpi / (double)priv->dpi);
}

static void ainput_apply_sensitivity(AInputPriv *priv, float new_sens)
{
    priv->sensitivity = new_sens;
    ainput_update_effective_sensitivity(priv);
}

/*
    Make sure you compiled XLibre or Xorg with the proper patches
*/
static inline void ainput_post_relative_motion(InputInfoPtr pInfo, double dx, double dy)
{
#ifdef AINPUT_XSERVER_DIRECT_REL2D
    QueueAInputRelativeMotion2D(pInfo->dev, dx, dy);
#elif defined(AINPUT_XSERVER_FAST_REL2D)
    QueuePointerRelativeMotion2D(pInfo->dev, dx, dy);
#else
    ValuatorMask *mask = ((AInputPriv *)pInfo->private)->motion_mask;

    valuator_mask_zero(mask);
    valuator_mask_set_double(mask, 0, dx);
    valuator_mask_set_double(mask, 1, dy);

    QueuePointerEvents(pInfo->dev, MotionNotify, 0, POINTER_RELATIVE, mask);
#endif
}

static inline void ainput_post_button(InputInfoPtr pInfo, int button, int is_down)
{
    ValuatorMask *mask = ((AInputPriv *)pInfo->private)->motion_mask;

    valuator_mask_zero(mask);
    QueuePointerEvents(pInfo->dev, is_down ? ButtonPress : ButtonRelease,
                       button, 0, mask);
}

static inline void ainput_post_key(InputInfoPtr pInfo, int key_code, int is_down)
{
    QueueKeyboardEvents(pInfo->dev, is_down ? KeyPress : KeyRelease, key_code);
}

static void ainput_apply_sensitivity_all(float new_sens)
{
    InputInfoPtr pInfo;

    for (pInfo = xf86FirstLocalDevice(); pInfo; pInfo = pInfo->next)
    {
        AInputPriv *priv;

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

    if (isnan(new_sens) || new_sens < 0.01f || new_sens > 20.0f)
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

static void ainput_read_keyboard(InputInfoPtr pInfo)
{
    struct input_event events[AINPUT_EVENT_BATCH];
    ssize_t len;

    while ((len = read(pInfo->fd, events, sizeof(events))) > 0)
    {
        size_t count = (size_t)len / sizeof(events[0]);

        for (size_t i = 0; i < count; i++)
        {
            const struct input_event *ev = &events[i];

            if (ev->type != EV_KEY || ev->value == 2)
                continue;

            int x11_keycode = ev->code + 8;
            if (x11_keycode >= 8 && x11_keycode <= 255)
                ainput_post_key(pInfo, x11_keycode, ev->value != 0);
        }
    }
}

static void ainput_read_mouse(InputInfoPtr pInfo)
{
    AInputPriv *priv = pInfo->private;
    struct input_event events[AINPUT_EVENT_BATCH];
    ssize_t len;

    while ((len = read(pInfo->fd, events, sizeof(events))) > 0)
    {
        size_t count = (size_t)len / sizeof(events[0]);

        for (size_t i = 0; i < count; i++)
        {
            const struct input_event *ev = &events[i];

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
                    {
                        int button = (ev->value > 0) ? 4 : 5;
                        ainput_post_button(pInfo, button, 1);
                        ainput_post_button(pInfo, button, 0);
                        break;
                    }
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
                int button = 0;
                switch (ev->code)
                {
                case BTN_LEFT:   button = 1; break;
                case BTN_MIDDLE: button = 2; break;
                case BTN_RIGHT:  button = 3; break;
                case BTN_SIDE:   button = 8; break;
                case BTN_EXTRA:  button = 9; break;
                case BTN_TOUCH:  button = 1; break;
                }
                if (button > 0)
                    ainput_post_button(pInfo, button, ev->value != 0);
                break;
            }

            case EV_SYN:
                if (ev->code != SYN_REPORT)
                    break;

                double sens = priv->effective_sensitivity;

                if (priv->acc_x != 0 || priv->acc_y != 0)
                {
                    double dx = (double)priv->acc_x * sens;
                    double dy = (double)priv->acc_y * sens;

                    ainput_post_relative_motion(pInfo, dx, dy);

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

                    ainput_post_relative_motion(pInfo, step_x, step_y);

                    priv->has_abs_event = 0;
                }
                break;
            }
        }
    }
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

    if (priv && priv->motion_mask)
        valuator_mask_free(&priv->motion_mask);

    if (pInfo && pInfo->fd != -1)
    {
        xf86CloseSerial(pInfo->fd);
        if (priv)
            priv->fd = -1;
        pInfo->fd = -1;
    }
    return Success;
}

static int ainput_control(DeviceIntPtr dev, int what)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;

    switch (what)
    {
    case DEVICE_INIT:
        return ainput_device_init(dev);

    case DEVICE_ON:
        if (pInfo->fd != -1)
        {
            SetNotifyFd(pInfo->fd, ainput_fd_handler, X_NOTIFY_READ, pInfo);
        }
        dev->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
        if (pInfo->fd != -1)
            RemoveNotifyFd(pInfo->fd);
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
    priv->xkb_layout = xf86FindOptionValue(pInfo->options, "xkb_layout");
    priv->xkb_variant = xf86FindOptionValue(pInfo->options, "xkb_variant");

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
    pInfo->read_input = (priv->type == DEV_MOUSE) ? ainput_read_mouse : ainput_read_keyboard;
    pInfo->device_control = ainput_control;
    pInfo->flags = XI86_ALWAYS_CORE;
    pInfo->type_name = (priv->type == DEV_MOUSE) ? XI_MOUSE : XI_KEYBOARD;
}

static void ainput_log_pre_init(InputInfoPtr pInfo, const AInputPriv *priv)
{
    if (priv->type == DEV_MOUSE)
    {
        xf86Msg(
            X_INFO,
            "%s: AInput mouse initialized, sensitivity=%.3f dpi=%.1f reference_dpi=%.1f effective=%.3f\n",
            pInfo->name,
            priv->sensitivity,
            priv->dpi,
            priv->reference_dpi,
            priv->effective_sensitivity);
    }
    else
    {
        xf86Msg(
            X_INFO,
            "%s: AInput keyboard initialized, layout='%s', variant='%s'\n",
            pInfo->name,
            priv->xkb_layout ? priv->xkb_layout : AINPUT_DEFAULT_LAYOUT,
            priv->xkb_variant ? priv->xkb_variant : "default");
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

    unsigned int clk = CLOCK_MONOTONIC;
    ioctl(priv->fd, EVIOCSCLOCKID, &clk);

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
        if (priv->fd >= 0)
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
    DRIVER_NAME, MODULEVENDORSTRING, MODINFOSTRING1, MODINFOSTRING2, XORG_VERSION_CURRENT, 1, 0, 0, ABI_CLASS_XINPUT, ABI_XINPUT_VERSION, MOD_CLASS_XINPUT, {0, 0, 0, 0}};

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
