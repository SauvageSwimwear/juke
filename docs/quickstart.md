# Jukebox TUI — Quickstart

## Prerequisites

```bash
pip install textual paho-mqtt
```

## Launch

```bash
./juke
```

This starts everything as one unit: the `conductor` playback engine runs in the background
(reads MIDI files, drives the mesh, handles MQTT commands) while the `listview` TUI runs in
front. Quitting the TUI stops the engine too. Engine output goes to `conductor.log`.

### Running the pieces separately (for development)

The two scripts can still be run by hand in separate terminals — start the engine first, since
the TUI is only a remote control and the conductor must be running for anything to play:

```bash
python3 conductor/conductor.py   # engine — leave running
python3 listview.py              # TUI remote (separate terminal)
```

## Keys

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate track list |
| `Enter` | Play selected track |
| `Space` | Pause / Resume |
| `w` | Skip to next |
| `e` | Stop (pause without advancing) |
| `s` | Shuffle |
| `y` | Volume up (+10) |
| `t` | Volume down (−10) |
| `q` / `Esc` | Quit |

## Notes

- **Track list** is sorted newest-first by file modification time, scanned once at startup.
- **Track names** are the `.mid` filenames with dots replaced by spaces for readability.
- **Status bar** (bottom) shows current volume, state, and track name — optimistic (reflects what was sent, not confirmed playback).
- **MQTT broker** is at `10.0.1.110:1884`. Both conductor and TUI connect to it independently; you can run them on different machines on the same network.
- Volume range is 0–127 (MIDI standard). Default is 100.
