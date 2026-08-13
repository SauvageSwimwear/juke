// tower_light.ino — DT60 tower light node for the jukebox ESP-NOW mesh
//
// Hardware: Seeed Xiao ESP32-C6
//   GPIO2 → ULN2003A IN1 → red    segment cathode
//   GPIO3 → ULN2003A IN2 → yellow segment cathode
//   GPIO4 → ULN2003A IN3 → green  segment cathode
//   GPIO5 → buzzer (parked LOW — role TBD)
//   Tower common (black wire) = 12V PSU anode via illuminated amber toggle
//
// Ethos: this node listens only. The conductor is not touched.
//   All behavior derives exclusively from ESP-NOW NOTE_ON packets.
//
// Behavior:
//   • Tracks which MIDI channels fired a NOTE_ON in the last ACTIVE_MS (250ms)
//   • Active channel count drives color: 1–3 = red, 4–10 = green, 11+ = yellow
//   • Brightness breathes with velocity — decays slowly when channels go quiet
//   • Max brightness capped at half (127) — tweak MAX_DUTY to taste
//   • Yellow: full-on only — internal oscillator fights PWM dimming (YELLOW_PWM_SAFE)
//   • Boot / silence default: red dim
//
// Requires: arduino-esp32 3.x (new LEDC API: ledcAttach / ledcWrite by pin)
//   If on 2.x: ledcSetup(ch,freq,bits) + ledcAttachPin(pin,ch) + ledcWrite(ch,duty)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── pin config ────────────────────────────────────────────────────────────────
#define RED_PIN        2
#define YEL_PIN        3
#define GRN_PIN        4
#define BUZ_PIN        5

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1
#define MSG_NOTE_ON    0x01

// ── PWM ───────────────────────────────────────────────────────────────────────
#define LEDC_FREQ      1000
#define LEDC_BITS      8

// ── tuning ────────────────────────────────────────────────────────────────────
#define ACTIVE_MS      250      // channel counts as active if it fired within this window
#define BUMP_BASE      30.0f   // brightness bump from a quiet note
#define BUMP_VEL       120.0f  // extra bump at full velocity
#define DECAY          0.994f  // per frame at 30fps → ~4s half-life
#define FLOOR_DUTY     3       // always dimly on — tower never fully dark
#define MAX_DUTY       127     // half brightness cap — raise to 255 for full
#define FRAME_MS       33      // ~30fps

// ── channel thresholds ────────────────────────────────────────────────────────
#define THRESH_GREEN   4       // 4+ active channels → green
#define THRESH_YELLOW  11      // 11+ active channels → yellow

// Yellow PWM: true = always full-on when active (safe); false = PWM like others
#define YELLOW_PWM_SAFE true

// ── state ─────────────────────────────────────────────────────────────────────
static uint32_t lastFired[16] = {0};  // timestamp of last NOTE_ON per channel
static float    brightness    = 40.0f;

// ── ISR-safe event queue (channel + velocity) ─────────────────────────────────
struct NoteEv { uint8_t ch, vel; };
#define EV_BUF 32
static volatile NoteEv  evBuf[EV_BUF];
static volatile uint8_t evHead = 0, evTail = 0;
static portMUX_TYPE     evMux  = portMUX_INITIALIZER_UNLOCKED;

// ── ESP-NOW receive ───────────────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;
    if ((data[0]^data[1]^data[2]^data[3]^data[4]) != data[5]) return;
    if (data[0] != MSG_NOTE_ON) return;
    if (data[3] == 0) return;

    taskENTER_CRITICAL(&evMux);
    uint8_t next = (evHead + 1) % EV_BUF;
    if (next != evTail) {
        evBuf[evHead].ch  = data[1] & 0x0F;
        evBuf[evHead].vel = data[3];
        evHead = next;
    }
    taskEXIT_CRITICAL(&evMux);
}

// ── write duty to one pin, zero the others ───────────────────────────────────
static void setColor(uint8_t pin, uint8_t duty) {
    uint8_t rd = (pin == RED_PIN) ? duty : 0;
    uint8_t yd = (pin == YEL_PIN) ? (YELLOW_PWM_SAFE ? (duty > 0 ? 255 : 0) : duty) : 0;
    uint8_t gd = (pin == GRN_PIN) ? duty : 0;
    ledcWrite(RED_PIN, rd);
    ledcWrite(YEL_PIN, yd);
    ledcWrite(GRN_PIN, gd);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(BUZ_PIN, OUTPUT);
    digitalWrite(BUZ_PIN, LOW);

    ledcAttach(RED_PIN, LEDC_FREQ, LEDC_BITS);
    ledcAttach(YEL_PIN, LEDC_FREQ, LEDC_BITS);
    ledcAttach(GRN_PIN, LEDC_FREQ, LEDC_BITS);

    setColor(RED_PIN, (uint8_t)brightness);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }
    esp_now_register_recv_cb(onRecv);

    Serial.printf("tower_light ready  channel=%d\n", ESPNOW_CHANNEL);
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastFrame = 0;
    uint32_t now = millis();
    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    // ── drain event queue ─────────────────────────────────────────────────────
    while (true) {
        taskENTER_CRITICAL(&evMux);
        bool   empty = (evHead == evTail);
        NoteEv e     = {};
        if (!empty) { e.ch = evBuf[evTail].ch; e.vel = evBuf[evTail].vel; evTail = (evTail + 1) % EV_BUF; }
        taskEXIT_CRITICAL(&evMux);
        if (empty) break;

        lastFired[e.ch] = now;
        brightness = min((float)MAX_DUTY, brightness + BUMP_BASE + BUMP_VEL * (e.vel / 127.0f));
    }

    // ── count active channels in the last ACTIVE_MS ───────────────────────────
    uint8_t active = 0;
    for (uint8_t i = 0; i < 16; i++)
        if (lastFired[i] && (now - lastFired[i]) < ACTIVE_MS) active++;

    // ── pick color from channel count ─────────────────────────────────────────
    uint8_t pin;
    if      (active >= THRESH_YELLOW) pin = YEL_PIN;
    else if (active >= THRESH_GREEN)  pin = GRN_PIN;
    else                              pin = RED_PIN;

    // ── decay brightness ──────────────────────────────────────────────────────
    brightness *= DECAY;

    // ── write ─────────────────────────────────────────────────────────────────
    uint8_t duty = (uint8_t)brightness;
    if (duty < FLOOR_DUTY) duty = FLOOR_DUTY;
    setColor(pin, duty);

    // ── debug (comment out after tuning) ─────────────────────────────────────
    static uint32_t lastLog = 0;
    if (now - lastLog > 1000) {
        lastLog = now;
        Serial.printf("active=%d  color=%s  duty=%d\n",
            active,
            pin == RED_PIN ? "red" : pin == GRN_PIN ? "green" : "yellow",
            duty);
    }
}
