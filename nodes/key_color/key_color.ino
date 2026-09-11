// key_color_node.ino — Tonal center visualizer for the Juke ESP-NOW mesh
//
// Listens to ESP-NOW NOTE_ON packets, maintains a decaying pitch-class
// histogram, detects the musical key, and displays it as a color derived
// from the circle of fifths — simultaneously on two LED technologies:
//
//   GPIO25 → PL9823 addressable LED  (one data wire, FastLED)
//   GPIO26 → RGB LED red leg         (PWM)
//   GPIO27 → RGB LED green leg       (PWM)
//   GPIO32 → RGB LED blue leg        (PWM)
//
// Hardware: classic ESP32 dev kit (WROOM-32)
// Arduino core: esp32 by Espressif 2.x (classic / "old" API)
// Requires: FastLED library

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <FastLED.h>

// ── config ────────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL  1

#define PL_PIN          25      // PL9823 data line
#define RGB_R_PIN       26
#define RGB_G_PIN       27
#define RGB_B_PIN       32

#define FRAME_MS        30
#define DECAY           0.994f
#define HYSTERESIS      1.30f
#define LED_BRIGHTNESS  200
#define LED_SATURATION  255

// ── packet constants (must match conductor/packet.py) ─────────────────────────
#define MSG_NOTE_ON     0x01
#define MSG_NOTE_OFF    0x02
#define MSG_PROGRAM     0x03
#define MSG_CC          0x04
#define MSG_CONFIG      0x05

// ── circle of fifths: chromatic pitch class → CoF position ───────────────────
//   C   C#  D   D#  E   F   F#  G   G#  A   Bb  B
static const uint8_t cofPos[12] = { 0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5 };
// hue = cofPos[pitchClass] * 21   (256/12 ≈ 21)

// ── key detection state ───────────────────────────────────────────────────────
static float   weight[12] = {0};
static uint8_t currentKey = 0;

// ── FastLED ───────────────────────────────────────────────────────────────────
#define NUM_LEDS 1
CRGB plLed[NUM_LEDS];

// ── PWM config (Core v3: pin-based, no explicit channel numbers) ──────────────
#define PWM_FREQ      5000
#define PWM_RES       8        // 8-bit = 0–255

// ── event queue (ISR-safe) ────────────────────────────────────────────────────
struct NoteEvent { uint8_t pitchClass; float velocityNorm; };

#define EVENT_BUF 32
static NoteEvent        eventBuf[EVENT_BUF];
static volatile uint8_t evHead = 0, evTail = 0;
static portMUX_TYPE     evMux  = portMUX_INITIALIZER_UNLOCKED;

// ── ESP-NOW receive callback (Core v3 signature) ──────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;

    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) return;

    if (data[0] == MSG_CONFIG) {
        uint8_t pc = data[1] & 0x0F;
        taskENTER_CRITICAL(&evMux);
        weight[pc] += 3.0f;
        taskEXIT_CRITICAL(&evMux);
        Serial.printf("CONFIG: key hint pitch class %d seeded\n", pc);
        return;
    }

    if (data[0] != MSG_NOTE_ON) return;
    if (data[3] == 0)           return;   // velocity 0 = note off

    NoteEvent e = {
        (uint8_t)(data[2] % 12),
        data[3] / 127.0f
    };

    taskENTER_CRITICAL(&evMux);
    uint8_t next = (evHead + 1) % EVENT_BUF;
    if (next != evTail) { eventBuf[evHead] = e; evHead = next; }
    taskEXIT_CRITICAL(&evMux);
}

// ── write color to both LEDs ──────────────────────────────────────────────────
static void showColor(uint8_t hue) {
    plLed[0] = CHSV(hue, LED_SATURATION, LED_BRIGHTNESS);
    FastLED.show();

    CRGB rgb;
    hsv2rgb_rainbow(CHSV(hue, LED_SATURATION, LED_BRIGHTNESS), rgb);
    ledcWrite(RGB_R_PIN, rgb.r);
    ledcWrite(RGB_G_PIN, rgb.g);
    ledcWrite(RGB_B_PIN, rgb.b);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);

    // PL9823
    FastLED.addLeds<PL9823, PL_PIN, RGB>(plLed, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS);
    plLed[0] = CRGB::Black;
    FastLED.show();

    // RGB LED PWM — Core v3: pin-based API
    ledcAttach(RGB_R_PIN, PWM_FREQ, PWM_RES);
    ledcAttach(RGB_G_PIN, PWM_FREQ, PWM_RES);
    ledcAttach(RGB_B_PIN, PWM_FREQ, PWM_RES);
    ledcWrite(RGB_R_PIN, 0);
    ledcWrite(RGB_G_PIN, 0);
    ledcWrite(RGB_B_PIN, 0);

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }
    esp_now_register_recv_cb(onRecv);

    Serial.println("key_color_node ready");
    Serial.println("  GPIO25 = PL9823");
    Serial.println("  GPIO26/27/32 = RGB R/G/B");

    showColor(cofPos[0] * 21);  // boot color: C = red
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastFrame = 0;
    uint32_t now = millis();
    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    // 1. drain event queue into histogram
    while (true) {
        taskENTER_CRITICAL(&evMux);
        bool      empty = (evHead == evTail);
        NoteEvent e     = {};
        if (!empty) { e = eventBuf[evTail]; evTail = (evTail + 1) % EVENT_BUF; }
        taskEXIT_CRITICAL(&evMux);
        if (empty) break;

        weight[e.pitchClass] += e.velocityNorm;
    }

    // 2. decay all bins
    for (int i = 0; i < 12; i++)
        weight[i] *= DECAY;

    // 3. find heaviest bin
    uint8_t leader = 0;
    for (int i = 1; i < 12; i++)
        if (weight[i] > weight[leader]) leader = i;

    // 4. hysteresis: only switch if leader is significantly heavier
    if (weight[leader] > weight[currentKey] * HYSTERESIS) {
        if (leader != currentKey) {
            currentKey = leader;
            Serial.printf("key -> pitch class %d  hue=%d\n",
                          currentKey, cofPos[currentKey] * 21);
        }
    }

    // 5. display
    showColor(cofPos[currentKey] * 21);
}
