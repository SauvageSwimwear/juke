// Minimal Standard MIDI File (format 0 and 1) streaming player.
//
// Why this exists instead of a library: MD_MIDIFile (the usual Arduino SMF
// player) hard-requires SdFat, which is built for SD cards or a separate SPI
// flash chip -- this board has neither (single internal 16MB flash, LittleFS).
//
// One shared File handle for the whole SMF file is used, with each track
// tracked only as a stored byte offset (not its own File object) -- ESP32's
// LittleFS VFS layer does not reliably support multiple concurrent open
// handles on the same file, which was crashing/rebooting the board when this
// used one File per track.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <string.h>
#include "MidiOut.h"

// Covers every file in the user's classical library except one 140-track
// outlier (Haendel Concerto Grosso op6 n12); extra tracks beyond this are
// silently dropped rather than crashing, just missing some parts.
#define SMF_MAX_TRACKS 64

// Note-thinning under UART backlog: once the TX buffer is this full, drop
// note-on events quieter than the threshold instead of queuing everything --
// fewer bytes during dense passages beats a snowballing backlog. Note-offs
// are never dropped (a dropped note-off risks a stuck/hung note, which is
// worse than losing one quiet note-on).
#define THIN_BACKLOG_THRESHOLD 0.6f
#define THIN_VELOCITY_CUTOFF   45

class SmfPlayer {
public:
  bool begin(fs::FS &fs, const char *path);
  void end();

  // Call every loop() iteration. Returns false once the whole file is done.
  bool update();

  // Suspend playback without losing position. Sends all-notes-off immediately.
  // Call resume() to continue from the same point.
  void pause();
  void resume();
  void setVolume(uint8_t vol);     // CC#7 on all 16 channels, 0-127
  void setNoteOnCallback(void (*cb)(uint8_t ch, uint8_t vel)); // fires on every NoteOn
  bool consumeBeatActivity();      // true if a quarter-note boundary was crossed since last call

  bool isDone()   const { return _done; }
  bool isPaused() const { return _paused; }
  const char *path() const { return _path; }

private:
  struct Track {
    uint32_t pos;          // file offset of the next unread byte for this track
    uint32_t dataEnd;      // file offset where this track chunk ends
    uint32_t nextTick;     // absolute tick of the next pending event (valid if !ended)
    uint8_t runningStatus; // last channel-voice status byte seen, for running status
    bool ended;
  };

  fs::FS *_fs = nullptr;
  char _path[64] = { 0 };
  File _file;  // single shared handle for the whole SMF file
  MidiOut _midi;
  bool _midiStarted = false;

  uint16_t _division = 96;      // ticks per quarter note (SMPTE division not supported)
  uint32_t _usPerQuarter = 500000; // default 120 BPM

  uint32_t _baseTick = 0;
  unsigned long _baseMillis = 0;

  Track _tracks[SMF_MAX_TRACKS];
  uint8_t _numTracks = 0;
  bool _done   = true;
  bool _paused = false;
  unsigned long _pausedAt = 0;

  void (*_noteOnCb)(uint8_t ch, uint8_t vel) = nullptr;
  bool     _beatActivity  = false;
  uint32_t _lastBeatCount = 0;

  uint32_t readVarLen(bool *ok);   // reads from _file at its current position
  uint32_t read32();
  uint16_t read16();
  bool primeTrack(Track &t);      // seeks _file to t.pos, reads next delta, sets nextTick
  void processEvent(Track &t, uint32_t tick); // seeks _file to t.pos first
  unsigned long msForTick(uint32_t tick) const;
};
