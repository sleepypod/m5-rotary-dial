# Capture the walkthrough from a connected Dial: stills for every state, short
# video clips of the animations, a self-contained HTML page, and a Pages site.
#
#   ~/.platformio/penv/bin/python tools/walkthrough.py            # everything
#   ~/.platformio/penv/bin/python tools/walkthrough.py --no-video # stills + page only
#   ~/.platformio/penv/bin/python tools/walkthrough.py --only settings  # re-record one clip
#   ~/.platformio/penv/bin/python tools/walkthrough.py --page-only  # rebuild the page from docs/
#
# Needs pyserial (PlatformIO's Python has it) and ffmpeg on PATH for video.
# Uses the firmware's serial debug channel (see handleSerialDebug in main.cpp).
# Fake mattress temperatures (h/l/a) are local only; the Pod is never written.
# Outputs: docs/screens/*.png (+ banner.png), docs/video/*.mp4 + *.gif, docs/walkthrough.html + docs/index.html
import base64, os, shutil, struct, subprocess, sys, tempfile, time, zlib
import serial

PORT = os.environ.get("DIAL_PORT", "/dev/cu.usbmodem21201")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCREENS = os.path.join(ROOT, "docs", "screens")
VIDEO = os.path.join(ROOT, "docs", "video")
PAGE = os.path.join(ROOT, "docs", "walkthrough.html")
W = H = 240
NO_VIDEO = "--no-video" in sys.argv
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None  # capture one clip, no page
PAGE_ONLY = "--page-only" in sys.argv  # no device: build the page from files already in docs/



def decode_frame(text):
    """Decode a run-length encoded dump (hex 'ccvvvv' pairs) into PNG bytes."""
    hexs = "".join(text.split())
    px = []
    total = W * H
    for i in range(0, len(hexs), 6):
        run = int(hexs[i:i + 2], 16)
        v = int(hexs[i + 2:i + 6], 16)
        v = ((v & 0xFF) << 8) | (v >> 8)  # sprite stores RGB565 byte-swapped
        px.append((run, bytes((((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31))))
    raw = bytearray()
    n = 0
    for run, rgb in px:
        for _ in range(run):
            if n % W == 0:
                raw.append(0)
            raw += rgb
            n += 1
    if n != total:
        raise RuntimeError(f"decoded {n} px, expected {total}")

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


class Dial:
    def __init__(self):
        self.s = serial.Serial(PORT, 115200, timeout=2)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def send(self, cmds, settle=0.35):
        for c in cmds:
            self.s.write(c.encode())
            self.s.flush()
            time.sleep(0.05)
        time.sleep(settle)
        self.s.reset_input_buffer()

    def frame(self):
        """Ask for a framebuffer dump and return it as PNG bytes. Log lines from
        the firmware's HTTP task can interleave with the dump, so retry."""
        for attempt in range(4):
            try:
                return self._frame()
            except RuntimeError as e:
                print("retry:", e)
                time.sleep(0.3)
        self.reset()
        raise RuntimeError("could not capture a clean frame; device was reset")

    def reset(self):
        """Hardware reset over USB (DTR/RTS), then wait for the boot + Pod sync."""
        self.s.setDTR(False); self.s.setRTS(True); time.sleep(0.1); self.s.setRTS(False)
        time.sleep(14)
        self.s.reset_input_buffer()

    def _frame(self):
        """Request a dump and read the byte stream until the end marker.
        Bytes are accumulated (not read line by line) so a pause on the device
        side, e.g. while the HTTP worker holds the serial mutex, cannot split a
        line. Anything before the start marker is firmware logging."""
        self.s.write(b"p")
        self.s.flush()
        buf = bytearray()
        t = time.time()
        while time.time() - t < 30:
            chunk = self.s.read(65536)
            if chunk:
                buf += chunk
                if b"<<END>>" in buf:
                    break
        text = buf.decode(errors="replace")
        if "<<FB>>" not in text or "<<END>>" not in text:
            for l in text.splitlines()[-6:]:
                print("serial:", l[:160], file=sys.stderr)
            raise RuntimeError(f"no frame ({len(buf)} bytes)")
        head, rest = text.split("<<FB>>", 1)
        for l in head.splitlines():
            if l.strip():
                print("serial:", l[:160], file=sys.stderr)
        body = rest.split("<<END>>", 1)[0]
        return decode_frame(body)

    def still(self, name, cmds):
        self.send(cmds)
        png = self.frame()
        with open(os.path.join(SCREENS, name + ".png"), "wb") as f:
            f.write(png)
        print("still", name)
        return png

    def clip(self, name, script):
        """script: list of (cmds, frames). Runs on the frozen UI clock: every
        frame advances 40ms (25fps) and dumps."""
        if NO_VIDEO or (ONLY and ONLY != name):
            return None
        tmp = tempfile.mkdtemp()
        n = 0
        self.send("w", settle=0.3)  # awake; the frozen clock may have drifted past the dim timeout
        self.send("T", settle=0.2)  # freeze
        try:
            for cmds, frames in script:
                if cmds:
                    self.send(cmds, settle=0.1)
                for _ in range(frames):
                    with open(os.path.join(tmp, f"f{n:04d}.png"), "wb") as f:
                        f.write(self.frame())
                    n += 1
        finally:
            self.send("t", settle=0.2)  # live again
        mp4 = os.path.join(VIDEO, name + ".mp4")
        gif = os.path.join(VIDEO, name + ".gif")
        seq = os.path.join(tmp, "f%04d.png")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "25", "-i", seq,
                        "-vf", "scale=480:480:flags=neighbor,format=yuv420p", "-c:v", "libx264", "-crf", "20", mp4], check=True)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "25", "-i", seq,
                        "-vf", "scale=360:360:flags=neighbor,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=none", gif], check=True)
        shutil.rmtree(tmp)
        print("clip", name, n, "frames")
        with open(mp4, "rb") as f:
            return f.read()


