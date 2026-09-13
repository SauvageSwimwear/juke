// ohgeeee_dynamic.ino — DYNAMIC-BUZZER variant of ohgeeee
//
// Identical to ohgeeee.ino (same LED behavior) EXCEPT the buzzers respond to
// MIDI velocity: soft notes play quieter, loud notes louder, via duty-cycle
// scaling. The stock ohgeeee.ino plays a flat "straight buzz" at full 50% duty
// on every note.
//
// UPDATED: down to 4 buzzers (GPIO 13 / channel 6 slot removed). Channels
// reassigned per new arrangement:
//   GPIO 25 → MIDI channel 1
//   GPIO 26 → MIDI channel 0  ─┐ paired — both respond to the same channel
//   GPIO 27 → MIDI channel 0  ─┘ (thickens that voice)
//   GPIO 14 → MIDI channel 10
// All four now carry an individually-tunable volume cap (BUZ_DUTY_MAX_PER) —
// see "buzzer tuning" below to quiet any one of them further.
//
// Avoided: 6–11 (flash), 34/35/36/39 (input-only), 0/2/12/15 (boot-strapping), 1/3 (USB serial)
//
// Ethos: listens only. Conductor untouched. Behavior derives entirely from ESP-NOW packets.
//
// Requires: arduino-esp32 3.x (pin-based LEDC API: ledcAttach / ledcChangeFrequency)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ── audio slots (buzzer only) ──────────────────────────────────────────────────
#define NUM_AUDIO 4
const uint8_t AUDIO_CH[NUM_AUDIO] = {  2,  1,  1,  1};   // MIDI channels (26 & 27 paired on ch 0)
const uint8_t BUZ_PIN[NUM_AUDIO]  = { 25, 26, 27, 14 };   // passive piezo GPIO

// ── buzzer tuning ─────────────────────────────────────────────────────────────
// DYNAMIC build: BUZ_DUTY_MIN < BUZ_DUTY_MAX_PER[i], so velocity scales duty
// between the two (soft/loud contrast) — same idea as before, but the ceiling
// is now per-buzzer so any one of them can be quieted without touching the
// others. Index order matches AUDIO_CH/BUZ_PIN above:
//   [0] GPIO25 ch1   [1] GPIO26 ch0   [2] GPIO27 ch0   [3] GPIO14 ch10
// All four start at a reduced 85 (down from the old flat 128 max) since all
// three groups were flagged to quiet down. Nudge any single index up/down
// to taste — e.g. raise index 0 back toward 128 if GPIO25 is fine as-is.
#define BUZ_RES_BITS   8       // LEDC resolution (0–255 duty)
const uint8_t BUZ_DUTY_MAX_PER[NUM_AUDIO] = { 85, 85, 85, 85 };  // per-buzzer loudest duty
#define BUZ_DUTY_MIN  45       // duty at lowest velocity — the dynamics floor (shared)
#define BUZ_OCTAVE_UP  0       // 0 = true pitch. Raise for loudness via resonance.

// ── LED slots (visual only) — unchanged ────────────────────────────────────────
#define NUM_LEDS_SLOT 2
const uint8_t LED_CH[NUM_LEDS_SLOT]  = {  1,  2 };   // MIDI channels
const uint8_t LED_PIN[NUM_LEDS_SLOT] = { 32, 33 };   // indicator LED GPIO

// ── LED tuning ────────────────────────────────────────────────────────────────
#define LED_DECAY_PER_MS 0.8f    // brightness lost per ms; tweak to taste (0.3–2.0)
#define LED_BRIGHT_MIN   60.0f   // floor brightness for quiet notes
#define LED_BRIGHT_MAX  220.0f   // ceiling brightness for loud notes

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1

#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02

// ── state ─────────────────────────────────────────────────────────────────────
static int16_t       activeNote[NUM_AUDIO]       = {};   // -1 = silent; guards stray note-offs
static float         ledBright[NUM_LEDS_SLOT]    = {};
static unsigned long lastLedUpdate               = 0;

// ── MIDI note → Hz ───────────────────────────────────────────────────────────
static uint32_t midiToHz(uint8_t note) {
  // MIDI 69 = A4 = 440 Hz; 12 equal-tempered semitones per octave.
  // +0.5f rounds to nearest so notes aren't systematically flat.
  return (uint32_t)(440.0f * powf(2.0f, (note - 69) / 12.0f) + 0.5f);
}

