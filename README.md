# AnbryInput

AnbryInput is a small experimental Xorg/XLibre input driver focused on
low-latency mouse and keyboard input. The Xorg driver module is named `ainput`.
The current release is `1.9.0`.

It is not a full libinput replacement. It intentionally does less work:
no touchpad gestures, no tablet handling, no adaptive acceleration pipeline, and no
Wayland support for now. The goal is a short, predictable path from Linux evdev
to Xorg.

## Features

- Relative mouse movement through evdev
- Mouse buttons through the standard evdev mouse range, plus vertical,
  horizontal, and high-resolution wheel input
- Basic keyboard events
- Linear mouse sensitivity
- Optional DPI normalization
- Optional io_uring input backend with fixed files, registered buffers,
  one-shot reads, and provided-buffer multishot reads
- XInput property for live sensitivity changes
- Small latency benchmark tool for XI_RawMotion and XI_Motion

## Limitations

- Xorg/XLibre only
- No Wayland support
- No proper touchpad support
- No proper tablet/stylus support
- No multitouch, gestures, palm rejection, or tablet pressure/tilt
- Games that read `/dev/input/event*` directly may bypass this driver
- Experimental: keep another input path, TTY, or rescue session available

## Compatibility

AnbryInput is built against the local Xorg/XLibre server headers through
`pkg-config xorg-server`.

Current source and patch validation:

- XLibre `25.1.9` — XInput driver ABI `26.0`
- Xorg Server `21.1.24` — XInput driver ABI `24.4`

Check your local input ABI with:

```sh
pkg-config --variable=abi_xinput xorg-server
```

Input drivers are ABI-sensitive. If your Xorg/XLibre server uses a different
XInput ABI, rebuild AnbryInput against that server's development headers.

## Build

Dependencies include a C compiler, `pkg-config`, Xorg/XLibre server development
headers, and X11/XInput development libraries for the benchmark tool.

Generic requirements:

- C compiler
- `make`
- `pkg-config`
- Xorg/XLibre server development headers
- X11 development headers
- XInput development headers
- Linux input headers
- liburing development headers and library (optional; enables io_uring)

Package examples:

<details>
<summary>Arch / CachyOS / Artix with Xorg</summary>

```sh
sudo pacman -S base-devel pkgconf xorg-server-devel libx11 libxi liburing
```

</details>

<details>
<summary>Arch / CachyOS / Artix with XLibre</summary>

```sh
sudo pacman -S base-devel pkgconf xlibre-xserver-devel libx11 libxi liburing
```

</details>

<details>
<summary>Debian / Ubuntu</summary>

```sh
sudo apt install build-essential pkg-config xserver-xorg-dev libx11-dev libxi-dev x11proto-dev liburing-dev
```

</details>

<details>
<summary>Fedora</summary>

```sh
sudo dnf install gcc make pkgconf-pkg-config xorg-x11-server-devel libX11-devel libXi-devel xorg-x11-proto-devel liburing-devel
```

</details>

<details>
<summary>openSUSE</summary>

```sh
sudo zypper install gcc make pkgconf-pkg-config xorg-x11-server-sdk libX11-devel libXi-devel liburing-devel
```

</details>

<details>
<summary>Void Linux</summary>

```sh
sudo xbps-install -S base-devel pkg-config xorg-server-devel libX11-devel libXi-devel liburing-devel
```

</details>

Build:

```sh
make
```

liburing is optional. `IO_URING=auto` is the default and enables the backend
when the required APIs are available. Use `IO_URING=0` for a read-only build,
or `IO_URING=1` to require liburing and fail if it is unavailable:

```sh
make IO_URING=0
make IO_URING=1
```

Build artifacts carry a configuration signature. Changing backend support,
direct mode, optimization flags, compiler, server ABI, or public headers
rebuilds the affected artifacts without requiring `make clean`.

Build optimized for the current CPU:

```sh
make NATIVE=1
```

Build with more aggressive compiler optimization:

```sh
make NATIVE=1 AGGRESSIVE=1
```

Build the latency tool:

```sh
make tools
```
`make latency-tool` is also available as an alias.

The implemented bottleneck analysis, tradeoffs, compatibility checks, and
validation details are documented in [PERFORMANCE.md](PERFORMANCE.md).

### Build with Experimental Xorg/XLibre Patches

AnbryInput can optionally use experimental Xorg/XLibre patches that provide
specialized paths for relative motion and keyboard-event construction.

These patches are **not part of upstream Xorg/XLibre**. They must be applied
when building your X server. If they are not present, simply build AnbryInput
normally.