def b64(kind, data):
    return f"data:{kind};base64," + base64.b64encode(data).decode()


STILL_NAMES = ["heating", "cooling", "at-target", "off", "on", "hold-ring", "settings", "night", "night-dim", "day-dim"]
CLIP_NAMES = ["turn", "loader", "power", "settings"]


def read_or_none(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def main():
    os.makedirs(SCREENS, exist_ok=True)
    os.makedirs(VIDEO, exist_ok=True)
    if PAGE_ONLY:
        stills = {k: read_or_none(os.path.join(SCREENS, k + ".png")) for k in STILL_NAMES}
        clips = {k: read_or_none(os.path.join(VIDEO, k + ".mp4")) for k in CLIP_NAMES}
        missing = [k for k, v in stills.items() if v is None]
        if missing:
            sys.exit(f"missing stills: {missing}; run without --page-only first")
        write_banner()
        write_page(stills, clips)
        return
    d = Dial()
    d.send("wxPa", settle=1.5)  # awake, main screen, side on, mattress at target
    if ONLY:
        scripts = {
            "turn": [("", 5)] + [("+", 3)] * 6 + [("", 12)] + [("-", 3)] * 6 + [("", 15)],
            "loader": [("h", 75), ("a", 15)],
            "power": [("", 5), ("o", 20), ("o", 25)],
            "settings": [("", 5), ("H", 45), ("x", 15)],
        }
        d.clip(ONLY, scripts[ONLY])
        d.send("wa")
        return

    stills = {
        "heating": d.still("heating", "h"),
        "cooling": d.still("cooling", "l"),
        "at-target": d.still("at-target", "a"),
        "off": d.still("off", "o"),
        "on": d.still("on", "o"),
        "hold-ring": d.still("hold-ring", "hH"),
        "settings": d.still("settings", "wS"),
        "night": d.still("night", "xn"),
        "night-dim": d.still("night-dim", "z"),
        "day-dim": d.still("day-dim", "wnnz"),
    }
    d.send("wa")

    clips = {
        "turn": d.clip("turn", [("", 5)] + [("+", 3)] * 6 + [("", 12)] + [("-", 3)] * 6 + [("", 15)]),
        "loader": d.clip("loader", [("h", 75), ("a", 15)]),
        "power": d.clip("power", [("", 5), ("o", 20), ("o", 25)]),
        "settings": d.clip("settings", [("", 5), ("H", 45), ("x", 15)]),
    }
    d.send("wa")
    write_banner()
    write_page(stills, clips)


def write_banner():
    """README hero: three screens side by side at 2x on the panel background."""
    src = [os.path.join(SCREENS, n + ".png") for n in ("heating", "night", "night-dim")]
    out = os.path.join(SCREENS, "banner.png")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src[0], "-i", src[1], "-i", src[2],
                    "-filter_complex", "[0]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[a];[1]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[b];"
                    "[2]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[c];[a][b][c]hstack=3,pad=iw+60:ih+80:30:40:0x0B0E14", out], check=True)
    print("banner", out)


