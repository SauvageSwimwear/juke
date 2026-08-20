#!/usr/bin/env python3
"""
colorviz_watch — tie the web UI's lifetime to the sensor.

Watches the retained MQTT status topic. When the colorfin sketch comes
online it launches colorviz.py; when the sketch goes offline (clean stop
or Last-Will on disconnect) it shuts the web UI down. Survives power
cycles: the UI relinks automatically the next time the sensor boots.

Run:    python3 colorviz_watch.py
Stop:   Ctrl-C  (also stops the web UI)
"""
import os
import signal
import subprocess
import sys
import paho.mqtt.client as mqtt

MQTT_HOST = "10.0.1.110"
MQTT_PORT = 1884
MQTT_USER = "mqttuser"
MQTT_PASS = "pibb"
TOPIC_STATUS = "colorfin/status"

HERE = os.path.dirname(os.path.abspath(__file__))
VIZ = os.path.join(HERE, "colorviz.py")

child: subprocess.Popen | None = None


def start_viz():
    global child
    if child and child.poll() is None:
        return
    print("[watch] sensor online  -> starting web UI")
    child = subprocess.Popen([sys.executable, VIZ])


def stop_viz():
    global child
    if child and child.poll() is None:
        print("[watch] sensor offline -> stopping web UI")
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
    child = None


def on_connect(client, userdata, flags, rc, properties=None):
    print(f"[watch] mqtt connected rc={rc}; waiting for sensor status")
    client.subscribe(TOPIC_STATUS)


def on_message(client, userdata, msg):
    status = msg.payload.decode("utf-8", "replace").strip()
    if status == "online":
        start_viz()
    elif status == "offline":
        stop_viz()


def shutdown(*_):
    print("\n[watch] shutting down")
    stop_viz()
    sys.exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="colorviz-watch")
    c.username_pw_set(MQTT_USER, MQTT_PASS)
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(MQTT_HOST, MQTT_PORT, 60)
    c.loop_forever()
