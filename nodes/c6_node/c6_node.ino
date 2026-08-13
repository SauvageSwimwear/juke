// Thin raw-MIDI-over-UART wrapper, same shape/semantics as the Synth class in
// ~/Downloads/midi.py (the proven MicroPython version) -- UART1, pin 17, 31250 baud,
// raw status+data bytes, no external MIDI library needed.
#pragma once
#include <Arduino.h>

class MidiOut {
public:
  void begin(int txPin = 17, unsigned long baud = 31250) {
    // Bigger TX buffer than the ESP32 core default (256B) so dense/simultaneous
    // passages (many notes due on the same tick) get queued instead of forcing
    // Serial1.write() to block -- blocking mid-burst eats real wall-clock time,
    // which lets other tracks' events fall further overdue too, snowballing
    // into the "scramble" sound. Must be called before begin(). 2048B is
    // ~655ms of buffering at 31250 baud -- generous headroom, still small RAM.
    _serial.setTxBufferSize(2048);
    _serial.begin(baud, SERIAL_8N1, -1, txPin);
  }

  // Fraction of the TX buffer currently in use, 0.0 (empty) - 1.0 (full).
  // Used to detect "we're falling behind" before it snowballs into a scramble.
  float backlogFraction() {
    return 1.0f - (float)_serial.availableForWrite() / (float)_txBufSize;
  }

  void noteOn(uint8_t note, uint8_t vel = 100, uint8_t ch = 0) {
    uint8_t msg[3] = { (uint8_t)(0x90 | (ch & 0x0F)), (uint8_t)(note & 0x7F), (uint8_t)(vel & 0x7F) };
    _serial.write(msg, 3);
  }

  void noteOff(uint8_t note, uint8_t ch = 0) {
    uint8_t msg[3] = { (uint8_t)(0x80 | (ch & 0x0F)), (uint8_t)(note & 0x7F), 0 };
    _serial.write(msg, 3);
  }

  void program(uint8_t p, uint8_t ch = 0) {
    uint8_t msg[2] = { (uint8_t)(0xC0 | (ch & 0x0F)), (uint8_t)(p & 0x7F) };
    _serial.write(msg, 2);
  }

  void cc(uint8_t controller, uint8_t value, uint8_t ch = 0) {
    uint8_t msg[3] = { (uint8_t)(0xB0 | (ch & 0x0F)), (uint8_t)(controller & 0x7F), (uint8_t)(value & 0x7F) };
    _serial.write(msg, 3);
  }

  void pitchBend(int16_t val, uint8_t ch = 0) {
    uint16_t v = (uint16_t)(val + 8192);
    uint8_t msg[3] = { (uint8_t)(0xE0 | (ch & 0x0F)), (uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F) };
    _serial.write(msg, 3);
  }

  // For channel-aftertouch (0xDn) or any other 2-byte (status+1) message.
  void raw1(uint8_t status, uint8_t data1) {
    uint8_t msg[2] = { status, (uint8_t)(data1 & 0x7F) };
    _serial.write(msg, 2);
  }

  void allNotesOff() {
    for (uint8_t ch = 0; ch < 16; ch++) cc(123, 0, ch);
  }

private:
  HardwareSerial _serial{1};  // UART1
  static const uint16_t _txBufSize = 2048;  // must match setTxBufferSize() above
};
