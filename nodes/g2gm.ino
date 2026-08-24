// gm_node.ino — Coffee: GM synthesizer performer node
// Receives 6-byte XOR-checksummed ESP-NOW packets from Elfer.
// Forwards to GM chip via MIDI UART (TX only, 31250 baud).
//
// WATCHDOG: if no packet arrives for WATCHDOG_MS, fires all-notes-off.
// Prevents hanging notes on Pi pause / shutdown / crash.
//
// STATUS: serial heartbeat every HEARTBEAT_MS.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── config ────────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL   1
#define MIDI_TX_PIN      17       // → GM chip MIDI RX. Verify vs your wiring.
#define MIDI_BAUD        31250
#define WATCHDOG_MS      1500     // silence threshold → all-notes-off.
                                  // Harmless during a musical rest (no notes
                                  // are playing), but will clip a sustained
                                  // chord held longer than this. Tune upward
                                  // if you have pieces with very long holds.
#define HEARTBEAT_MS     30000
#define VERBOSE_PACKETS  0        // 1 = log every packet. Noisy during playback.

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
static volatile unsigned long lastPacketTime = 0;
static volatile bool          watchdogArmed  = false;  // true after first packet received
static uint32_t               packetCount    = 0;
static uint32_t               dropCount      = 0;
static uint32_t               watchdogFires  = 0;

// ── MIDI helpers ──────────────────────────────────────────────────────────────
static void midi3(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t m[3] = {a, b, c};
    Serial1.write(m, 3);
}
static void midi2(uint8_t a, uint8_t b) {
    uint8_t m[2] = {a, b};
    Serial1.write(m, 2);
}

// Sends CC 123 (all notes off) + CC 121 (reset controllers) on all 16 channels.
static void allNotesOff() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midi3(0xB0 | ch, 123, 0);   // CC 123: all notes off
        midi3(0xB0 | ch, 121, 0);   // CC 121: reset all controllers
    }
}

// ── SAM2695 DSP init ──────────────────────────────────────────────────────────
// Global DSP settings — reverb, EQ. The conductor never sends these so they
// survive the full performance without being overwritten by MIDI file data.
//
// Tuning:
//   REV_TYPE  0x05 = Plate (tighter than default Hall2 0x04)
//             Other options: 00h Room1, 01h Room2, 02h Room3, 03h Hall1,
//                            06h Delay, 07h Pan delay
//   REV_SEND  0x28 = drier than default (0x40). Raise toward 0x40 for more room.
//   EQ        bass + treble flattened from default +6dB (0x60) to 0dB (0x40).
//             Mid bands are already 0dB by default.

static void samNRPN(uint8_t hi, uint8_t lo, uint8_t val) {
    midi3(0xB0, 0x63, hi);   // CC99 NRPN high
    midi3(0xB0, 0x62, lo);   // CC98 NRPN low
    midi3(0xB0, 0x06, val);  // CC6  data entry
}

static void samInit() {
    // Reverb type: Plate instead of default Hall2
    midi3(0xB0, 0x50, 0x05);

    // Reverb send level: drier than default
    samNRPN(0x37, 0x15, 0x28);

    // EQ: flatten bass and treble to 0dB
    samNRPN(0x37, 0x00, 0x40);   // bass
    samNRPN(0x37, 0x03, 0x40);   // treble
}

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

    Serial1.begin(MIDI_BAUD, SERIAL_8N1, -1, MIDI_TX_PIN);
    delay(600);   // SAM2695 needs 500ms minimum after power-on before accepting MIDI
    allNotesOff();
    samInit();
    Serial.println("[coffee] boot all-notes-off + SAM init sent");

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
                if (e.d2 == 0) midi3(0x80 | e.ch, e.d1, 0);
                else           midi3(0x90 | e.ch, e.d1, e.d2);
                break;
            case MSG_NOTE_OFF: midi3(0x80 | e.ch, e.d1, e.d2); break;
            case MSG_PROGRAM:  midi2(0xC0 | e.ch, e.d1);       break;
            case MSG_CC:       midi3(0xB0 | e.ch, e.d1, e.d2); break;
            case MSG_CONFIG:   /* reserved */                    break;
        }
    }

    // ── watchdog: silence → all-notes-off ─────────────────────────────────────
    static bool watchdogHasFired = false;
    if (watchdogArmed && (now - lastPacketTime > WATCHDOG_MS)) {
        if (!watchdogHasFired) {
            allNotesOff();
            watchdogFires++;
            watchdogHasFired = true;
            Serial.printf("[coffee] watchdog fired  (fire #%u  silence=%lums)\n",
                          watchdogFires, now - lastPacketTime);
        }
    } else {
        watchdogHasFired = false;
    }

    // ── heartbeat ─────────────────────────────────────────────────────────────
    static unsigned long nextHB = HEARTBEAT_MS;
    if (now >= nextHB) {
        nextHB += HEARTBEAT_MS;
        Serial.printf("[coffee] up=%lus  pkts=%u  drops=%u  watchdog_fires=%u\n",
                      now / 1000, packetCount, dropCount, watchdogFires);
    }
}