// ── ESP-NOW receive callback ──────────────────────────────────────────────────
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != 6) return;

  // Verify XOR checksum (bytes 0–4 XOR'd must equal byte 5)
  uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
  if (chk != data[5]) return;

  uint8_t msgType = data[0];
  uint8_t ch      = data[1] & 0x0F;   // low nibble = MIDI channel
  uint8_t note    = data[2];
  uint8_t vel     = data[3];

  if (msgType != MSG_NOTE_ON && msgType != MSG_NOTE_OFF) return;

  // ── audio: buzzers (velocity-scaled duty, per-buzzer ceiling) ──────────────
  // NOTE: no `break` here — GPIO26 and GPIO27 now share channel 0, so both
  // must be checked and fired on every matching packet, not just the first hit.
  for (int i = 0; i < NUM_AUDIO; i++) {
    if (ch != AUDIO_CH[i]) continue;
    bool isOn  = (msgType == MSG_NOTE_ON  && vel > 0);
    bool isOff = (msgType == MSG_NOTE_OFF) || (msgType == MSG_NOTE_ON && vel == 0);
    if (isOn) {
      // Octave-shift toward the piezo's resonant band for loudness; fall back to
      // true pitch if the shift would overflow the MIDI range.
      uint8_t bnote = note + 12 * BUZ_OCTAVE_UP;
      if (bnote > 127) bnote = note;
      // Frequency and duty set separately (not ledcWriteTone, which forces 50%)
      // so velocity can scale loudness via duty cycle.
      ledcChangeFrequency(BUZ_PIN[i], midiToHz(bnote), BUZ_RES_BITS);
      uint32_t duty = BUZ_DUTY_MIN + (uint32_t)((vel / 127.0f) * (BUZ_DUTY_MAX_PER[i] - BUZ_DUTY_MIN));
      ledcWrite(BUZ_PIN[i], duty);
      activeNote[i] = note;
    } else if (isOff && activeNote[i] == (int16_t)note) {
      // Only silence if the ringing note matches — Crick's stray-note-off guard.
      ledcWrite(BUZ_PIN[i], 0);
      activeNote[i] = -1;
    }
  }

  // ── visual: LEDs ───────────────────────────────────────────────────────────
  // Velocity-scaled brightness; note-offs let the LED decay naturally (no hard cut).
  for (int i = 0; i < NUM_LEDS_SLOT; i++) {
    if (ch != LED_CH[i]) continue;
    if (msgType == MSG_NOTE_ON && vel > 0) {
      float target = LED_BRIGHT_MIN + (vel / 127.0f) * (LED_BRIGHT_MAX - LED_BRIGHT_MIN);
      if (target > ledBright[i]) ledBright[i] = target;
    }
    break;
  }
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  // Audio init
  for (int i = 0; i < NUM_AUDIO; i++) {
    activeNote[i] = -1;
    ledcAttach(BUZ_PIN[i], 440, BUZ_RES_BITS);   // freq/duty overridden per note
    ledcWrite(BUZ_PIN[i], 0);                     // silent at boot
  }

  // LED init
  for (int i = 0; i < NUM_LEDS_SLOT; i++) {
    ledBright[i] = 0.0f;
    ledcAttach(LED_PIN[i], 5000, 8);   // 5 kHz PWM, 8-bit — real dimming, no flicker
    ledcWrite(LED_PIN[i], 0);          // dark at boot
  }

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed — halting");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  lastLedUpdate = millis();

  Serial.printf(
    "ohgeeee-dynamic ready (4 buzzers)\n"
    "  audio  ch=[%d,%d,%d,%d]  buz=[%d,%d,%d,%d]  maxduty=[%d,%d,%d,%d]\n"
    "  leds   ch=[%d,%d]  led=[%d,%d]\n",
    AUDIO_CH[0], AUDIO_CH[1], AUDIO_CH[2], AUDIO_CH[3],
    BUZ_PIN[0],  BUZ_PIN[1],  BUZ_PIN[2],  BUZ_PIN[3],
    BUZ_DUTY_MAX_PER[0], BUZ_DUTY_MAX_PER[1], BUZ_DUTY_MAX_PER[2], BUZ_DUTY_MAX_PER[3],
    LED_CH[0],   LED_CH[1],
    LED_PIN[0],  LED_PIN[1]
  );
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  unsigned long dt  = now - lastLedUpdate;
  if (dt == 0) return;
  lastLedUpdate = now;

  for (int i = 0; i < NUM_LEDS_SLOT; i++) {
    ledBright[i] -= LED_DECAY_PER_MS * (float)dt;
    if (ledBright[i] < 0.0f) ledBright[i] = 0.0f;
    ledcWrite(LED_PIN[i], (uint32_t)ledBright[i]);   // 0–255 duty = smooth velocity-scaled fade
  }
}
