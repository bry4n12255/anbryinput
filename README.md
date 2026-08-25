# AnbryInput

AnbryInput is a small experimental Xorg/XLibre input driver focused on
low-latency mouse and keyboard input. The Xorg driver module is named `ainput`.

It is not a full libinput replacement. It intentionally does less work:
no touchpad gestures, no tablet handling, no adaptive acceleration pipeline, and no
Wayland support for now. The goal is a short, predictable path from Linux evdev
to Xorg.

## Features

- Relative mouse movement through evdev
- Basic mouse buttons and wheel
- Basic keyboard events
- Linear mouse sensitivity
- Optional DPI normalization
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

Current tested setup:

- XLibre `25.0.0.22`
- XInput driver ABI `26.0`

Also:
- XOrg `1.21.1.24`
- XInput driver ABI `24.4`

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

Package examples:

<details>
<summary>Arch / CachyOS / Artix with Xorg</summary>

```sh
sudo pacman -S base-devel pkgconf xorg-server-devel libx11 libxi
```

</details>

<details>
<summary>Arch / CachyOS / Artix with XLibre</summary>

```sh
sudo pacman -S base-devel pkgconf xlibre-xserver-devel libx11 libxi
```

</details>

<details>
<summary>Debian / Ubuntu</summary>

```sh
sudo apt install build-essential pkg-config xserver-xorg-dev libx11-dev libxi-dev x11proto-dev
```

</details>

<details>
<summary>Fedora</summary>

```sh
sudo dnf install gcc make pkgconf-pkg-config xorg-x11-server-devel libX11-devel libXi-devel xorg-x11-proto-devel
```

</details>

<details>
<summary>openSUSE</summary>

```sh
sudo zypper install gcc make pkgconf-pkg-config xorg-x11-server-sdk libX11-devel libXi-devel
```

</details>

<details>
<summary>Void Linux</summary>

```sh
sudo xbps-install -S base-devel pkg-config xorg-server-devel libX11-devel libXi-devel
```

</details>

Build:

```sh
make
```

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

### Build with Experimental Xorg/XLibre Patches

AnbryInput can optionally use experimental Xorg/XLibre patches that provide
specialized paths for relative motion and keyboard-event construction.

These patches are **not part of upstream Xorg/XLibre**. They must be applied
when building your X server. If they are not present, simply build AnbryInput
normally.

The experimental patch files are:

- `patches/xlibre-ainput-direct-experimental.patch` provides `QueueAInputRelativeMotion2DRaw` and `QueueAInputKey`
- `patches/xorg-ainput-direct-experimental.patch` provides `QueueAInputRelativeMotion2DRaw` and `QueueAInputKey`

Each patch is focused on the latest stable release of its respective X server.

Buttons use the normal Xorg/XLibre path.

#### Direct AnbryInput Path

Uses `QueueAInputRelativeMotion2DRaw` and `QueueAInputKey`, written
specifically for AnbryInput. The motion helper skips the generic public
wrapper and scroll-axis scan, and passes its relative two-axis mask directly
to the server's original
`fill_pointer_events()` and queue helpers. This deliberately keeps the normal
transform, positioning, barrier, confinement, screen-crossing, history,
master/slave, validation, and queue semantics. The keyboard helper constructs
the standard raw-key/key pair directly and retains normal downstream XKB,
focus, grab, master-device, and XI2 processing.

Both helpers use the X server's current event time, matching the normal input
path. Kernel timestamps are not mixed with server-timestamped buttons and
scroll events.

The patch is additive: it does not rewrite the server's generic event or
pointer paths. Its entry points run only when an AnbryInput module built with
`XSERVER_DIRECT=1` calls them; every other driver keeps the original behavior.

The earlier hand-written pointer-positioning and button helpers were removed
after real-world testing exposed incorrect focus, multi-screen boundary, and
button handling. The current motion helper deliberately shares the normal
server positioning core. Do not combine a current driver with the obsolete
`XSERVER_FAST_REL2D` patch or option.

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

    Option "Type" "mouse"
    Option "Sensitivity" "1.0"
    Option "DPI" "1000"
    Option "ReferenceDPI" "1000"
    Option "ReadBudget" "2"
EndSection
```

Example keyboard config:

```conf
Section "InputClass"
    Identifier "AnbryInput Keyboard"
    MatchProduct "YOUR_KEYBOARD_NAME_HERE"
    MatchIsKeyboard "on"
    Driver "ainput"

    Option "Type" "keyboard"
    Option "xkb_layout" "us"
    Option "ReadBudget" "1"
