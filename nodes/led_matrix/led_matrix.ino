// led_matrix.ino — WS2812B 8×8 "Channel Towers" visualizer for the jukebox ESP-NOW mesh
//
// One calm, always-on mode (no touch/mode select):
//   • Each MIDI channel that plays claims its own column (first-come, up to 8).
//   • That column's bar rises with the channel's activity and drains when it rests.
//   • When a bar fills to the top it ROLLS OVER to the next cool shade and keeps going
//     ("counts up, then changes color").
//   • Mono cool palette (cyan→violet); each column starts on a distinct shade so the
//     voices are distinguishable.
//
// Requires: FastLED library
// Hardware: ESP32-S2 (Lolin S2 Mini) + WS2812B 8×8 panel (serpentine wiring)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <FastLED.h>

// ── config (twist these to taste) ────────────────────────────────────────────
#define DATA_PIN       5        // WS2812B data line GPIO
#define BRIGHTNESS     110      // global brightness cap (0–255); deep blues read dim, so a touch higher
#define ESPNOW_CHANNEL 1        // must match bridge and all other nodes

#define COOL_LO    128          // cyan   — low end of the cool hue band
#define COOL_HI    200          // violet — high end of the cool hue band
#define HUE_STEP     9          // hue advance each time a tower rolls over
#define BUMP_BASE  0.35f        // level added per note (floor)
#define BUMP_VEL   1.10f        // extra level scaled by velocity (0..1)
#define DRAIN      0.055f       // level lost per frame (~1.8 rows/sec @ ~33 fps)
#define TOWER_SAT  200          // cool saturation (0–255; lower = washed out)
#define TOWER_VAL  200          // brightness of solid tower rows (0–255)

// ── matrix ───────────────────────────────────────────────────────────────────
#define MATRIX_W  8
#define MATRIX_H  8
#define NUM_LEDS  (MATRIX_W * MATRIX_H)

CRGB leds[NUM_LEDS];

// Serpentine panel: odd rows run right-to-left.
// If your panel wires all rows the same direction, remove the y&1 branch.
static inline uint8_t xy(uint8_t x, uint8_t y) {
    if (y & 1) x = (MATRIX_W - 1) - x;
    return y * MATRIX_W + x;
}

// ── packet constants (must match conductor/packet.py) ─────────────────────────
#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02
#define MSG_PROGRAM  0x03
#define MSG_CC       0x04
#define MSG_CONFIG   0x05

// ── channel towers state ──────────────────────────────────────────────────────
static int8_t  colOfChannel[16];    // channel -> column, -1 = unassigned
static uint8_t nextCol = 0;         // next free column (0..MATRIX_W-1)
static float   level[MATRIX_W];     // current fill height, 0..MATRIX_H
static uint8_t stage[MATRIX_W];     // color-rollover counter
static uint8_t baseHue[MATRIX_W];   // distinct cool base hue per column

// ── event queue ───────────────────────────────────────────────────────────────
struct NoteEvent { uint8_t channel, note, velocity; };

#define EVENT_BUF 32
static NoteEvent         eventBuf[EVENT_BUF];
static volatile uint8_t  evHead = 0, evTail = 0;
static portMUX_TYPE      evMux  = portMUX_INITIALIZER_UNLOCKED;

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;
    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) return;
    if (data[0] != MSG_NOTE_ON) return;
    if (data[3] == 0)           return;

    NoteEvent e = { (uint8_t)(data[1] & 0x0F), data[2], data[3] };
    taskENTER_CRITICAL(&evMux);
    uint8_t next = (evHead + 1) % EVENT_BUF;
    if (next != evTail) { eventBuf[evHead] = e; evHead = next; }
    taskEXIT_CRITICAL(&evMux);
}

// ── main ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();

    // tower state: no channels mapped yet, all bars empty, distinct cool base hues
    for (int c = 0; c < 16; c++) colOfChannel[c] = -1;
    for (int x = 0; x < MATRIX_W; x++) {
        level[x]   = 0.0f;
        stage[x]   = 0;
        baseHue[x] = COOL_LO + (uint16_t)(COOL_HI - COOL_LO) * x / (MATRIX_W - 1);
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }
    esp_now_register_recv_cb(onRecv);

    Serial.printf("led_matrix ready  channel=%d  pin=%d  mode=TOWERS\n",
                  ESPNOW_CHANNEL, DATA_PIN);
}

void loop() {
    static uint32_t lastFrame = 0;
    uint32_t now = millis();

    // ── frame limiter (~33 fps) ────────────────────────────────────────────────
    if (now - lastFrame < 30) return;
    lastFrame = now;

    // ── drain event queue → raise the matching tower ───────────────────────────
    while (true) {
        taskENTER_CRITICAL(&evMux);
        bool      empty = (evHead == evTail);
        NoteEvent e     = {};
        if (!empty) { e = eventBuf[evTail]; evTail = (evTail + 1) % EVENT_BUF; }
        taskEXIT_CRITICAL(&evMux);
        if (empty) break;

        int8_t col = colOfChannel[e.channel];
        if (col < 0) {
            if (nextCol >= MATRIX_W) continue;          // all columns taken; ignore extra voices
            col = colOfChannel[e.channel] = nextCol++;  // first-come column assignment
        }
        level[col] += BUMP_BASE + BUMP_VEL * (e.velocity / 127.0f);
        if (level[col] >= MATRIX_H) {                   // filled to the top…
            level[col] -= MATRIX_H;                      // …carry the remainder…
            stage[col]++;                                // …and roll to the next cool shade
        }
    }

    // ── drain every tower a little each frame ──────────────────────────────────
    for (int x = 0; x < MATRIX_W; x++)
        level[x] = max(0.0f, level[x] - DRAIN);

    // ── render towers, filling from the bottom, with a soft fractional crest ───
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    uint8_t range = COOL_HI - COOL_LO;
    for (int x = 0; x < MATRIX_W; x++) {
        if (level[x] <= 0.0f) continue;
        uint8_t hue  = COOL_LO + ((baseHue[x] - COOL_LO + stage[x] * HUE_STEP) % range);
        int     full = (int)floorf(level[x]);
        for (int r = 0; r < full && r < MATRIX_H; r++) {           // solid rows
            uint8_t y = MATRIX_H - 1 - r;                          // fill from the bottom up
            leds[xy(x, y)] = CHSV(hue, TOWER_SAT, TOWER_VAL);
        }
        float frac = level[x] - full;                              // soft crest pixel
        if (full < MATRIX_H && frac > 0.02f)
            leds[xy(x, MATRIX_H - 1 - full)] = CHSV(hue, TOWER_SAT, (uint8_t)(frac * TOWER_VAL));
    }

    FastLED.show();
}
