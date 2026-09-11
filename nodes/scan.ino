// ── ESP32-S3 N16R8 — ESP-NOW MIDI LED Receiver ───────────────────────────────
// Receives 6-byte packets from the Pi→ESP-NOW bridge.
// Packet layout  [msg_type | ch | note | vel | pad | xor_chk]
// msg_type 0x01 = MIDI note on/off
// MIDI channels 1-8 → GPIO PWM LEDs with velocity brightness + soft decay.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/ledc.h>

// ── config ────────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL  1
#define NUM_LEDS        8
#define PWM_FREQ        5000    // Hz
#define PWM_RESOLUTION  8       // bits → 0-255
#define DECAY_MS        8       // ms per decay tick (≈ 120 fps)
#define DECAY_STEP      6       // brightness units dropped per tick

// GPIO pins for LEDs 0-7  (safe pins on S3, avoid 0/3/19/20/45/46)
static const uint8_t LED_PINS[NUM_LEDS] = { 18, 17, 4, 5, 6, 7, 16, 17 };

// ── state ─────────────────────────────────────────────────────────────────────
static volatile uint8_t target[NUM_LEDS]  = {};   // set by ESP-NOW ISR
static          uint8_t current[NUM_LEDS] = {};   // smoothed value written to PWM

// ── helpers ───────────────────────────────────────────────────────────────────
static void setLED(uint8_t idx, uint8_t brightness) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)idx, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)idx);
}

// ── ESP-NOW receive callback (runs in WiFi task / ISR context) ────────────────
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) return;

    // verify XOR checksum
    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) return;

    uint8_t msg_type = data[0];
    uint8_t channel  = data[1];   // MIDI channel 1-16 (or 0-15 if zero-indexed)
    // uint8_t note  = data[2];   // available if you want note-specific logic
    uint8_t vel      = data[3];   // 0-127

    if (msg_type != 0x01) return;                    // only handle note msgs

    // normalise channel to 0-indexed, clamp to 8 LEDs
    uint8_t idx = (channel > 0) ? channel - 1 : channel;
    if (idx >= NUM_LEDS) return;

    // velocity 0-127 → brightness 0-255
    target[idx] = (uint8_t)((vel * 255UL) / 127);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // configure LEDC channels
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)PWM_RESOLUTION,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = LED_PINS[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        ledc_channel_config(&ch);
    }

    // ESP-NOW init
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onReceive);

    Serial.printf("LED receiver ready  ch=%d  leds=%d\n",
                  ESPNOW_CHANNEL, NUM_LEDS);
    Serial.printf("pins: ");
    for (uint8_t i = 0; i < NUM_LEDS; i++)
        Serial.printf("%d ", LED_PINS[i]);
    Serial.println();
}

// ── loop — decay ──────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastDecay = 0;
    uint32_t now = millis();

    if (now - lastDecay >= DECAY_MS) {
        lastDecay = now;
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            uint8_t t = target[i];
            // snap up to target instantly, decay down gradually
            if (current[i] < t) {
                current[i] = t;
            } else if (current[i] > 0) {
                current[i] = (current[i] > DECAY_STEP)
                             ? current[i] - DECAY_STEP : 0;
                // also pull target down with it so it decays fully
                if (target[i] > current[i]) target[i] = current[i];
            }
            setLED(i, current[i]);
        }
    }
}
