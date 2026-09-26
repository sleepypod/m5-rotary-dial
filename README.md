<p align="center">
  <img src="docs/screens/banner.png" alt="Sleepypod Dial: heating, night theme, and the dimmed glance state" width="900">
</p>

<h1 align="center">Sleepypod Dial</h1>

<p align="center"><strong>One arc. One number. Nothing to learn in the dark.</strong></p>

<p align="center">
A bedside knob for the Eight Sleep Pod, built on the <a href="https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen">M5Stack Dial</a> and talking to <a href="https://github.com/throwaway31265/free-sleep">sleepypod-core</a> on your own network.<br>
It shows where your mattress is, where it is going, and lets you turn it, click it off, and leave it alone.
</p>

<p align="center">
  <a href="https://codecov.io/gh/sleepypod/m5-rotary-dial"><img src="https://codecov.io/gh/sleepypod/m5-rotary-dial/branch/main/graph/badge.svg" alt="codecov"></a>
</p>

> Every image on this page is a frame captured from the device, not a mockup. `tools/walkthrough.py` regenerates all of them.

## In motion

<table>
<tr>
<td align="center"><img src="docs/video/turn.gif" width="200" alt="Turning"><br><strong>Turning</strong><br><sub>1° per detent, 2° when you spin. The arc settles 180 ms after you stop.</sub></td>
<td align="center"><img src="docs/video/loader.gif" width="200" alt="Getting there"><br><strong>Getting there</strong><br><sub>Solid fill is the mattress. The dim span to the target carries a highlight that travels toward it.</sub></td>
<td align="center"><img src="docs/video/power.gif" width="200" alt="Off and on"><br><strong>Off and on</strong><br><sub>One click empties the arc. One more brings it back at the last setpoint.</sub></td>
<td align="center"><img src="docs/video/settings.gif" width="200" alt="Holding"><br><strong>Holding</strong><br><sub>The ring fills over 1.5 s, then settings opens. Let go early and nothing happens.</sub></td>
</tr>
</table>

## Nine screens, in the order you meet them

<table>
<tr>
<td align="center"><img src="docs/screens/heating.png" width="200" alt="Heating"><br><strong>Turn toward comfort</strong><br><sub>Solid fill is where the mattress is. The span beyond it leads to the number you chose and shrinks as the bed catches up.</sub></td>
<td align="center"><img src="docs/screens/cooling.png" width="200" alt="Cooling"><br><strong>Same timeline, either direction</strong><br><sub>Cooling reads the same way: the target sits at the cap, the span is the distance still to travel.</sub></td>
<td align="center"><img src="docs/screens/at-target.png" width="200" alt="At target"><br><strong>Then it goes quiet</strong><br><sub>At the target the loader disappears. Just your side, your number, the time.</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/screens/off.png" width="200" alt="Off"><br><strong>Off is a click</strong><br><sub>Click the dial, or tap the power glyph. The arc empties and the number dims.</sub></td>
<td align="center"><img src="docs/screens/hold-ring.png" width="200" alt="Hold ring"><br><strong>Hold for settings</strong><br><sub>Hold the dial or the screen. A ring fills around the rim; release early to cancel.</sub></td>
<td align="center"><img src="docs/screens/settings.png" width="200" alt="Settings"><br><strong>Your side is a preference</strong><br><sub>Pick Left or Right once in Settings. Nothing on the main screen switches it by accident.</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/screens/night.png" width="200" alt="Night"><br><strong>Red after ten</strong><br><sub>Between 10 pm and 7 am everything shifts to red on black at 20% brightness. No sounds at all.</sub></td>
<td align="center"><img src="docs/screens/night-dim.png" width="200" alt="Night dim"><br><strong>Glanceable at 3 am</strong><br><sub>After five seconds it dims to 1%. Only the number and one status dot survive, by design.</sub></td>
<td align="center"><img src="docs/screens/day-dim.png" width="200" alt="Day dim"><br><strong>Wake without changing anything</strong><br><sub>The first touch after a dim only wakes the screen. A bump in the dark never moves your temperature.</sub></td>
</tr>
</table>

## Five things, and only five

| You do | It does |
|---|---|
| **Turn** | Moves the target. 1° per detent, 2° when you spin. Two detents below 55° reach an off stop. |
| **Click**, or tap ⏻ | Turns your side off, or back on at the last setpoint. |
| **Hold**, or tap ⚙ | Opens settings after a ring fills. Release early and nothing happens. |
| **Settings › Side** | Picks Left or Right once. It is remembered and never changes by accident. |
| **Touch while dim** | Only wakes the screen. The next turn counts. |

Nothing on the main screen changes a value by touch. Rotation is always silent (the detents are the feedback); a side change or power toggle gets one 10 ms tick by day and nothing at night.

## By the numbers

| | |
|---|---|
| **55–110 °F** | the Pod's range, 1° per detent |
| **2°** | per detent when you spin, never more |
| **1.5 s** | hold for settings, ring shows progress |
| **0 dB** | after 10 pm, no sounds at all |
| **19 ms** | per frame; the encoder is polled every 1 ms so a spin never drops detents |
| **500 ms** | after the last detent before a change is sent, from a worker task that never blocks the dial |

## What it does for you

- **Local only.** Talks to sleepypod-core on your network. Finds the Pod by mDNS (`_sleepypod._tcp`) or uses an IP you set. No cloud, no account.
- **Honest about the network.** A hollow cap means the Pod has not confirmed a change yet. "Pod offline" and "No Wi-Fi" say so in words, and the dial reconnects on its own after a router restart.
- **Your name on it.** The side name comes from the Pod's settings, so the dial says Jon, not L.
- **Your changes win.** The Pod's own state never overwrites a number you touched in the last 30 seconds.
- **Night mode that stays out of the way.** Red-only theme 10 pm to 7 am at 20% brightness, dims to 1% after five seconds, fades instead of stepping. Override it in Settings.
- **Reliable.** Daily restart follows the Pod's own reboot schedule, and a watchdog reboots the dial if the UI loop ever stalls.

Based on [RotaryDial by dallonby](https://github.com/dallonby/RotaryDial). There is also a self-contained walkthrough page at [`docs/walkthrough.html`](docs/walkthrough.html).

---

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
git clone https://github.com/your-org/sleepypod-mt-rotary-dial.git
cd sleepypod-mt-rotary-dial

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
- **[free-sleep](https://github.com/throwaway31265/free-sleep)** — Open source Eight Sleep Pod control
- **[M5Stack](https://m5stack.com/)** — M5Stack Dial hardware
- **[PlatformIO](https://platformio.org/)** — Build system

## License

MIT License — See LICENSE file for details.