def write_page(stills, clips):
    slides = [
        ("heating", "Turn toward comfort", "Solid fill is where the mattress is. The span beyond it pulses toward the number you chose and shrinks as the bed catches up."),
        ("cooling", "Same timeline, either direction", "Cooling reads the same way: the target sits at the cap, the pulsing span is the distance still to travel."),
        ("at-target", "Then it goes quiet", "At the target the loader disappears. Just your side, your number, the time."),
        ("off", "Off is a click", "Click the dial, or tap the power glyph. The arc empties and the number dims. A click brings it back at the last setpoint."),
        ("hold-ring", "Hold for settings", "Hold the dial or the screen. A ring fills around the rim; let go early and nothing happens."),
        ("settings", "Your side is a preference", "Pick Left or Right once in Settings. Nothing on the main screen switches it by accident."),
        ("night", "Red after ten", "Between 10 pm and 7 am the whole interface shifts to red on black at 20% brightness. No sounds at all."),
        ("night-dim", "Glanceable at 3 am", "After five seconds it dims to 1%. Only the number and one status dot survive, by design."),
        ("day-dim", "Wake without changing anything", "The first touch after a dim only wakes the screen. A bump in the dark never moves your temperature."),
    ]
    img = {k: b64("image/png", v) for k, v in stills.items()}

    def slide(i, k, h, p):
        return (f'<article class="slide"><div class="device"><img src="{img[k]}" alt="{h}" width="240" height="240"></div>'
                f'<div class="cap"><span class="n">{i:02d}</span><h3>{h}</h3><p>{p}</p></div></article>')

    def video(k, title, text):
        if not clips.get(k):
            return ""
        src = b64("video/mp4", clips[k])
        return (f'<article class="slide"><div class="device"><video src="{src}" muted loop playsinline width="240" height="240"></video></div>'
                f'<div class="cap"><h3>{title}</h3><p>{text}</p></div></article>')

    videos = "".join([
        video("turn", "Turning", "Each detent moves the target one degree, two when you spin. The arc settles 180 ms after you stop."),
        video("loader", "Getting there", "The span between the mattress and the target pulses gently until the bed arrives."),
        video("power", "Off and on", "One click empties the arc. One more brings it back at the last setpoint."),
        video("settings", "Holding", "The ring fills over a second and a half, then settings opens. Let go early and nothing happens."),
    ])

    html = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>sleepypod Dial</title>
<meta name="description" content="A bedside knob for the Eight Sleep Pod: one arc, one number, no menus in the dark.">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Sora:wght@500;600&family=Instrument+Sans:ital,wght@0,400;0,500;1,400&family=DM+Mono:wght@400;500&display=swap">
<style>
:root{{--bg:#F3F2EE;--ink:#15181F;--muted:#6B7079;--line:#DCD8CF;--panel:#FFFFFF;--warm:#D97F22;--cool:#2792AA;--metal:#2A2F38;--metal2:#0E1116;--screen:#0B0E14;
  --display:'Sora',system-ui,sans-serif;--body:'Instrument Sans',system-ui,sans-serif;--mono:'DM Mono',ui-monospace,monospace}}
