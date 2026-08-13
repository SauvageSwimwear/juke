import json
import threading
import time

import mido
import paho.mqtt.client as mqtt
import serial

from config import (MIDI_DIR, MQTT_CTL, MQTT_HOST, MQTT_PASS, MQTT_PORT, MQTT_STATUS, MQTT_USER,
                    NOTE_TIMEOUT, PANIC_REPEAT, SERIAL_BAUD, SERIAL_PORT, SHED_BACKLOG,
                    SHED_VELOCITY, WATCHDOG_INTERVAL)
from packet import (MSG_CC, MSG_CONFIG, MSG_NOTE_OFF, MSG_NOTE_ON, MSG_PROGRAM,
                    encode)
from playlist import Playlist

# ── shared state ──────────────────────────────────────────────────────────────
_ser:      serial.Serial | None = None
_playlist: Playlist | None      = None
_volume    = 100    # 0–127

_paused    = threading.Event()   # set = paused
_skip      = threading.Event()   # set = skip to next track
_lock      = threading.Lock()    # guards _volume, _playlist mutations

# Every note we've turned on but not yet turned off: (channel, note) → start time.
# Used to force Note-Off on panic and to time out notes whose Note-Off was
# dropped over ESP-NOW.
_active:     dict[tuple[int, int], float] = {}
_active_lock = threading.Lock()  # guards _active
_ser_lock    = threading.Lock()  # serializes serial writes across threads


# ── serial / note tracking ────────────────────────────────────────────────────
def _write(buf: bytes):
    """Thread-safe serial write (playback thread + watchdog thread both write)."""
    with _ser_lock:
        _ser.write(buf)


def _note_on(ch: int, note: int, vel: int):
    with _active_lock:
        _active[(ch, note)] = time.monotonic()
    _write(encode(MSG_NOTE_ON, ch, note, vel))


def _note_off(ch: int, note: int):
    with _active_lock:
        _active.pop((ch, note), None)
    _write(encode(MSG_NOTE_OFF, ch, note, 0))


def _panic():
    """Silence everything now. Sends an explicit Note-Off for each tracked note,
    then All-Notes-Off (CC123) + All-Sound-Off (CC120) + sustain-off (CC64) on all
    16 channels. Repeated PANIC_REPEAT times to survive ESP-NOW packet loss."""
    with _active_lock:
        notes = list(_active.keys())
        _active.clear()
    for _ in range(PANIC_REPEAT):
        for ch, note in notes:
            _write(encode(MSG_NOTE_OFF, ch, note, 0))
        for ch in range(16):
            _write(encode(MSG_CC, ch, 123, 0))   # all notes off
            _write(encode(MSG_CC, ch, 120, 0))   # all sound off
            _write(encode(MSG_CC, ch, 64, 0))    # sustain pedal off


def _watchdog():
    """Force Note-Off on any note that has been sounding longer than NOTE_TIMEOUT.
    Catches notes whose Note-Off was dropped over the lossy ESP-NOW link."""
    while True:
        time.sleep(WATCHDOG_INTERVAL)
        now = time.monotonic()
        with _active_lock:
            expired = [k for k, t in _active.items() if now - t > NOTE_TIMEOUT]
            for k in expired:
                del _active[k]
        for ch, note in expired:
            _write(encode(MSG_NOTE_OFF, ch, note, 0))


# ── helpers ───────────────────────────────────────────────────────────────────
def _sleep(seconds: float) -> bool:
    """Sleep for `seconds`, pausing the clock while _paused is set.
    Returns False if _skip fires during the sleep."""
    remaining = seconds
    while remaining > 0:
        if _skip.is_set():
            return False
        if _paused.is_set():
            while _paused.is_set():
                if _skip.is_set():
                    return False
                time.sleep(0.02)
            continue    # don't decrement remaining while paused
        chunk = min(0.02, remaining)
        time.sleep(chunk)
        remaining -= chunk
    return True


def _send_volume(volume: int):
    for ch in range(16):
        _write(encode(MSG_CC, ch, 7, volume))


def _publish_status(client, state: str, path: str | None = None):
    payload = {"state": state}
    if path:
        import os
        payload["file"] = os.path.basename(path)
    client.publish(MQTT_STATUS, json.dumps(payload))


# ── MIDI playback ─────────────────────────────────────────────────────────────
def _play_file(path: str):
    """Play one MIDI file. Returns when finished, skipped, or on error."""
    mid = mido.MidiFile(path)
    for msg in mid:
        # msg.time is seconds (mido converts ticks→seconds via tempo map)
        if not _sleep(msg.time):
            return  # skip requested

        if msg.type == "note_on":
            vel = msg.velocity
            if vel == 0:                        # note_on v=0 is note_off
                _note_off(msg.channel, msg.note)
            else:
                if _ser.out_waiting > SHED_BACKLOG and vel < SHED_VELOCITY:
                    continue                    # load-shed
                _note_on(msg.channel, msg.note, vel)

        elif msg.type == "note_off":
            _note_off(msg.channel, msg.note)

        elif msg.type == "program_change":
            _write(encode(MSG_PROGRAM, msg.channel, msg.program, 0))

        elif msg.type == "control_change":
            _write(encode(MSG_CC, msg.channel, msg.control, msg.value))


# ── MQTT callbacks ────────────────────────────────────────────────────────────
def _on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"MQTT connected → {MQTT_HOST}:{MQTT_PORT}")
        client.subscribe(MQTT_CTL)
    else:
        print(f"MQTT connect failed rc={reason_code}")


def _on_message(client, userdata, msg):
    global _volume
    cmd = msg.payload.decode().strip()
    print(f"← {cmd}")

    if cmd == "pause":
        _paused.set()
        _panic()                # kill notes still ringing at the pause point
        _publish_status(client, "paused", _playlist.current() if _playlist else None)

    elif cmd == "resume":
        _paused.clear()
        _publish_status(client, "playing", _playlist.current() if _playlist else None)

    elif cmd == "skip":
        _skip.set()
        _panic()

    elif cmd == "shuffle":
        with _lock:
            if _playlist:
                _playlist.shuffle()
        print("Playlist shuffled")

    elif cmd.startswith("play/"):
        filename = cmd[5:]
        with _lock:
            if _playlist and _playlist.select(filename):
                _paused.clear()
                _skip.set()     # interrupt current track; main loop will pick it up
                _panic()
                print(f"Jumping to {filename}")
            else:
                print(f"Not found: {filename}")

    elif cmd.startswith("volume/"):
        try:
            vol = int(cmd[7:])
            vol = max(0, min(127, vol))
            with _lock:
                _volume = vol
            _send_volume(vol)
            print(f"Volume → {vol}")
        except ValueError:
            pass


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    global _ser, _playlist

    _ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0)
    print(f"Serial {SERIAL_PORT} @ {SERIAL_BAUD}")

    _playlist = Playlist(MIDI_DIR)
    print(f"Playlist: {len(_playlist)} files")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = _on_connect
    client.on_message = _on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()

    threading.Thread(target=_watchdog, daemon=True).start()

    try:
        while True:
            path = _playlist.current()
            if not path:
                time.sleep(1)
                continue

            print(f"▶  {path}")
            _skip.clear()
            _publish_status(client, "playing", path)

            _play_file(path)
            _panic()            # clear any notes left ringing (skip mid-track, etc.)

            _publish_status(client, "stopped", path)
            _playlist.advance()

    except KeyboardInterrupt:
        pass
    finally:
        client.loop_stop()
        _panic()
        _send_volume(0)
        _ser.close()


if __name__ == "__main__":
    main()
