#!/usr/bin/env python3
"""Live viewer for an Android-on-UML guest, in a web browser.

  umlview.py FRAMEFILE FIFO [PORT] [ADDRESS]

Serves http://localhost:PORT/ (default 6080), only on this device unless
ADDRESS is given (0.0.0.0 makes it reachable from the network, including
touch control, so only do that on a network you trust): the page shows the exported
frames (vkms.export=FRAMEFILE) and turns mouse/touch input into touch
events on the guest (umltouch=FIFO). Buttons send Back, Home and Recents.
Only the Python standard library is used, so it also runs in Termux.
"""
import http.server, json, mmap, os, struct, sys, threading, time

HDR = struct.Struct("<6I2Q")
MAGIC = 0x42464D55
FRAME, FIFO = sys.argv[1], sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 6080
ADDR = sys.argv[4] if len(sys.argv) > 4 else "127.0.0.1"

PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Android on UML</title><style>
body{margin:0;background:#111;color:#ddd;font:14px sans-serif;display:flex;flex-direction:column;align-items:center}
canvas{display:block;width:auto;height:auto;max-width:100vw;max-height:calc(100vh - 56px);touch-action:none;background:#000}
#bar{display:flex;gap:12px;padding:8px;align-items:center}button{font-size:16px;padding:6px 18px}
</style></head><body><canvas id=c></canvas><div id=bar>
<button data-k=158>Back</button><button data-k=172>Home</button><button data-k=580>Recents</button>
<span id=s>waiting for Android...</span></div><script>
const c=document.getElementById('c'),g=c.getContext('2d'),s=document.getElementById('s');let seq=-1,down=false;
async function loop(){try{const r=await fetch('/frame?after='+seq);if(r.status==200){seq=+r.headers.get('X-Seq');
const w=+r.headers.get('X-Width'),h=+r.headers.get('X-Height'),px=new Uint8ClampedArray(await r.arrayBuffer());
if(c.width!=w||c.height!=h){c.width=w;c.height=h}
g.putImageData(new ImageData(px,w,h),0,0);s.textContent='frame '+r.headers.get('X-Frames')}}catch(e){await new Promise(r=>setTimeout(r,500))}loop()}
function pos(e){const r=c.getBoundingClientRect();return[Math.round((e.clientX-r.left)*c.width/r.width),Math.round((e.clientY-r.top)*c.height/r.height)]}
function send(t,a,b){fetch('/input',{method:'POST',body:JSON.stringify([t,a,b])})}
c.onpointerdown=e=>{down=true;c.setPointerCapture(e.pointerId);send(1,...pos(e))};
c.onpointermove=e=>{if(down)send(1,...pos(e))};
c.onpointerup=c.onpointercancel=e=>{if(down){down=false;send(0,0,0)}};
document.querySelectorAll('button').forEach(b=>b.onclick=()=>{send(2,+b.dataset.k,1);send(2,+b.dataset.k,0)});
loop();</script></body></html>"""

fifo_lock = threading.Lock()
fifo_fd = None


def send_event(t, a, b):
    global fifo_fd
    with fifo_lock:
        if fifo_fd is None:
            fifo_fd = os.open(FIFO, os.O_RDWR)
        os.write(fifo_fd, struct.pack("<Iii", t, a, b))


def snapshot(m):
    for _ in range(100):
        magic, ver, w, h, stride, fmt, seq, frames = HDR.unpack_from(m, 0)
        if magic == MAGIC and not seq & 1 and w:
            data = m[4096:4096 + stride * h]
            if HDR.unpack_from(m, 0)[6] == seq:
                return seq, frames, w, h, stride, data
        time.sleep(0.005)
    return None


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/frame"):
            after = int(self.path.split("after=")[1]) if "after=" in self.path else -1
            deadline = time.time() + 10
            while True:
                try:
                    with open(FRAME, "rb") as f:
                        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                        snap = snapshot(m)
                except (FileNotFoundError, ValueError):
                    snap = None
                if snap and snap[0] != after:
                    break
                if time.time() > deadline:
                    self.send_response(204); self.end_headers(); return
                time.sleep(0.03)
            seq, frames, w, h, stride, data = snap
            # Raw RGBA rows, drawn with putImageData: compressing every frame
            # (PNG) costs more CPU than sending it over the loopback.
            body = data if stride == w * 4 else b"".join(
                data[y * stride:y * stride + w * 4] for y in range(h))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Width", str(w)); self.send_header("X-Height", str(h))
            self.send_header("X-Seq", str(seq)); self.send_header("X-Frames", str(frames))
            self.send_header("Cache-Control", "no-store")
            self.end_headers(); self.wfile.write(body)
        else:
            body = PAGE.encode()
            self.send_response(200); self.send_header("Content-Type", "text/html")
            self.end_headers(); self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        t, a, b = json.loads(self.rfile.read(n))
        send_event(int(t), int(a), int(b))
        self.send_response(204); self.end_headers()


class QuietServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Browsers drop the long-polling frame request when a page reloads
        # or goes to the background; that is not worth a traceback.
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


if __name__ == "__main__":
    print(f"Android on UML viewer: http://localhost:{PORT}/")
    QuietServer((ADDR, PORT), Handler).serve_forever()
