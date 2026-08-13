#include "SmfPlayer.h"

uint32_t SmfPlayer::read32() {
  uint8_t b[4];
  _file.read(b, 4);
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

uint16_t SmfPlayer::read16() {
  uint8_t b[2];
  _file.read(b, 2);
  return ((uint16_t)b[0] << 8) | b[1];
}

uint32_t SmfPlayer::readVarLen(bool *ok) {
  uint32_t value = 0;
  uint8_t b;
  int n = 0;
  do {
    if (_file.position() >= _file.size() || n++ >= 4) { *ok = false; return 0; }
    b = _file.read();
    value = (value << 7) | (b & 0x7F);
  } while (b & 0x80);
  *ok = true;
  return value;
}

bool SmfPlayer::begin(fs::FS &fs, const char *path) {
  end();
  if (!_midiStarted) {
    _midi.begin(17, 31250);
    _midiStarted = true;
  }
  _fs = &fs;
  strncpy(_path, path, sizeof(_path) - 1);

  _file = fs.open(path, "r");
  if (!_file) return false;

  char tag[4];
  _file.read((uint8_t *)tag, 4);
  if (memcmp(tag, "MThd", 4) != 0) { _file.close(); return false; }
  uint32_t hdrLen = read32();
  uint16_t format = read16();
  uint16_t ntrks = read16();
  _division = read16();
  (void)format;
  if (_division & 0x8000) { _file.close(); return false; } // SMPTE time division not supported
  _file.seek(_file.position() + (hdrLen - 6)); // skip any extra header bytes

  _numTracks = 0;
  _usPerQuarter = 500000;
  _baseTick = 0;
  _baseMillis = millis();
  _done = false;
  _beatActivity = false;
  _lastBeatCount = 0;

  uint32_t pos = _file.position();
  for (uint16_t i = 0; i < ntrks && _numTracks < SMF_MAX_TRACKS; i++) {
    _file.seek(pos);
    char ttag[4];
    _file.read((uint8_t *)ttag, 4);
    if (memcmp(ttag, "MTrk", 4) != 0) break;
    uint32_t len = read32();
    uint32_t dataStart = _file.position();

    Track &t = _tracks[_numTracks];
    t.pos = dataStart;
    t.dataEnd = dataStart + len;
    t.runningStatus = 0;
    t.ended = false;
    t.nextTick = 0;
    if (primeTrack(t)) _numTracks++;

    pos = dataStart + len;
  }

  if (_numTracks == 0) { _done = true; return false; }
  Serial.printf("  parsed: %d track(s), division=%d ticks/quarter\n", _numTracks, _division);
  return true;
}

void SmfPlayer::end() {
  if (_file) _file.close();
  if (_numTracks > 0 && _midiStarted) _midi.allNotesOff();
  _numTracks = 0;
  _done   = true;
  _paused = false;
}

void SmfPlayer::pause() {
  if (_paused || _done) return;
  _paused   = true;
  _pausedAt = millis();
  _midi.allNotesOff();
}

void SmfPlayer::setVolume(uint8_t vol) {
  if (!_midiStarted) return;
  for (uint8_t ch = 0; ch < 16; ch++) _midi.cc(7, vol, ch);
}

void SmfPlayer::setNoteOnCallback(void (*cb)(uint8_t ch, uint8_t vel)) { _noteOnCb = cb; }
bool SmfPlayer::consumeBeatActivity() { bool v = _beatActivity; _beatActivity = false; return v; }

void SmfPlayer::resume() {
  if (!_paused) return;
  // Shift the wall-clock anchor forward by the pause duration so tick→ms math stays correct.
  _baseMillis += millis() - _pausedAt;
  _paused = false;
}

bool SmfPlayer::primeTrack(Track &t) {
  if (t.pos >= t.dataEnd) { t.ended = true; return false; }
  _file.seek(t.pos);
  bool ok;
  uint32_t delta = readVarLen(&ok);
  if (!ok) { t.ended = true; return false; }
  t.nextTick += delta;
  t.pos = _file.position();
  return true;
}

unsigned long SmfPlayer::msForTick(uint32_t tick) const {
  uint32_t deltaTicks = tick - _baseTick;
  // us = deltaTicks * usPerQuarter / division
  uint64_t us = (uint64_t)deltaTicks * _usPerQuarter / _division;
  return _baseMillis + (unsigned long)(us / 1000);
}

void SmfPlayer::processEvent(Track &t, uint32_t tick) {
  _file.seek(t.pos); // another track's read may have moved the shared cursor since t.pos was set

  // Read the byte unconditionally and treat it as the first data byte of a
  // running-status event if it doesn't have the high bit set.
  uint8_t b = _file.read();
  uint8_t status;
  uint8_t firstDataByte = 0;
  bool haveFirstData = false;
  if (b & 0x80) {
    status = b;
    if (status < 0xF0) t.runningStatus = status; // only channel messages set running status
  } else {
    status = t.runningStatus;
    firstDataByte = b;
    haveFirstData = true;
  }

  if (status == 0xFF) {
    uint8_t type = _file.read();
    bool ok;
    uint32_t len = readVarLen(&ok);
    if (type == 0x51 && len == 3) {
      uint8_t b0 = _file.read(), b1 = _file.read(), b2 = _file.read();
      _usPerQuarter = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
      _baseTick = tick;
      _baseMillis = msForTick(tick);
    } else if (type == 0x2F) {
      t.ended = true;
    } else {
      _file.seek(_file.position() + len);
    }
  } else if (status == 0xF0 || status == 0xF7) {
    bool ok;
    uint32_t len = readVarLen(&ok);
    _file.seek(_file.position() + len); // skip sysex, not needed for audio playback
  } else {
    uint8_t hi = status & 0xF0;
    uint8_t d1 = haveFirstData ? firstDataByte : _file.read();
    if (hi == 0xC0 || hi == 0xD0) {
      // program change / channel aftertouch: 1 data byte
      if (hi == 0xC0) _midi.program(d1, status & 0x0F);
      else _midi.raw1(status, d1);
    } else {
      uint8_t d2 = _file.read();
      switch (hi) {
        case 0x80: _midi.noteOff(d1, status & 0x0F); break;
        case 0x90:
          if (d2 == 0) {
            _midi.noteOff(d1, status & 0x0F);
          } else if (d2 < THIN_VELOCITY_CUTOFF && _midi.backlogFraction() > THIN_BACKLOG_THRESHOLD) {
            // Falling behind and this note is quiet -- drop it to shed load
            // rather than queue it and let the backlog snowball. Its file
            // position still advances normally (primeTrack below), so the
            // corresponding note-off later is harmless even though it never
            // sounded.
          } else {
            _midi.noteOn(d1, d2, status & 0x0F);
            if (_noteOnCb) _noteOnCb(status & 0x0F, d2);
          }
          break;
        case 0xA0: /* poly aftertouch, ignore */ break;
        case 0xB0: _midi.cc(d1, d2, status & 0x0F); break;
        case 0xE0: _midi.pitchBend(((int16_t)(((uint16_t)d2 << 7) | d1)) - 8192, status & 0x0F); break;
        default: break;
      }
    }
  }

  t.pos = _file.position();
  if (!t.ended) primeTrack(t);
}

bool SmfPlayer::update() {
  if (_done)   return false;
  if (_paused) return true;   // alive but suspended

  while (true) {
    int minIdx = -1;
    uint32_t minTick = 0;
    for (uint8_t i = 0; i < _numTracks; i++) {
      if (_tracks[i].ended) continue;
      if (minIdx == -1 || _tracks[i].nextTick < minTick) {
        minIdx = i;
        minTick = _tracks[i].nextTick;
      }
    }
    if (minIdx == -1) { _done = true; end(); return false; }

    if ((long)(millis() - msForTick(minTick)) < 0) break; // not due yet

    uint32_t beat = minTick / _division;
    if (beat > _lastBeatCount) { _lastBeatCount = beat; _beatActivity = true; }

    processEvent(_tracks[minIdx], minTick);
  }
  return true;
}
