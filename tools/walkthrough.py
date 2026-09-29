# Capture the walkthrough from a connected Dial: stills for every state, short
# video clips of the animations for the unified Sleepypod documentation.
#
#   ~/.platformio/penv/bin/python tools/walkthrough.py            # everything
#   ~/.platformio/penv/bin/python tools/walkthrough.py --no-video # stills only
#   ~/.platformio/penv/bin/python tools/walkthrough.py --only settings  # re-record one clip
#   ~/.platformio/penv/bin/python tools/walkthrough.py --banner-only  # rebuild README banner
#
# Needs pyserial (PlatformIO's Python has it) and ffmpeg on PATH for video.
# Uses the firmware's serial debug channel (see handleSerialDebug in main.cpp).
# Fake mattress temperatures (h/l/a) are local only; the Pod is never written.
# Outputs: docs/screens/*.png (+ banner.png), docs/video/*.mp4 + *.gif
import os, shutil, struct, subprocess, sys, tempfile, time, zlib
import serial

PORT = os.environ.get("DIAL_PORT", "/dev/cu.usbmodem21201")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCREENS = os.path.join(ROOT, "docs", "screens")
VIDEO = os.path.join(ROOT, "docs", "video")
W = H = 240
NO_VIDEO = "--no-video" in sys.argv
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None  # capture one clip
BANNER_ONLY = "--banner-only" in sys.argv or "--page-only" in sys.argv  # legacy flag is an alias; never replaces the redirect



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


def main():
    os.makedirs(SCREENS, exist_ok=True)
    os.makedirs(VIDEO, exist_ok=True)
    if BANNER_ONLY:
        write_banner()
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


def write_banner():
    """README hero: three screens side by side at 2x on the panel background."""
    src = [os.path.join(SCREENS, n + ".png") for n in ("heating", "night", "night-dim")]
    out = os.path.join(SCREENS, "banner.png")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src[0], "-i", src[1], "-i", src[2],
                    "-filter_complex", "[0]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[a];[1]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[b];"
                    "[2]scale=480:480:flags=neighbor,pad=540:480:30:0:0x0B0E14[c];[a][b][c]hstack=3,pad=iw+60:ih+80:30:40:0x0B0E14", out], check=True)
    print("banner", out)


if __name__ == "__main__":
    main()
