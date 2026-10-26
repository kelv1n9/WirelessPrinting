# Serves the local copy of the page and forwards everything else to the module,
# so the layout can be looked at with real files and real temperatures.
#   python3 scripts/uiproxy.py [--canned]    потом открыть http://127.0.0.1:8787
# --canned отвечает заготовленными данными вместо модуля, когда тот занят печатью.
import http.server, urllib.request, urllib.error, sys, os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(HERE, "ui", "index.html")
MODULE = os.environ.get("KP3S", "http://kp3s.local")

# Taken off the live module, so the layout is laid out over the real thing: long names,
# a running print, a full console.
CANNED = {
  "/api/status": """{"state":"Printing","connected":true,"printing":true,"paused":false,
  "temperature":{"tool":{"actual":244.62,"target":245.0},"bed":{"actual":84.97,"target":85.0}},
  "job":{"file":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","size":22020110,"position":9241284,
  "completion":41.97,"completionFrom":"position","elapsed":4620,"remaining":6390,"remainingFrom":"slicer",
  "layer":118,"layers":251,"z":23.6,"fan":80},
  "link":{"baud":115200,"sent":11584302,"resent":0,"mangled":0},
  "energy":{"wh":612.4,"watt":214.8,"power":0.2148,"grams":188.42,"filament":0.0,"cost":7.58,"tariff":0.077,"kilo":40.0},
  "storage":{"card":true,"selected":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","free":59528000000,"total":62708350976},
  "socket":{"token":true,"device":"\u041f\u0440\u0438\u043d\u0442\u0435\u0440","auto":true,"state":"armed, printer still warm","status":"ok"},
  "system":{"heap":138024,"uptime":5231,"epoch":1792871000,"rssi":-56,"machine":"KP3S","bootReason":"power on","version":"2e76aef"}}""",
  "/api/console": """{"lines":[],"seq":0}""",
  "/api/files": """{"files":[
  {"name":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","size":22020110},
  {"name":"xyz_cube_PETG_12m50s.gcode","size":371944},
  {"name":"Calibration_PETG.gcode","size":67800},
  {"name":"Calibration_TPU95A_5m20s.gcode","size":66471},
  {"name":"Latch_PETG_14m24s.gcode","size":562176}],"free":59528000000,"total":62708350976}""",
  "/api/files/local": """{"files":[
  {"name":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","path":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","display":"Remote_Back_Shell_PETG-CF_4h33m_v2_final.gcode","origin":"local","size":22020110,"date":1792800000,"type":"machinecode","typePath":["machinecode","gcode"],"selected":true,"refs":{"download":"/download?name=a"}},
  {"name":"xyz_cube_PETG_12m50s.gcode","path":"xyz_cube_PETG_12m50s.gcode","display":"xyz_cube_PETG_12m50s.gcode","origin":"local","size":371944,"date":1792700000,"type":"machinecode","typePath":["machinecode","gcode"],"selected":false,"refs":{"download":"/download?name=b"}},
  {"name":"Calibration_PETG.gcode","path":"Calibration_PETG.gcode","display":"Calibration_PETG.gcode","origin":"local","size":67800,"date":1792600000,"type":"machinecode","typePath":["machinecode","gcode"],"selected":false,"refs":{"download":"/download?name=c"}},
  {"name":"Calibration_TPU95A_5m20s.gcode","path":"Calibration_TPU95A_5m20s.gcode","display":"Calibration_TPU95A_5m20s.gcode","origin":"local","size":66471,"date":1792500000,"type":"machinecode","typePath":["machinecode","gcode"],"selected":false,"refs":{"download":"/download?name=d"}},
  {"name":"Latch_PETG_14m24s.gcode","path":"Latch_PETG_14m24s.gcode","display":"Latch_PETG_14m24s.gcode","origin":"local","size":562176,"date":1792400000,"type":"machinecode","typePath":["machinecode","gcode"],"selected":false,"refs":{"download":"/download?name=e"}}],
  "free":59528000000,"total":62708350976}""",
}

if "--canned" not in sys.argv:
    CANNED = {}          # by default the real module answers, this is only for when it is busy

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = open(PAGE, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if False:
            import os
            pic = "/tmp/th_Lid_PETG_2h8m.gcode.png"
            if os.path.exists(pic):
                body = open(pic, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(404); self.end_headers(); return

        if self.path.split("?")[0] in CANNED:
            body = CANNED[self.path.split("?")[0]].encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        try:
            with urllib.request.urlopen(MODULE + self.path, timeout=20) as r:
                body = r.read()
                self.send_response(r.status)
                self.send_header("Content-Type", r.headers.get("Content-Type", "application/octet-stream"))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        except Exception as e:
            self.send_response(502); self.end_headers()
            try: self.wfile.write(str(e).encode())
            except Exception: pass

    do_POST = do_GET

http.server.ThreadingHTTPServer(("127.0.0.1", 8787), Handler).serve_forever()