The experimental patch files are:

- `patches/xlibre-ainput-direct-experimental.patch` provides timestamped
  relative-motion, key, and button entry points
- `patches/xorg-ainput-direct-experimental.patch` provides the same entry
  points for Xorg

Two additional optimized variants are temporarily available:

- `patches/xlibre-ainput-direct-optimized-experimental.patch`
- `patches/xorg-ainput-direct-optimized-experimental.patch`

Development builds can additionally apply the matching hot-reload patch after
the direct-path patch:

- `patches/xorg-input-driver-hot-reload-development.patch`
- `patches/xlibre-input-driver-hot-reload-development.patch`

They reload newly installed input-driver code on SIGUSR2; see
[Development hot reload](#development-hot-reload).

These are complete alternatives to the corresponding direct patches, not
patches to apply on top of them. Apply only one patch to a clean X server
source tree. The optimized variants preserve the same event path while
reducing `ValuatorMask` setup for AnbryInput relative motion.

All four patches apply cleanly and compile on the releases listed above.

Each patch is focused on the latest stable release of its respective X server.

Buttons retain the server's standard pointer-event construction, grab and
button-state handling. Current patches expose a timestamped entry point for
that path.

#### Direct AnbryInput Path

Uses `QueueAInputRelativeMotion2DRawAtTime`, `QueueAInputKeyAtTime` and
`QueueAInputButtonAtTime`, written specifically for AnbryInput. The motion
helper skips generic setup while retaining the server's positioning, barrier,
confinement, history, master/slave and MIEQ processing. Keys and buttons retain
the normal XKB, focus, grab, master-device and XI2 behavior.

The direct patches add `AtTime` entry points for keys, relative motion and
buttons. The driver samples `GetTimeInMillis()` once at each `SYN_REPORT` and
uses that value for motion and every discrete event in the report. On Xorg this
follows the server-selected `CLOCK_MONOTONIC_COARSE`, avoiding a precise evdev
timestamp that can be slightly ahead of the server clock. Existing helper
names remain available and use the X server's current time too. A timestamp
that cannot safely enter the optimized key queue falls back to the generic
keyboard helper and retains its queue policy.

A driver built with `XSERVER_DIRECT=1` calls the three timestamped helpers
directly, without runtime symbol checks in the event path. It therefore
requires one of the current patches in this repository. Use a normal build for
an unpatched server.

```sh
make XSERVER_DIRECT=1
```

These options can also be combined with compiler optimizations:

```sh
make NATIVE=1 AGGRESSIVE=1 XSERVER_DIRECT=1
```

## Install

Install the driver into the Xorg/XLibre input module directory:

```sh
sudo make install
```

or

```sh
sudo make NATIVE=1 XSERVER_DIRECT=1 install
```

or

```sh
sudo make NATIVE=1 AGGRESSIVE=1 XSERVER_DIRECT=1 install
```

Restart Xorg/XLibre after installing. Input drivers are loaded into the server
process and are not reloaded just because the `.so` file changed on disk.

Uninstall:

```sh
sudo make uninstall
```

## Configuration

Prefer matching only the exact devices you want AnbryInput to control. A broad
`MatchIsPointer "on"` rule may catch touchpads or tablets, which AnbryInput does
not handle properly.

Find device names:

```sh
xinput list
```

Find stable device paths:

```sh
ls -l /dev/input/by-id/
```

Example `InputClass` mouse config:

```conf
Section "InputClass"
    Identifier "AnbryInput Mouse"
    MatchProduct "YOUR_MOUSE_NAME_HERE"
    MatchIsPointer "on"
    MatchDevicePath "/dev/input/event*"
    Driver "ainput"

    Option "Backend" "read"
    Option "Type" "mouse"
    Option "Sensitivity" "1.0"
    Option "DPI" "1000"
    Option "ReferenceDPI" "1000"

    # Optional low-latency backend. The traditional read backend is default.
    # Option "Backend" "io_uring"
    # Option "ReadMultishot" "on"
    # Option "SQPoll" "on"
    # Option "SQPollIdle" "50"
EndSection
```

Example keyboard config:

```conf
Section "InputClass"
    Identifier "AnbryInput Keyboard"
    MatchProduct "YOUR_KEYBOARD_NAME_HERE"
    MatchIsKeyboard "on"
    Driver "ainput"

    Option "Backend" "read"
    Option "Type" "keyboard"
    Option "xkb_layout" "us"
EndSection
```

The repository also includes [99-ainput.conf](99-ainput.conf), but treat it as a
starting point. Device names and event paths differ between systems.

Default options:

| Option | Default | Notes |
| --- | --- | --- |
| `Backend` | `read` | Use `io_uring` to enable the optional ring backend. |
| `Type` | auto-detected | Use `mouse` or `keyboard` to avoid ambiguous devices. |
| `Sensitivity` | `1.0` | Runtime changes are exposed as `AInput Sensitivity`. |
| `DPI` | `1000` | Used for relative mouse DPI normalization. |
| `ReferenceDPI` | `1000` | Baseline DPI for the sensitivity formula. |
| `ReadBudget` | `1` | Fairness limit for ready batches consumed per X server callback: `1`, `2`, `4`, or `8`. Applies to both backends; normally leave it unset. |
| `SQPoll` | `off` | io_uring only. Keeps a kernel submission thread active to reduce wakeup latency at a substantial CPU cost. |
| `SQPollIdle` | `50` | Milliseconds before the SQPOLL thread sleeps. Range: `1`–`60000`. |
| `SQPollCPU` | unset | Optional logical CPU for the SQPOLL thread; `-1` leaves placement to the kernel. |
| `ReadMultishot` | `off` | Use provided-buffer multishot reads. Falls back to direct one-shot reads if the buffer ring is unavailable. |
| `IoUringDebug` | `off` | Log bounded CQE traces, lifecycle state, cancellation results, counters, and invariant failures. |
| `xkb_layout` | `us` | Keyboard layout fallback. |
| `xkb_variant` | unset | Example for Brazilian ABNT2: `abnt2`. |

Multishot uses a registered provided-buffer ring. If ring creation fails, the
driver logs the failure and falls back to the traditional `read` backend. If
multishot setup alone is unavailable, it falls back to direct one-shot reads.
SQPOLL is separately opt-in because its lower measured latency comes with
continuous CPU use while its kernel thread is active.

For kernel-level io_uring diagnosis, enable `Option "IoUringDebug" "on"` in
the matching `InputClass`. The driver logs the first 16 CQEs, later
power-of-two CQE milestones, every negative CQE, ring setup features,
watch registration, cancellation state, and a teardown summary. Bounded
sampling keeps high-polling-rate devices from filling the log. Error and
invariant messages that identify malformed CQEs remain visible even when the
debug option is off.

## Read Budget

`ReadBudget` is an internal per-device fairness limit, not the device polling
rate. It also applies to io_uring: each CQE can contain a batch of up to 256
Linux input events, and the budget prevents one continuously busy ring from
holding the X server's event loop indefinitely. Remaining CQEs keep the ring
readable, so Xorg/XLibre invokes the driver again.

The traditional backend attempts another nonblocking `read()` when the
previous array was completely full. The io_uring backend consumes only CQEs
that are already ready. Both stop after `ReadBudget` batches or as soon as no
more data is immediately available; neither waits for a future event in the
callback. The default value of `1` needs no configuration and is recommended
for normal use.

A larger value can drain an accumulated high-polling-rate mouse backlog with
fewer Xserver wakeups. The tradeoff is that the mouse callback can process and
queue more events before the Xserver services the keyboard, another pointer,
or other work. Values such as `4` and `8` can therefore hurt fairness under a
sustained input flood even though they do not normally change anything when
the first read contains fewer than 256 events.

Recommended starting values:

| Device/workload | `ReadBudget` |
| --- | --- |
| Keyboard | `1` |
| Mouse up to 1 kHz | `1` |
| Mouse from 2 kHz through 8 kHz | `2` |
| Backlog/throughput experiments | `4` or `8` |

The accepted values are `1`, `2`, `4`, and `8`. Omitting the option uses the
driver default of `1`; it does not disable input. Configure it in each
device's `InputClass` when different devices need different limits.

To check whether events remain available after a callback exhausts its read
budget, build the driver with the compile-time diagnostic enabled:

```sh
make clean
make READ_BUDGET_DEBUG=1
```

After the configured number of successful reads, the diagnostic polls the FD
without consuming another event and produces an entry in `Xorg.0.log`:

```text
ReadBudget debug: reads=1/1 events=256 more_events=yes
```

`more_events=yes` means that the budget ended while the device was still
readable, so a larger value could drain more of that backlog in the same
callback. `more_events=no` means no additional event was immediately available
at that snapshot. Rebuild without `READ_BUDGET_DEBUG=1` after testing: the
extra `poll()` and logging alter timing and invalidate latency measurements.

## Sensitivity And DPI

AnbryInput applies a simple linear multiplier:

```text
effective = Sensitivity * (ReferenceDPI / DPI)
```

The multiplier is applied to normal pointer motion only. `XI_RawMotion`
retains the original hardware counts, so games can apply their own fractional
sensitivity without receiving sub-count deltas that some engines truncate to
zero. Consequently, games using raw input are not affected by AnbryInput's
`Sensitivity`, `DPI`, or `ReferenceDPI` settings.

For example:

```conf
Option "Sensitivity" "1.0"
Option "DPI" "1600"
Option "ReferenceDPI" "1000"
```

gives:

```text
effective = 1.0 * (1000 / 1600) = 0.625
```

You can change sensitivity live with XInput:

```sh
xinput set-prop <device-id> "AInput Sensitivity" 0.5
```

## Latency Benchmark

Build the tool:

```sh
make tools
```

Run against your mouse event device and XInput device id:

```sh
sudo ./tools/mouse_latency_xi2 \
  --event /dev/input/eventX \
  --device-id <device-id> \
  --mode raw \
  --samples 10000
```

Motion path:

```sh
sudo ./tools/mouse_latency_xi2 \
  --event /dev/input/eventX \
  --device-id <device-id> \
  --mode motion \
  --samples 10000
```

The tool measures the time between a Linux evdev motion frame and the matching
XInput2 event reaching the benchmark process. It reports percentiles such as
`p50_ms`, `p95_ms`, and `p99_ms`. Prefer percentiles over `max_ms`; isolated max
spikes can come from scheduling noise or test timing.

The observer requires `EVIOCSCLOCKID(CLOCK_MONOTONIC)`, resolves the supplied
evdev node to exactly one physical XI2 device, and suppresses all percentiles
after `SYN_DROPPED`, overflow, a value mismatch, an impossible timestamp, or a
missing counterpart. Raw mouse and keyboard events are value-checked. Cooked
motion and manual raw motion are reported as estimates because repeated deltas
can hide a lost event.

When samples are saved, the CSV also contains the evdev monotonic timestamp,
the 32-bit XI2/X server timestamp, and the monotonic time at which the client
received the event. These columns help distinguish an X server backlog from a
benchmark-client scheduling pause. The X server value uses its coarse clock,
so its two partial intervals can be negative or quantized by several
milliseconds; their sum remains the precise end-to-end `latency_ms` value.

The dashboards read the timestamp CSV produced by the current benchmark. Open
[`tools/latency_samples/.latency_dashboard.html`](tools/latency_samples/.latency_dashboard.html)
for the distribution and spike views, or
[`tools/latency_samples/latency_timestamp_dashboard.html`](tools/latency_samples/latency_timestamp_dashboard.html)
for the approximate evdev-to-Xorg and Xorg-to-client split. Select or drop a
CSV after opening either page.

The analyzer reports tails, repeated spike spacing, autocorrelation, and
backlog-like descending runs. Compare only runs collected with the same
scheduler, affinity, power settings, and background workload.

To compare CPU work in Xorg/XLibre, run the latency tool while attaching
`perf stat` to the server process from another terminal:

```sh
sudo perf stat \
  -e cycles,instructions,branches,branch-misses,cache-misses \
  -p "$(pidof Xorg)" -- sleep 15
```

Use the same sample count and test duration for both builds, then divide the
reported cycles and instructions by the number of matched samples. These
figures include other work performed by the X server during the interval, so
compare alternating runs on the same session and avoid moving windows or
running unrelated clients. End-to-end `p50`, `p95`, and `p99` from
`mouse_latency_xi2` remain the deciding measurements; fewer instructions in
the driver are useful only when they reduce those delivery times or CPU use.

## Troubleshooting

Check that Xorg/XLibre loaded AnbryInput:

```sh
grep -i ainput ~/.local/share/xorg/Xorg.0.log
```

List properties:

```sh
xinput list-props <device-id>
```

If `AInput Sensitivity` is missing, verify that the correct device matched your
`InputClass` and that Xorg was restarted after installing.

If sensitivity changes with `xinput set-prop` but movement does not change,
check that the device you are changing is the device that actually moves the
cursor.

### Restarting a device

An existing AnbryInput device can be restarted through the standard XInput
`Device Enabled` lifecycle:

```sh
xinput disable <device-id>
xinput enable <device-id>
```

This runs the driver's `DEVICE_OFF` and `DEVICE_ON` handlers. It clears runtime
input state and removes and restores the event reader. The X server releases
pressed keys/buttons and sends its normal disabled/enabled hierarchy events;
clients that held a grab on this device must handle that lifecycle normally.

Xorg/XLibre clears a disabled slave's master association. On a multi-master
setup, note the current master ID before the restart and restore it afterward:

```sh
xinput list --short
xinput disable <slave-id>
xinput enable <slave-id>
xinput reattach <slave-id> <previous-master-id>
```

The last command is unnecessary when the slave belongs on the default core
master chosen by the server.

After EOF or a permanent read error on a path opened by AnbryInput, the driver
now closes the stale descriptor. The next disable/enable cycle reopens the
configured `Device` path. Prefer a stable `/dev/input/by-id/...` path when the
configuration supplies `Option "Device"`; an `/dev/input/eventN` number may
refer to another device after reconnection.

With server-managed FDs, udev/logind owns opening, closing, removal, and
recreation. AnbryInput deliberately does not close or reopen those descriptors.
Unplugging and reconnecting the hardware lets Xorg/XLibre hotplug recreate the
physical slave. `xinput reattach` only moves an existing slave between masters,
and `xinput create-master` creates virtual master devices; neither command
recreates a missing physical input device.

The disable/enable cycle does not reload `ainput_drv.so`. A normal Xorg/XLibre
build therefore requires a server restart after installing a changed driver
binary.

### Development hot reload

The separate hot-reload patches target Xorg 21.1.24 and XLibre 25.1.9. They
make the server unload the old module and execute a newly installed
`ainput_drv.so` without restarting the X session. Apply only the patch matching
your server; neither patch changes AnbryInput itself.

> **Warning:** Hot reload is not recommended for normal use. ABI, internal
> structures, callbacks, and retained server state can change between builds,
> causing input loss or a server crash. Use it only for development, with a
> TTY or remote shell available for recovery. Restart the X server when
> validating a build for normal use.

Apply the matching direct-path patch first and the hot-reload patch second.
For Xorg:

```sh
git apply /path/to/anbryinput/patches/xorg-ainput-direct-optimized-experimental.patch
git apply /path/to/anbryinput/patches/xorg-input-driver-hot-reload-development.patch
```

For XLibre:

```sh
git apply /path/to/anbryinput/patches/xlibre-ainput-direct-optimized-experimental.patch
git apply /path/to/anbryinput/patches/xlibre-input-driver-hot-reload-development.patch
```

Build and install the server, then add this argument to its command line once:

```text
-inputdevreload ainput
```

After changing the driver, atomically install the new module and signal the
running Xorg process:

```sh
make NATIVE=1 XSERVER_DIRECT=1
sudo make NATIVE=1 XSERVER_DIRECT=1 install
sudo kill -USR2 "$(pgrep -n Xorg)"
```

The signal handler only records the request. The server main loop performs the
reload while holding the input mutex: it snapshots every device using the
driver, rereads the Xorg configuration files, removes the devices, unloads the
module with `dlclose()`, loads the new file, and recreates the devices. Options
contributed by the old matching `InputClass` sections are discarded and the
new on-disk `InputClass` configuration is applied. It restores enabled state,
device attributes, custom master attachment, and floating state.

This means `Backend`, `ReadMultishot`, `SQPoll`, sensitivity, and other driver
options can be changed in `xorg.conf.d` immediately before sending `SIGUSR2`.
If parsing the updated configuration fails, the hot reload aborts before
removing or unloading any input device.

The recreated slaves receive new XInput device IDs and active grabs on the old
IDs end as part of normal device removal. Runtime XInput properties, including
a sensitivity value changed after startup, return to their configured values.
If the new module cannot load or a device cannot initialize, Xorg logs the
failure and the affected device remains removed; keep a second input path or
remote shell available while testing unfinished driver code.

`SYN_DROPPED` means the kernel evdev queue overflowed before the X server read
it. The driver retires the
active read backend, discards all old events until the fd reaches `EAGAIN`,
then reconstructs key/button and absolute-axis state before registering input
again. This prevents a state snapshot from overtaking reports already queued
in the fd or io_uring. If the state ioctl fails, it releases its tracked state
and resumes instead of freezing the device. Repeated drops at
8000 Hz still indicate that the server was unable to drain input promptly. A
read callback handles at most `256 * ReadBudget` events so a continuously busy
device cannot indefinitely starve window, focus, rendering, or DPMS work in
the X server. Compare 1000 Hz
and 8000 Hz and check CPU, scheduler, GPU-driver, and suspend behavior when
reporting the issue.
