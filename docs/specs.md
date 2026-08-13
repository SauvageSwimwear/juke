# Pi-Side Interface — Brief for Next Crew

## What the Pi needs to do
The Pi is the source of truth — it hosts the MIDI files and broadcasts the stream over ESP-NOW to all performer nodes. This doc covers only the **control interface** running on the Pi itself, not the nodes listening to the broadcast.

## Requirements
- **Must have:** cue up and play a specific track from a directory of files
- **Nice to have:** play/pause, current-track display, 16 status lights showing live MIDI channel activity

## Direction: Textual TUI
We're going with a [Textual](https://textual.textualize.io/) (Python terminal UI) app running on the Pi, controlled via SSH or a terminal at the rig — no browser, no web server to maintain.

- Track list = scan a default directory on startup, populate a scrollable `ListView`
- Select a file → publish to the existing MQTT control topic (`jukebox/control`, same scheme already proven out: skip/pause/resume/play/`<filename>`/volume)
- Play/pause and now-playing are just additional widgets reading MQTT status
- The 16-channel activity lights are a **separate concern** — that's its own broadcast-listening node (like the other performers), not something built into this interface. Optionally, a Textual widget could mirror that same status for an on-screen version, but it's not required for v1.

## Scope note
Keep the TUI to control/status only. It doesn't need to know about individual performer nodes, filtering logic, or anything downstream of the broadcast — its only job is: pick a track, send it, show what's happening.

## Starter code
A rough working draft exists (directory scan → `ListView` → MQTT publish on selection). Recommend handing it over as a **starting skeleton, not a finished spec** — it proves the pattern (scan dir, populate list, publish on select) but hasn't been used, so the crew should treat it as scaffolding to build on rather than something to preserve as-is. Flag the open decisions inline as comments:
- static scan vs. live directory refresh
- sort order (alphabetical vs. most-recent-upload)
- visual state for "now playing" item
- whether MQTT status subscribe (for live play/pause/now-playing feedback) gets added now or later
