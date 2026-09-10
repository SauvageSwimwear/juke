// baton.ino — ESP32-S3
// 28BYJ-48 stepper + ULN2003 driver, sweeping back and forth as a physical
// tempo-adjacent gesture. Doesn't know BPM — advances one increment per
// qualifying note-on, bounces at travel limits, same shape as
// elfer_choir_pwm.ino's chase-light logic, just driving a motor instead
// of LEDs. No touch on Bridge or Conductor — reads the same broadcast
// every other node already gets.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <AccelStepper.h>

// ---- mesh config (must match bridge / every other node) ----
#define ESPNOW_CHANNEL 1

// ---- packet format (must match conductor/packet.py) ----
#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02
#define MSG_PROGRAM  0x03
#define MSG_CC       0x04
#define MSG_CONFIG   0x05

// ---- stepper pins (ULN2003 IN1–IN4) ----
// Deliberately avoiding boot-strapping pins (0, 3, 45, 46) and native-USB
// pins (19, 20) on the S3.
#define STEPPER_IN1 4
#define STEPPER_IN2 5
#define STEPPER_IN3 6
#define STEPPER_IN4 7

// AccelStepper HALF4WIRE wants pins in (IN1, IN3, IN2, IN4) order — a
// known gotcha with the 28BYJ-48's coil pairing, not a typo.
AccelStepper stepper(AccelStepper::HALF4WIRE, STEPPER_IN1, STEPPER_IN3, STEPPER_IN2, STEPPER_IN4);

// ---- trigger tuning (tune these to taste, same spirit as elfer's knobs) ----
#define TRIGGER_CHANNEL  -1    // -1 = all channels, 0-15 = one channel only
#define NOTES_PER_STEP    3    // lower = twitchier, higher = calmer

// ---- sweep geometry ----
// 28BYJ-48 w/ HALF4WIRE: 4096 half-steps per output-shaft revolution.
// Quarter-turn arc feels like a baton gesture without needing a full spin.
#define SWEEP_MIN       0
#define SWEEP_MAX       1024
#define STEPS_PER_ADVANCE 24   // how far one qualifying note nudges the baton

#define STEPPER_MAX_SPEED  400   // steps/sec — keep well under what the
                                  // gearbox can actually deliver (~15 RPM)
#define STEPPER_ACCEL      200

// ---- incoming note queue (filled by ESP-NOW recv callback, drained in loop) ----
struct NoteEvent { uint8_t channel; uint8_t velocity; };
#define EVENT_BUF 32
static NoteEvent          eventBuf[EVENT_BUF];
static volatile uint8_t   evHead = 0, evTail = 0;
static portMUX_TYPE       evMux = portMUX_INITIALIZER_UNLOCKED;

// ---- sweep state ----
static uint32_t noteCount = 0;
static int8_t   sweepDir  = 1;

// ── ESP-NOW recv callback: deliberately minimal, no heavy work here ────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;

    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) return;   // bad packet, drop it

    if (data[0] != MSG_NOTE_ON) return;
    uint8_t channel  = data[1];
    uint8_t velocity = data[3];
    if (velocity == 0) return;    // note_on vel=0 is really a note_off

    if (TRIGGER_CHANNEL >= 0 && channel != TRIGGER_CHANNEL) return;

    uint8_t next = (evHead + 1) % EVENT_BUF;
    if (next == evTail) return;   // buffer full, drop — better than blocking

    portENTER_CRITICAL(&evMux);
    eventBuf[evHead] = { channel, velocity };
    evHead = next;
    portEXIT_CRITICAL(&evMux);
}

// ── setup ────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    stepper.setMaxSpeed(STEPPER_MAX_SPEED);
    stepper.setAcceleration(STEPPER_ACCEL);
    stepper.setCurrentPosition(SWEEP_MIN);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onRecv);

    Serial.printf("Baton ready  channel=%d  trigger_ch=%d  notes_per_step=%d\n",
                  ESPNOW_CHANNEL, TRIGGER_CHANNEL, NOTES_PER_STEP);
}

// ── loop ─────────────────────────────────────────────────────────────────
void loop() {
    // drain the event queue
    while (evTail != evHead) {
        portENTER_CRITICAL(&evMux);
        NoteEvent ev = eventBuf[evTail];
        evTail = (evTail + 1) % EVENT_BUF;
        portEXIT_CRITICAL(&evMux);

        noteCount++;
        if (noteCount >= NOTES_PER_STEP) {
            noteCount = 0;

            long target = stepper.targetPosition() + sweepDir * STEPS_PER_ADVANCE;

            if (target >= SWEEP_MAX) {
                target = SWEEP_MAX;
                sweepDir = -1;
            } else if (target <= SWEEP_MIN) {
                target = SWEEP_MIN;
                sweepDir = 1;
            }

            stepper.moveTo(target);
        }
    }

    stepper.run();   // non-blocking, call every loop iteration
}
