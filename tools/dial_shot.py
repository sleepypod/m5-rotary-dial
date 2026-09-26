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
buf=bytearray(); t=time.time()
while time.time()-t<30:
    c=s.read(65536)
    if c:
        buf+=c
        if b'<<END>>' in buf: break
text=buf.decode(errors='replace')
body=text.split('<<FB>>',1)[1].split('<<END>>',1)[0]
hexs=''.join(body.split())
W,H=240,240
raw=bytearray(); n=0
for i in range(0,len(hexs),6):
    run=int(hexs[i:i+2],16); v=int(hexs[i+2:i+6],16)
    v=((v&0xFF)<<8)|(v>>8)   # sprite stores RGB565 byte-swapped
    rgb=bytes((((v>>11)&0x1F)*255//31,((v>>5)&0x3F)*255//63,(v&0x1F)*255//31))
    for _ in range(run):
        if n%W==0: raw.append(0)
        raw+=rgb; n+=1
print('pixels',n)
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(bytes(raw)))+chunk(b'IEND',b'')
open(out,'wb').write(png); print('wrote',out)