EndSection
```

The repository also includes [99-ainput.conf](99-ainput.conf), but treat it as a
starting point. Device names and event paths differ between systems.

Default options:

| Option | Default | Notes |
| --- | --- | --- |
| `Type` | auto-detected | Use `mouse` or `keyboard` to avoid ambiguous devices. |
| `Sensitivity` | `1.0` | Runtime changes are exposed as `AInput Sensitivity`. |
| `DPI` | `1000` | Used for relative mouse DPI normalization. |
| `ReferenceDPI` | `1000` | Baseline DPI for the sensitivity formula. |
| `ReadBudget` | `1` | Full 256-event reads allowed per callback: `1`, `2`, `4`, or `8`. |
| `xkb_layout` | `us` | Keyboard layout fallback. |
| `xkb_variant` | unset | Example for Brazilian ABNT2: `abnt2`. |

## Read Budget

`ReadBudget` is a per-device fairness limit, not the device polling rate. Each
callback reads a fixed array of up to 256 Linux input events. The driver only
attempts another nonblocking `read()` when the previous array was completely
full, and stops after `ReadBudget` reads or as soon as no more data is
immediately available. It never waits for new events.

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
device's `InputClass` when different devices need different limits. Do not set
it to `1000`, `8000`, or another polling-rate value.

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

## Example Results

These are example results from one system, not a universal guarantee.

Test setup:

- CachyOS 
- 7.0.12-1-cachyos-bore
- Dell Precision 7530
- XLibre 25.0.0.22
- Logitech G203 LIGHTSYNC
- AnbryInput (`ainput`)
- `scx_flash -m all`
- sudo cpupower frequency-set -g powersave
- sudo cpupower frequency-set -u 3.5GHz

Command style:

```sh
sudo ./tools/mouse_latency_xi2 \
  --event /dev/input/eventX \
  --device-id <device-id> \
  --mode raw \
  --samples 10000
```

The benchmark discarded 128 warmup samples before recording results.

Observed results:

| Driver | Mode | mean | p50 | p90 | p95 | p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| AnbryInput (`ainput`) | XI_RawMotion | ~0.057-0.066 ms | ~0.055 ms | ~0.067-0.069 ms | ~0.074-0.078 ms | ~0.100-0.113 ms |
| libinput | XI_RawMotion | ~0.084-0.086 ms | ~0.081-0.082 ms | ~0.103-0.106 ms | ~0.113-0.120 ms | ~0.137-0.152 ms |
| AnbryInput (`ainput`) | XI_Motion | ~0.062-0.063 ms | ~0.059 ms | ~0.076-0.077 ms | ~0.086-0.089 ms | ~0.112-0.116 ms |
| libinput | XI_Motion | ~0.090-0.091 ms | ~0.088 ms | ~0.108-0.112 ms | ~0.119-0.123 ms | ~0.144-0.150 ms |

On this setup, AnbryInput was consistently faster than libinput in p50, p95,
and p99 latency. The advantage was roughly 20-35% depending on the percentile
and event path. This is a small absolute difference, measured in tens of
microseconds, but it was repeatable in this test.

Different kernels, schedulers, mice, USB controllers, WMs, compositors, and games
can change the results. Measure on your own system.

### Additional Arch Linux Results

A later comparison was run on Arch Linux with the Zen kernel, the same Dell
Precision 7530 and Logitech G203 LIGHTSYNC, `scx_flash -m all`, the `powersave`
governor, and a maximum CPU frequency of 3.5 GHz. Each value below is the
average of three runs with 5000 recorded samples and 128 discarded warmup
samples. The complete outputs are in `tools/new_test_raw.txt` and
`tools/new_test_motion.txt`.

| Driver | Mode | mean | p50 | p90 | p95 | p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| AnbryInput (`ainput`) | XI_RawMotion | 0.091 ms | 0.087 ms | 0.109 ms | 0.116 ms | 0.222 ms |
| libinput | XI_RawMotion | 0.103 ms | 0.100 ms | 0.120 ms | 0.126 ms | 0.241 ms |
| AnbryInput (`ainput`) | XI_Motion | 0.095 ms | 0.091 ms | 0.114 ms | 0.121 ms | 0.210 ms |
| libinput | XI_Motion | 0.105 ms | 0.100 ms | 0.123 ms | 0.131 ms | 0.235 ms |

AnbryInput remained faster in every reported percentile, with an advantage of
roughly 8-13% in this run. Absolute latency was higher and the relative
advantage was smaller than in the earlier CachyOS result. This is not enough
to identify a driver regression because the kernel, distribution, X server
version, and other system components were not held constant between the two
series.

Results from additional clean computers are required before drawing broader
performance conclusions. Ideally, contributors should compare AnbryInput and
libinput after a fresh boot on the same machine, alternate the test order, run
at least three repetitions per mode, record the exact kernel and X server
versions, and avoid background workloads. Tests on different CPUs, USB
controllers, polling rates, and distributions are especially useful.

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

`SYN_DROPPED` means the kernel evdev queue overflowed before the X server read
it; it is not a sensitivity or acceleration message. The driver discards the
incomplete report, attempts to reconstruct key/button state, and resumes at
the next complete report. If the state ioctl returns `EAGAIN`, it releases its
tracked buttons and resumes instead of freezing the device. Repeated drops at
8000 Hz still indicate that the server was unable to drain input promptly. A
read callback handles at most 256 events so a continuously busy device cannot
starve window, focus, rendering, or DPMS work in the X server. Compare 1000 Hz
and 8000 Hz and check CPU, scheduler, GPU-driver, and suspend behavior when
reporting the issue.

## Contributing

Useful areas for improvement:

- More testing on other Xorg/XLibre systems
- Packaging
- Better documentation for device matching
- Keyboard latency benchmarking
- More mouse buttons and horizontal wheel support
- Safer configuration examples
- Xorg/XLibre input path investigation

Please keep the project goal in mind: small, predictable, low-latency input for
Xorg/XLibre.