@media (prefers-color-scheme: dark){{:root:not([data-theme="light"]){{--bg:#0B0E14;--ink:#F2F2F0;--muted:#9AA0A8;--line:#232833;--panel:#11151D;--warm:#FFA53C;--cool:#35C4E0;--metal:#3A404B;--metal2:#181C24;color-scheme:dark}}}}
:root[data-theme="dark"]{{--bg:#0B0E14;--ink:#F2F2F0;--muted:#9AA0A8;--line:#232833;--panel:#11151D;--warm:#FFA53C;--cool:#35C4E0;--metal:#3A404B;--metal2:#181C24;color-scheme:dark}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font-family:var(--body);font-size:17px;line-height:1.5}}
.wrap{{max-width:1080px;margin:0 auto;padding-inline:20px;padding-block:48px 80px;display:grid;gap:72px}}
h1,h2,h3{{font-family:var(--display);font-weight:600;letter-spacing:-0.01em;text-wrap:balance;margin:0}}
.eyebrow{{font-family:var(--mono);font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted)}}
p{{margin:0;max-width:62ch}}
.hero{{display:grid;grid-template-columns:1.1fr 1fr;gap:40px;align-items:center}}
.hero h1{{font-size:clamp(38px,5.5vw,60px);line-height:1.05;margin-block:10px 18px}}
.hero .lede{{font-size:19px;color:var(--muted)}}
.device{{position:relative;width:300px;height:300px;border-radius:50%;margin:0 auto;background:radial-gradient(circle at 35% 30%,var(--metal) 0%,var(--metal2) 70%);box-shadow:0 30px 60px -30px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.08)}}
.device::before{{content:"";position:absolute;inset:14px;border-radius:50%;background:var(--screen);box-shadow:inset 0 0 0 2px #05070A}}
.device img,.device video{{position:absolute;inset:30px;width:240px;height:240px;border-radius:50%;display:block;object-fit:cover}}
.hero .device{{width:360px;height:360px}} .hero .device img,.hero .device video{{inset:40px;width:280px;height:280px}} .hero .device::before{{inset:18px}}
.strip{{display:grid;gap:18px}}
.rail{{display:flex;gap:20px;overflow-x:auto;scroll-snap-type:x mandatory;padding-block:8px 18px;margin-inline:-20px;padding-inline:20px;scrollbar-width:thin}}
.slide{{flex:0 0 min(320px,84vw);scroll-snap-align:start;background:var(--panel);border:1px solid var(--line);border-radius:28px;padding:28px 22px 26px;display:grid;gap:22px;justify-items:center}}
.slide .device{{width:240px;height:240px}} .slide .device img,.slide .device video{{inset:22px;width:196px;height:196px}} .slide .device::before{{inset:11px}}
.cap{{justify-self:stretch}} .cap .n{{font-family:var(--mono);font-size:12px;color:var(--muted)}}
.cap h3{{font-size:20px;margin-block:6px 8px}} .cap p{{font-size:15px;color:var(--muted)}}
.grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:28px 32px}}
.grid h3{{font-size:17px;margin-bottom:6px}} .grid p{{font-size:15px;color:var(--muted)}}
table{{border-collapse:collapse;width:100%;font-size:15px}} th,td{{text-align:left;padding:12px 10px;border-bottom:1px solid var(--line);vertical-align:top}}
th{{font-family:var(--mono);font-weight:500;font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}}
td:first-child{{font-weight:500;white-space:nowrap}}
.nums{{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:18px;border-top:1px solid var(--line);border-bottom:1px solid var(--line);padding-block:22px}}
.nums b{{display:block;font-family:var(--mono);font-weight:500;font-size:26px;font-variant-numeric:tabular-nums}} .nums span{{font-size:13px;color:var(--muted)}}
.warm{{color:var(--warm)}} .cool{{color:var(--cool)}}
footer{{font-size:13px;color:var(--muted)}}
a{{color:var(--cool);text-underline-offset:4px}}
.links{{display:flex;flex-wrap:wrap;gap:12px 24px;margin-top:24px;align-items:center}}
.links a{{font-weight:500}}
.motion-toggle{{font:inherit;color:var(--ink);background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:8px 14px;cursor:pointer}}
:focus-visible{{outline:3px solid var(--cool);outline-offset:5px}}
@media (prefers-reduced-motion:reduce){{.rail{{scroll-behavior:auto;scroll-snap-type:none}}}}
@media (max-width:760px){{.hero{{grid-template-columns:1fr}} .hero .device{{width:300px;height:300px}} .hero .device img,.hero .device video{{inset:30px;width:240px;height:240px}} .hero .device::before{{inset:14px}}}}
</style>
</head>
<body>
<main class="wrap">
<section class="hero">
  <div>
    <div class="eyebrow">sleepypod Dial · for the Eight Sleep Pod</div>
    <h1>One arc. One number. Nothing to learn in the dark.</h1>
    <p class="lede">A bedside knob that shows where your mattress is, where it is going, and lets you turn it, click it off, and leave it alone. Every frame on this page was captured from the device.</p>
    <nav class="links" aria-label="Project links">
      <a href="https://github.com/sleepypod/m5-rotary-dial#setup">Build your dial →</a>
      <a href="https://github.com/sleepypod/m5-rotary-dial">Source on GitHub</a>
      <button class="motion-toggle" type="button" aria-pressed="false">Pause animations</button>
    </nav>
  </div>
  <div class="device">{('<video src="' + b64("video/mp4", clips["loader"]) + '" muted loop playsinline width="280" height="280"></video>') if clips.get("loader") else ('<img src="' + img["heating"] + '" alt="Heating" width="280" height="280">')}</div>
</section>
<section class="strip">
  <div><div class="eyebrow">Walkthrough</div><h2>Nine screens, in the order you meet them</h2></div>
  <div class="rail">{"".join(slide(i + 1, k, h, p) for i, (k, h, p) in enumerate(slides))}</div>
</section>
{('<section class="strip"><div><div class="eyebrow">In motion</div><h2>Four things you will do every night</h2></div><div class="rail">' + videos + '</div></section>') if videos else ''}
<section class="nums">
  <div><b>55–110<span style="font-size:15px"> °F</span></b><span>the Pod's range, 1° per detent</span></div>
  <div><b class="warm">2°</b><span>per detent when you spin, never more</span></div>
  <div><b>1.5 s</b><span>hold for settings, ring shows progress</span></div>
  <div><b class="cool">0 dB</b><span>after 10 pm, no sounds at all</span></div>
  <div><b>19 ms</b><span>per frame, encoder polled every 1 ms</span></div>
</section>
<section class="strip">
  <div><div class="eyebrow">Controls</div><h2>Five things, and only five</h2></div>
  <div style="overflow-x:auto"><table>
    <tr><th>You do</th><th>It does</th></tr>
    <tr><td>Turn</td><td>Moves the target. 1° per detent, 2° when you spin. Two detents below 55° reach an off stop.</td></tr>
    <tr><td>Click, or tap ⏻</td><td>Turns your side off, or back on at the last setpoint.</td></tr>
    <tr><td>Hold, or tap ⚙</td><td>Opens settings after a ring fills. Release early and nothing happens.</td></tr>
    <tr><td>Settings › Side</td><td>Picks Left or Right once. It is remembered and never changes by accident.</td></tr>
    <tr><td>Touch while dim</td><td>Only wakes the screen. The next turn counts.</td></tr>
  </table></div>
</section>
<section class="grid">
  <div><h3>Local only</h3><p>Talks to sleepypod-core on your network. Finds the Pod by mDNS. No cloud, no account.</p></div>
  <div><h3>Honest about the network</h3><p>A hollow cap means the Pod has not confirmed yet. "Pod offline" and "No Wi-Fi" say so in words.</p></div>
  <div><h3>Your name on it</h3><p>The side name comes from the Pod's settings, so the dial says Jon, not L.</p></div>
  <div><h3>Your changes win</h3><p>The Pod's own state never overwrites a number you touched in the last 30 seconds.</p></div>
</section>
<footer>Frames captured from an M5Stack Dial running the sleepypod-mt-rotary-dial firmware via tools/walkthrough.py. Names shown are example side names from the Pod's settings.</footer>
</main>
<script>
const videos = [...document.querySelectorAll('video')];
const button = document.querySelector('.motion-toggle');
let paused = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
function updateMotion() {{
  videos.forEach(video => {{
    if (paused) video.pause();
    else video.play().catch(() => {{}});
  }});
  button.textContent = paused ? 'Play animations' : 'Pause animations';
  button.setAttribute('aria-pressed', String(paused));
}}
button.addEventListener('click', () => {{ paused = !paused; updateMotion(); }});
updateMotion();
</script>
</body>
</html>
'''
    with open(PAGE, "w", encoding="utf-8") as f:
        f.write(html)
    print("page", PAGE, len(html) // 1024, "KB")
    # Pages uses the same content with cacheable assets instead of data URLs.
    site = html
    for name, data in stills.items():
        site = site.replace(b64("image/png", data), f"screens/{name}.png")
    for name, data in clips.items():
        if data:
            site = site.replace(b64("video/mp4", data), f"video/{name}.mp4")
    site_path = os.path.join(ROOT, "docs", "index.html")
    with open(site_path, "w", encoding="utf-8") as f:
        f.write(site)
    print("site", site_path, len(site) // 1024, "KB")


if __name__ == "__main__":
    main()
