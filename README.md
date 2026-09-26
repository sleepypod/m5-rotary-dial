# sleepypod Dial

A local bedside controller for your Eight Sleep Pod, built on the M5Stack Dial and [sleepypod-core](https://github.com/sleepypod/core). Turn to adjust the temperature, click for power, and hold for settings.

[![Heating, night theme, and the dimmed glance state](docs/screens/banner.png)](https://sleepypod.github.io/m5-rotary-dial/)

**[Explore the walkthrough →](https://sleepypod.github.io/m5-rotary-dial/)** — real device screens, animations, night mode, and the controls guide.

[![codecov](https://codecov.io/gh/sleepypod/m5-rotary-dial/branch/main/graph/badge.svg)](https://codecov.io/gh/sleepypod/m5-rotary-dial)

## Hardware Requirements

- **[M5Stack Dial](https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen)** — ESP32-S3, 240x240 round capacitive touchscreen, rotary encoder
- **sleepypod-core** running on your Pod's local network

## Setup

### 1. Install PlatformIO

**VS Code (Recommended):**
1. Install [VS Code](https://code.visualstudio.com/)
2. Install the PlatformIO IDE extension
3. Restart VS Code

**CLI:**
```bash
pip install platformio
# or
brew install platformio
```

### 2. Clone & Configure

```bash
git clone https://github.com/sleepypod/m5-rotary-dial.git
cd m5-rotary-dial

# Set your WiFi credentials
cp include/credentials.h.example include/credentials.h
# Edit include/credentials.h with your SSID and password
```

### 3. Configure Timezone (Optional)

Edit `include/config.h`:

```c
#define GMT_OFFSET_SEC -28800     // PST (UTC-8)
#define DAYLIGHT_OFFSET_SEC 0     // Set to 3600 for DST
```

| Timezone | Offset |
|----------|--------|
| EST (US Eastern) | `-18000` |
| CST (US Central) | `-21600` |
| PST (US Pacific) | `-28800` |
| GMT/UTC | `0` |
| CET (Central Europe) | `3600` |

### 4. Build & Flash

```bash
pio run --target upload
```

### 5. Configure Pod Connection

On first boot, the dial will:
1. Connect to WiFi
2. Attempt mDNS auto-discovery of your Pod
3. Fall back to the saved/default IP (192.168.1.88)

To manually set the Pod IP:
- Open Settings (hold the dial or screen, or tap the gear)
- Navigate to "Pod IP Address"
- Use the rotary dial to set each octet

## Reference and development

- [Features and controls](docs/features.md)
- [Architecture and API integration](docs/architecture.md)
- [Configuration defaults](include/config.h)
- [Device walkthrough](https://sleepypod.github.io/m5-rotary-dial/) and [offline HTML](docs/walkthrough.html)

The website is served from `docs/` on GitHub Pages. To regenerate both HTML pages from the checked-in captures without connecting a device:

```bash
# Requires pyserial and ffmpeg; PlatformIO's Python includes pyserial.
~/.platformio/penv/bin/python tools/walkthrough.py --page-only
```

Run the same command without `--page-only` to capture new screens and animations from a connected Dial. See the options in [`tools/walkthrough.py`](tools/walkthrough.py).

## Troubleshooting

### Pod not found via mDNS
- Ensure sleepypod-core is running on the Pod
- Verify both devices are on the same subnet
- Try manual IP configuration via Settings > Pod IP Address
- The Pod advertises `_sleepypod._tcp` on port 3000

### Temperature not syncing
- Check serial monitor for API error messages
- Verify Pod IP is correct (Settings > Pod IP Address)
- Ensure sleepypod-core is accessible on port 3000
- The dial syncs every 30 seconds — restart to force immediate sync

### Display issues
- Night mode activates automatically 10pm-7am; check timezone in `config.h`
- If stuck dimmed, touch or rotate to wake

## Acknowledgments

- **[dallonby/RotaryDial](https://github.com/dallonby/RotaryDial)** — Original FreeSleep rotary dial controller. This project is built on their excellent work adapting the M5Stack Dial for bed temperature control.
- **[free-sleep](https://github.com/throwaway31265/free-sleep)** — Open source Eight Sleep Pod control
- **[M5Stack](https://m5stack.com/)** — M5Stack Dial hardware
- **[PlatformIO](https://platformio.org/)** — Build system

## License

MIT License — See LICENSE file for details.
