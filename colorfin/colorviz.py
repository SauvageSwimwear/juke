#!/usr/bin/env python3
"""
colorviz — live web display for the AS7341 color finder.

Subscribes to the colorfin MQTT topics and streams each reading to a browser
(Server-Sent Events). Shows a big live swatch, hex/RGB, the 8-band spectrum,
and calibration / online status.

Run:   python3 colorviz.py
Open:  http://10.0.1.110:8000   (or the Pi's IP from any device on the LAN)
"""
import json
import queue
import threading
import paho.mqtt.client as mqtt
from flask import Flask, Response, render_template_string

# ── config (matches the sketch) ──────────────────────────────────────────────
MQTT_HOST = "10.0.1.110"
MQTT_PORT = 1884
MQTT_USER = "mqttuser"
MQTT_PASS = "pibb"
TOPIC_READING = "colorfin/reading"
TOPIC_STATUS = "colorfin/status"
WEB_PORT = 8000

# ── shared state: latest reading + fan-out to connected browsers ─────────────
state = {"reading": None, "status": "offline"}
subscribers: list[queue.Queue] = []
lock = threading.Lock()


def broadcast(payload: str):
    with lock:
        dead = []
        for q in subscribers:
            try:
                q.put_nowait(payload)
            except queue.Full:
                dead.append(q)
        for q in dead:
            subscribers.remove(q)


# ── MQTT ─────────────────────────────────────────────────────────────────────
def on_connect(client, userdata, flags, rc, properties=None):
    print(f"[mqtt] connected rc={rc}")
    client.subscribe(TOPIC_READING)
    client.subscribe(TOPIC_STATUS)


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", "replace")
    if msg.topic == TOPIC_STATUS:
        state["status"] = text
        broadcast(json.dumps({"status": text}))
        return
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return
    state["reading"] = data
    data["status"] = state["status"]
    broadcast(json.dumps(data))


def mqtt_thread():
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="colorviz")
    c.username_pw_set(MQTT_USER, MQTT_PASS)
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(MQTT_HOST, MQTT_PORT, 60)
    c.loop_forever()


# ── web ──────────────────────────────────────────────────────────────────────
app = Flask(__name__)

PAGE = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>colorfin — live</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: -apple-system, system-ui, sans-serif;
    background: #0d0d10; color: #e8e8ea; min-height: 100vh;
    display: flex; flex-direction: column; align-items: center;
  }
  h1 { font-weight: 600; font-size: 15px; letter-spacing: .18em;
       text-transform: uppercase; color: #6b6b76; margin: 22px 0 4px; }
  #swatch {
    width: min(78vw, 420px); height: min(78vw, 420px); border-radius: 26px;
    margin: 14px 0; box-shadow: 0 18px 60px rgba(0,0,0,.55);
    transition: background .25s ease; border: 1px solid rgba(255,255,255,.06);
  }
  #hex { font-size: 34px; font-weight: 700; letter-spacing: .04em; margin: 2px; }
  #rgb { font-size: 14px; color: #8a8a95; margin-bottom: 4px; }
  #meta { font-size: 12px; color: #6b6b76; margin-bottom: 18px; height: 16px; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%;
         background:#555; margin-right:6px; vertical-align: middle; }
  .dot.on { background:#39d353; box-shadow:0 0 8px #39d353; }
  #spectrum {
    display: flex; align-items: flex-end; gap: 6px; height: 130px;
    width: min(88vw, 460px); padding: 0 4px; margin-bottom: 30px;
  }
  .band { flex: 1; display: flex; flex-direction: column; align-items: center;
          justify-content: flex-end; height: 100%; }
  .bar { width: 100%; border-radius: 4px 4px 2px 2px; transition: height .2s ease;
         min-height: 2px; }
  .band small { font-size: 9px; color: #6b6b76; margin-top: 5px; }
</style>
</head>
<body>
  <h1>colorfin</h1>
  <div id="swatch"></div>
  <div id="hex">#------</div>
  <div id="rgb">—</div>
  <div id="meta"><span class="dot" id="statusdot"></span><span id="statustext">waiting…</span></div>
  <div id="spectrum"></div>

<script>
// 8 AS7341 bands with approximate display colors
const BANDS = [
  ["f1","415", "#7d00ff"], ["f2","445", "#0040ff"], ["f3","480", "#00b7ff"],
  ["f4","515", "#33ff00"], ["f5","555", "#b6ff00"], ["f6","590", "#ffd000"],
  ["f7","630", "#ff5a00"], ["f8","680", "#ff0000"],
];
const spec = document.getElementById("spectrum");
const bars = {};
for (const [key,nm,col] of BANDS) {
  const band = document.createElement("div"); band.className = "band";
  const bar = document.createElement("div"); bar.className = "bar";
  bar.style.background = col;
  const lab = document.createElement("small"); lab.textContent = nm;
  band.appendChild(bar); band.appendChild(lab); spec.appendChild(band);
  bars[key] = bar;
}
const hex2 = n => n.toString(16).padStart(2,"0");
function apply(d) {
  if (d.status !== undefined) {
    const on = d.status === "online";
    document.getElementById("statusdot").className = "dot" + (on ? " on" : "");
  }
  if (d.r === undefined) return;
  const hex = ("#" + hex2(d.r) + hex2(d.g) + hex2(d.b)).toUpperCase();
  document.getElementById("swatch").style.background = hex;
  document.getElementById("hex").textContent = hex;
  document.getElementById("rgb").textContent = `rgb(${d.r}, ${d.g}, ${d.b})`;
  const cal = (d.white && d.black) ? "calibrated" : "UNCALIBRATED — run black + white";
  const sat = d.sat ? " · SATURATED" : "";
  document.getElementById("statustext").textContent =
    (d.status || "online") + " · " + cal + sat;
  // spectrum: scale bars to the peak band in this reading
  const peak = Math.max(1, ...BANDS.map(([k]) => d[k] || 0));
  for (const [k] of BANDS)
    bars[k].style.height = Math.round((d[k]||0) / peak * 100) + "%";
}
const es = new EventSource("/stream");
es.onmessage = e => { try { apply(JSON.parse(e.data)); } catch(_) {} };
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return render_template_string(PAGE)


@app.route("/stream")
def stream():
    def gen():
        q: queue.Queue = queue.Queue(maxsize=10)
        with lock:
            subscribers.append(q)
        # prime the new client with whatever we last saw
        if state["reading"]:
            primed = dict(state["reading"], status=state["status"])
            yield f"data: {json.dumps(primed)}\n\n"
        else:
            yield f"data: {json.dumps({'status': state['status']})}\n\n"
        try:
            while True:
                try:
                    payload = q.get(timeout=15)
                    yield f"data: {payload}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        finally:
            with lock:
                if q in subscribers:
                    subscribers.remove(q)

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache",
                             "X-Accel-Buffering": "no"})


if __name__ == "__main__":
    threading.Thread(target=mqtt_thread, daemon=True).start()
    print(f"[web] http://{MQTT_HOST}:{WEB_PORT}")
    app.run(host="0.0.0.0", port=WEB_PORT, threaded=True)
