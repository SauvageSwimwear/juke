# colorfin

An ESP32 + AS7341 spectral color reader. The sensor measures reflected light
across 8 visible bands, converts it to an RGB color, and publishes every reading
over MQTT. A small Flask app on the Pi turns that stream into a live web display.

```
  AS7341 sensor ──I²C──> ESP32 ──WiFi/MQTT──> Mosquitto (Pi) ──> colorviz web UI
     (colorfin.ino)                                              (browser swatch)
```

## Files

| File | Runs on | What it does |
|------|---------|--------------|
| `colorfin.ino`      | ESP32 | Reads the sensor, calibrates, publishes MQTT |
| `colorviz.py`       | Pi    | Web page: live swatch + spectrum (Flask + SSE) |
| `colorviz_watch.py` | Pi    | Supervisor: runs the web UI only while the sensor is online |

## Hardware / wiring

AS7341 breakout → ESP32 DevKit, over I²C:

| Sensor | ESP32 |
|--------|-------|
| VIN | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

The sensor's on-board LED flashes on each reading to illuminate the sample.

## Flashing the firmware

Requires `arduino-cli` with the ESP32 core and the **Adafruit AS7341** +
**PubSubClient** libraries installed.

```sh
cd /home/pibb/juke/colorfin
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .
```

WiFi and MQTT settings live at the top of `colorfin.ino` (SSID, broker IP/port,
username/password). Edit them there before flashing if anything changes.

## Calibrating (do this every session — cal lives in RAM, not flash)

The sensor uses **two-point calibration** so black reads black and white reads
white, cancelling out ambient light. Both steps are required:

1. Rest the sensor on a **black** card → run `cfin black`
2. Rest the sensor on a **white** card → run `cfin white`

That's it — now measure real objects. Re-do both after any power cycle or reset.

## Everyday use — the `cfin` helper

A fish shortcut (`~/.config/fish/functions/cfin.fish`) wraps all the MQTT plumbing:

| Command | Does |
|---------|------|
| `cfin black` | Capture black reference |
| `cfin white` | Capture white reference |
| `cfin reset` | Clear calibration |
| `cfin stop` / `cfin start` | Pause / resume readings |
| `cfin watch` | Live raw JSON stream in the terminal |
| `cfin viz` | Live **web display**, linked to the sensor |

You can also type single letters at the Arduino serial monitor: `b` `w` `r` `s`.

## The web display

```sh
cfin viz
```

Then open **http://10.0.1.110:8000** from any device on the LAN. You get a big
live color swatch, hex/RGB readout, the 8-band spectrum as colored bars, and an
online/calibration status line — all updating in real time as you move the sensor.

`cfin viz` runs a supervisor: the web UI starts when the sensor comes online and
stops when it goes offline (it relinks automatically across power cycles). Press
**Ctrl-C** to stop the supervisor and the UI together.

## MQTT topics

| Topic | Payload |
|-------|---------|
| `colorfin/reading` | JSON: `f1`..`f8` bands, `clear`, `nir`, `r`/`g`/`b`, `white`/`black` cal flags, `sat`, gain/atime/astep |
| `colorfin/status`  | `online` / `offline` (retained, Last-Will) |
| `colorfin/cmd`     | Send `black` `white` `reset` `start` `stop` to control the sensor |

Broker: `10.0.1.110:1884` (user `mqttuser`).

## Notes

- WiFi/MQTT credentials are currently hardcoded in the source. Move them to an
  untracked config file before making this repo public.
