// gm_node.ino — Coffee: GM synthesizer performer node
// Receives 6-byte XOR-checksummed ESP-NOW packets from Elfer.
// Forwards to GM chip via MidiOut (UART1, pin 17, 31250 baud).
//
// WATCHDOG: if no packet arrives for WATCHDOG_MS, fires all-notes-off.
// Prevents hanging notes on Pi pause / shutdown / crash.
//
// Requires MidiOut.h in the same sketch folder.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "MidiOut.h"

// ── config ────────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL  1
#define MIDI_TX_PIN     17        // → GM chip MIDI RX. Verify vs your wiring.
#define WATCHDOG_MS     1500      // silence threshold → all-notes-off.
                                  // Harmless during a musical rest. Increase if
                                  // your pieces have held notes longer than this.
#define HEARTBEAT_MS    30000
#define VERBOSE_PACKETS 0         // 1 = log every packet. Noisy during playback.

// ── packet types (must match conductor/packet.py) ─────────────────────────────
#define MSG_NOTE_ON   0x01
#define MSG_NOTE_OFF  0x02
#define MSG_PROGRAM   0x03
#define MSG_CC        0x04
#define MSG_CONFIG    0x05

// ── event queue (ESP-NOW callback → loop, ISR-safe ring buffer) ───────────────
struct MidiEvent { uint8_t type, ch, d1, d2; };
#define EVENT_BUF 32
static MidiEvent        eventBuf[EVENT_BUF];
static volatile uint8_t evHead = 0, evTail = 0;
static portMUX_TYPE     evMux  = portMUX_INITIALIZER_UNLOCKED;

// ── state ─────────────────────────────────────────────────────────────────────
static MidiOut                _midi;
static volatile unsigned long lastPacketTime = 0;
static volatile bool          watchdogArmed  = false;
static uint32_t               packetCount    = 0;
static uint32_t               dropCount      = 0;
static uint32_t               watchdogFires  = 0;

// ── ESP-NOW receive callback ───────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;
    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) { dropCount++; return; }

    lastPacketTime = millis();
    watchdogArmed  = true;
    packetCount++;

    MidiEvent e = { data[0], (uint8_t)(data[1] & 0x0F), data[2], data[3] };
    taskENTER_CRITICAL(&evMux);
    uint8_t next = (evHead + 1) % EVENT_BUF;
    if (next != evTail) { eventBuf[evHead] = e; evHead = next; }
    else dropCount++;
    taskEXIT_CRITICAL(&evMux);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("[coffee] booting  midi_tx=%d  channel=%d  watchdog=%dms\n",
                  MIDI_TX_PIN, ESPNOW_CHANNEL, WATCHDOG_MS);

    _midi.begin(MIDI_TX_PIN);
    delay(150);             // give GM chip a moment after power-on
    _midi.allNotesOff();    // clear any hanging notes from previous session
    Serial.println("[coffee] boot all-notes-off sent");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[coffee] ESP-NOW init FAILED — halting");
        while (true) delay(1000);
    }
    esp_now_register_recv_cb(onRecv);

    lastPacketTime = millis();
    Serial.println("[coffee] ready");
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── drain event queue → MIDI out ──────────────────────────────────────────
    while (true) {
        taskENTER_CRITICAL(&evMux);
        bool      empty = (evHead == evTail);
        MidiEvent e     = {};
        if (!empty) { e = eventBuf[evTail]; evTail = (evTail + 1) % EVENT_BUF; }
        taskEXIT_CRITICAL(&evMux);
        if (empty) break;

#if VERBOSE_PACKETS
        Serial.printf("[coffee] rx 0x%02X ch=%d d1=%d d2=%d\n",
                      e.type, e.ch, e.d1, e.d2);
#endif
        switch (e.type) {
            case MSG_NOTE_ON:
                if (e.d2 == 0) _midi.noteOff(e.d1, e.ch);          // vel=0 = note off
                else           _midi.noteOn(e.d1, e.d2, e.ch);
                break;
            case MSG_NOTE_OFF: _midi.noteOff(e.d1, e.ch);           break;
            case MSG_PROGRAM:  _midi.program(e.d1, e.ch);           break;
            case MSG_CC:       _midi.cc(e.d1, e.d2, e.ch);         break;
            case MSG_CONFIG:   /* reserved */                         break;
        }
    }

    // ── watchdog: silence → all-notes-off ─────────────────────────────────────
    static bool watchdogHasFired = false;
    if (watchdogArmed && (now - lastPacketTime > WATCHDOG_MS)) {
        if (!watchdogHasFired) {
            _midi.allNotesOff();
            watchdogFires++;
            watchdogHasFired = true;
            Serial.printf("[coffee] watchdog fired (fire #%u  silence=%lums)\n",
                          watchdogFires, now - lastPacketTime);
        }
    } else {
        watchdogHasFired = false;   // reset when packets resume
    }

    // ── heartbeat ─────────────────────────────────────────────────────────────
    static unsigned long nextHB = HEARTBEAT_MS;
    if (now >= nextHB) {
        nextHB += HEARTBEAT_MS;
        Serial.printf("[coffee] up=%lus  pkts=%u  drops=%u  watchdog_fires=%u\n",
                      now / 1000, packetCount, dropCount, watchdogFires);
    }
}
