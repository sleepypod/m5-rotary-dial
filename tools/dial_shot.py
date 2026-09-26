# Drive the Dial over USB serial: send debug commands, then grab the framebuffer as a PNG.
# usage: ~/.platformio/penv/bin/python tools/dial_shot.py <out.png> [commands...]
#   commands: + - (detents) c (click) o (power) n (night) z (dim) w (wake)
# example: tools/dial_shot.py shot.png c c   -> screenshot of "Both" mode
import serial, sys, time, zlib, struct, os
port=os.environ.get("DIAL_PORT","/dev/cu.usbmodem21201")
out=sys.argv[1]; cmds=sys.argv[2:]
s=serial.Serial(port,115200,timeout=2)
time.sleep(0.3); s.reset_input_buffer()
for c in cmds:
    s.write(c.encode()); s.flush(); time.sleep(0.35 if c in 'co' else 0.12)
time.sleep(0.6)
s.reset_input_buffer()
s.write(b'p'); s.flush()
rows=[]; started=False; t=time.time()
while time.time()-t<40:
    line=s.readline().decode(errors='replace').strip()
    if not line: continue
    if line=='<<FB>>': started=True; continue
    if line=='<<END>>': break
    if started and len(line)==960: rows.append(line)
    elif started: print('bad row', len(line))
print('rows', len(rows))
W,H=240,240
raw=bytearray()
for r in rows:
    raw.append(0)
    for x in range(W):
        v=int(r[x*4:x*4+4],16)
        v=((v&0xFF)<<8)|(v>>8)   # sprite stores RGB565 byte-swapped
        R=((v>>11)&0x1F)*255//31; G=((v>>5)&0x3F)*255//63; B=(v&0x1F)*255//31
        raw += bytes((R,G,B))
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,len(rows),8,2,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw)))+chunk(b'IEND',b'')
open(out,'wb').write(png); print('wrote',out)
