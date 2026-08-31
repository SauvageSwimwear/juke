// channel_leds.ino — one PWM LED per MIDI channel (16 channels → 16 LEDs).
//
// A pure ESP-NOW actuator: listens to the conductor's 6-byte packets and lights
// LED[channel] with brightness scaled by note velocity, then fades it out. No
// MIDI parsing, no channel folding — MIDI channel N drives LED N directly.
//
// Hardware: ESP32 DevKit (WROOM-32E)
//   16 LEDs, one per MIDI channel 0–15, each on its own LEDC PWM channel.
//   The classic ESP32 has exactly 16 LEDC channels, so this maxes them out —
//   no PWM channels remain for anything else (use digitalWrite if you need more).
//
// Pin map avoids: 6–11 (flash), 34/35/36/39 (input-only),
//                 0/2/12/15 (boot-strapping), 1/3 (USB serial).
//
// Ethos: listens only. Conductor untouched. Behavior derives entirely from
// ESP-NOW packets. (Matches ohgeeee / tower_light / gm_node.)
//
// Requires: arduino-esp32 3.x (pin-based LEDC API: ledcAttach / ledcWrite)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── LEDs: MIDI channel 0–15 → LED 0–15 (index == channel) ─────────────────────
#define NUM_LEDS 16
const uint8_t LED_PIN[NUM_LEDS] = {
   4,  5, 13, 14, 16, 17, 18, 19,   // channels 0–7
  21, 22, 23, 25, 26, 27, 32, 33    // channels 8–15
};

// ── LED tuning ────────────────────────────────────────────────────────────────
#define LED_PWM_FREQ     5000     // 5 kHz — flicker-free dimming
#define LED_PWM_BITS     8        // 0–255 duty
#define LED_DECAY_PER_MS 0.8f     // brightness lost per ms; tweak to taste (0.3–2.0)
#define LED_BRIGHT_MIN   60.0f    // floor brightness for quiet notes
#define LED_BRIGHT_MAX   255.0f   // ceiling brightness for loud notes

// ── ESP-NOW / packet format (see project_jukebox_v2) ──────────────────────────
#define ESPNOW_CHANNEL 1
#define MSG_NOTE_ON    0x01
#define MSG_NOTE_OFF   0x02

// ── state ─────────────────────────────────────────────────────────────────────
static float         ledBright[NUM_LEDS] = {};
static unsigned long lastLedUpdate       = 0;

// ── ESP-NOW receive callback ──────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != 6) return;

  // Verify XOR checksum (bytes 0–4 XOR'd must equal byte 5).
  uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
  if (chk != data[5]) return;

  uint8_t msgType = data[0];
  uint8_t ch      = data[1] & 0x0F;   // low nibble = MIDI channel 0–15
  uint8_t vel     = data[3];

  if (msgType != MSG_NOTE_ON) return;   // note-offs let the LED decay naturally
  if (vel == 0) return;                 // running-status note-off (vel 0)

  // Velocity-scaled brightness; only ever brighten (never cut a ringing LED short).
  float target = LED_BRIGHT_MIN + (vel / 127.0f) * (LED_BRIGHT_MAX - LED_BRIGHT_MIN);
  if (target > ledBright[ch]) ledBright[ch] = target;
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  for (int i = 0; i < NUM_LEDS; i++) {
    ledBright[i] = 0.0f;
    ledcAttach(LED_PIN[i], LED_PWM_FREQ, LED_PWM_BITS);
    ledcWrite(LED_PIN[i], 0);          // dark at boot
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed — halting");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  lastLedUpdate = millis();
  Serial.printf("channel_leds ready — %d LEDs, one per MIDI channel 0–15\n", NUM_LEDS);
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  unsigned long dt  = now - lastLedUpdate;
  if (dt == 0) return;
  lastLedUpdate = now;

  for (int i = 0; i < NUM_LEDS; i++) {
    ledBright[i] -= LED_DECAY_PER_MS * (float)dt;
    if (ledBright[i] < 0.0f) ledBright[i] = 0.0f;
    ledcWrite(LED_PIN[i], (uint32_t)ledBright[i]);
  }
}
