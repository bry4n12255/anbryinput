# AnbryInput

AnbryInput is a small experimental Xorg/XLibre input driver focused on
low-latency mouse and keyboard input. The Xorg driver module is named `ainput`.

It is not a full libinput replacement. It intentionally does less work: no
touchpad gestures, no tablet handling, no adaptive acceleration pipeline, and no
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
faster paths for relative mouse motion and ordinary mouse buttons.

These patches are **not part of upstream Xorg/XLibre**. They must be applied
when building your X server. If they are not present, simply build AnbryInput
normally.

The experimental patch files are:

- `patches/xlibre-relative-motion-2d-fast-path.patch` provides `QueuePointerRelativeMotion2D`
- `patches/xlibre-ainput-direct-experimental.patch` provides `QueueAInputRelativeMotion2D` and `QueueAInputButton`
- `patches/xlibre-ainput-direct-keys-experimental.patch` provides `QueueAInputKey` and must be applied after the direct AnbryInput patch

Wheel buttons still use the normal Xorg/XLibre path so scroll behavior stays
compatible.

#### Fast Relative Motion

Uses a small X server helper (`QueuePointerRelativeMotion2D`) that avoids some
generic event setup while still going through the normal Xorg input pipeline.

```sh
make XSERVER_FAST_REL2D=1
```

#### Direct AnbryInput Path

Uses dedicated X server fast paths (`QueueAInputRelativeMotion2D` and
`QueueAInputButton`) written specifically for AnbryInput. It bypasses much of
the generic pointer/button event generation while still producing the expected
XInput events.

```sh
make XSERVER_DIRECT=1
```

#### Direct Keyboard Path

Uses an additional X server fast path (`QueueAInputKey`) for basic keyboard
press/release events. This is more experimental than the mouse direct path and
may affect keyboard repeat, modifiers, shortcuts, or XKB behavior.

```sh
make XSERVER_DIRECT_KEYS=1
```

It must be combined with the direct AnbryInput patch:

```sh
make XSERVER_DIRECT=1 XSERVER_DIRECT_KEYS=1
```

These options can also be combined with compiler optimizations:

```sh
make NATIVE=1 AGGRESSIVE=1 XSERVER_DIRECT=1 XSERVER_DIRECT_KEYS=1
```

## Install

Install the driver into the Xorg/XLibre input module directory:

```sh
sudo make install
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
| `xkb_layout` | `us` | Keyboard layout fallback. |
| `xkb_variant` | unset | Example for Brazilian ABNT2: `abnt2`. |

## Sensitivity And DPI

AnbryInput applies a simple linear multiplier:

```text
effective = Sensitivity * (ReferenceDPI / DPI)
```

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
