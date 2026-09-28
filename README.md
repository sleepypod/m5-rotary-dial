<p align="center">
  <img src="docs/screens/banner.png" alt="sleepypod Dial: heating, night theme, and the dimmed glance state" width="900">
</p>

<h1 align="center">sleepypod Dial</h1>

<p align="center"><strong>One arc. One number. Nothing to learn in the dark.</strong></p>

<p align="center">
A bedside knob for your Pod, built on the <a href="https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen">M5Stack Dial</a> and talking to <a href="https://github.com/sleepypod/core">sleepypod-core</a> on your own network.<br>
It shows where your mattress is, where it is going, and lets you turn it, click it off, and leave it alone.
</p>

<p align="center">
  <a href="https://codecov.io/gh/sleepypod/m5-rotary-dial"><img src="https://codecov.io/gh/sleepypod/m5-rotary-dial/branch/main/graph/badge.svg" alt="codecov"></a>
</p>

## In motion

<table>
<tr>
<td align="center"><img src="docs/video/turn.gif" width="220" alt="Turning"><br><strong>Turn</strong><br><sub>1° per detent, 2° when you spin. The arc settles when you stop.</sub></td>
<td align="center"><img src="docs/video/loader.gif" width="220" alt="Getting there"><br><strong>Getting there</strong><br><sub>Solid fill is the mattress. The dim span to the target shrinks as the bed catches up.</sub></td>
<td align="center"><img src="docs/video/power.gif" width="220" alt="Off and on"><br><strong>Click</strong><br><sub>One click empties the arc. One more lets core choose the current target.</sub></td>
</tr>
</table>

<p align="center"><strong><a href="https://sleepypod.github.io/m5-rotary-dial/">See the full walkthrough →</a></strong><br>
<sub>Every screen, the night theme, and how each control behaves. Every frame is captured from the device.</sub></p>

<p align="center"><strong><a href="https://makerworld.com/en/models/3365781-sleepypod-dial-enclosure-for-m5stack-dial">Print the enclosure on MakerWorld →</a></strong><br>
<sub>A two-part bedside stand for the Dial. Print files are also in <a href="hardware/"><code>hardware/</code></a>.</sub></p>

## Controls

| You do | It does |
|---|---|
| **Turn** | Moves the target. 1° per detent, 2° when you spin. Two detents below 55° reach an off stop. |
| **Click**, or tap ⏻ | Turns your side off, or back on using core’s current target. |
| **Hold**, or tap ⚙ | Opens settings after a ring fills. Release early and nothing happens. |
| **Settings › Side** | Picks Left or Right once. It is remembered and never changes by accident. |
| **Settings › Hold duration** | Cycles 15, 30, 60, or 120 minutes for your next adjustment (default 30). |
| **Settings › Resume** | Releases your side’s manual hold so core can resume the applicable automation. |
| **Touch while dim** | Only wakes the screen. The next turn counts. |

With temperature-control support in core, the main screen shows the current owner and manual hold expiry, plus any safety/off block. Older core versions keep the existing temperature requests; hold duration and Resume take effect once core reports support.

Local only: the dial talks to sleepypod-core on your network, found by mDNS (`_sleepypod._tcp`) or an IP you set. Between 10 pm and 7 am it shifts to a red-on-black theme with no sounds. Based on [RotaryDial by dallonby](https://github.com/dallonby/RotaryDial). For the full feature reference see [`docs/features.md`](docs/features.md); for boot, loop, rendering and state diagrams see [`docs/architecture.md`](docs/architecture.md).

---

## Hardware Requirements

- **[M5Stack Dial](https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen)** — ESP32-S3, 240x240 round capacitive touchscreen, rotary encoder
- **sleepypod-core** running on your Pod's local network

## Enclosure

The enclosure is a base and a faceplate that hold an M5Stack Dial v1.1 at the bedside.

- **Easiest:** open the [MakerWorld listing](https://makerworld.com/en/models/3365781-sleepypod-dial-enclosure-for-m5stack-dial) and print its profile.
- **From this repo:** the 3MF files in [`hardware/`](hardware/) are the same parts. See [`hardware/README.md`](hardware/README.md) for orientation and settings.

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

### Tests

The hardware-free core (`src/dial_logic.cpp`: temperature maths, arc geometry and colour, detent acceleration, the OFF stop, the loader shimmer, Pod JSON parsing) has host-side Unity tests. No device needed:

```bash
pio test -e native
gcovr -r . --filter src/ --exclude-throw-branches --exclude-unreachable-branches --xml-pretty -o coverage.xml   # pip install gcovr
```

### 5. Configure Pod Connection

On first boot, the dial will:
1. Connect to WiFi
2. Attempt mDNS auto-discovery of your Pod
3. Fall back to the saved/default IP (192.168.1.88)

To manually set the Pod IP:
- Open Settings (long press center or tap bottom area)
- Navigate to "Pod IP Address"
- Use the rotary dial to set each octet

## Usage

Controls are summarised above. The settings menu (hold the dial or screen, or tap the gear) offers:

### Settings Menu

| Setting | Description |
|---------|-------------|
| **WiFi Settings** | Scan and connect to WiFi (select DEL in the character carousel to backspace; hold the dial button to connect) |
| **Pod IP Address** | Set Pod IP manually (tap to save at any octet) |
| **Discover Pod** | Re-run mDNS discovery |
| **Temperature Unit** | Toggle °F / °C display (a local choice sticks — Pod sync won't revert it) |
| **Night Mode** | Cycle Auto / Forced On / Forced Off |
| **Side** | Which side this dial controls (switches immediately, remembered) |

## Architecture

See [docs/architecture.md](docs/architecture.md) for detailed system diagrams including boot sequence, main loop flow, API integration, and state management.

## Configuration Reference

| Setting | Default | Description |
|---------|---------|-------------|
| `TEMP_MIN_F` | 55 | Minimum temperature (°F) |
| `TEMP_MAX_F` | 110 | Maximum temperature (°F) |
| `TEMP_DEFAULT_F` | 75 | Default/reset temperature (°F) |
| `POD_API_PORT` | 3000 | sleepypod-core API port |
| `API_PORT` | 80 | Local HTTP API port |
| `BRIGHTNESS_DAY` | 255 | Day brightness (0-255) |
| `BRIGHTNESS_NIGHT` | 51 | Night brightness (~20%) |
| `BRIGHTNESS_DIM` | 2 | Idle brightness (~1%) |
| `DIM_TIMEOUT_MS` | 10000 | Idle timeout before dimming |
| `NIGHT_START_HOUR` | 22 | Night mode start (24h) |
| `NIGHT_END_HOUR` | 7 | Night mode end (24h) |

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
- **[free-sleep](https://github.com/throwaway31265/free-sleep)** — Open source Pod control
- **[M5Stack](https://m5stack.com/)** — M5Stack Dial hardware
- **[PlatformIO](https://platformio.org/)** — Build system

## License

MIT License — See LICENSE file for details.
