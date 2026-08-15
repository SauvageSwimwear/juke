 // ohgeeee.ino — passive buzzer triad node for the jukebox ESP-NOW mesh
//
// Spiritual successor to Crick (ESP32-C3, CircuitPython, UART tap on Coffee).
//xyxyuixyuiuyxiuxyiuxyxiuyxiuxyiuxyxiuyxiuyxxasd
// so ohgeeee is untethered from Coffee and lives anywhere in the mesh.
//
// Hardware: ESP32 DevKit (WROOM-32)
//   GPIO 25 → passive piezo buzzer 0
//   GPIO 26 → passive piezo buzzer 1
//   GPIO 27 → passive piezo buzzer 2
//   GPIO 32 → indicator LED 0
//   GPIO 33 → indicator LED 1
//   GPIO 13 → indicator LED 2
//
// Avoided: 6–11 (flash), 34/35/36/39 (input-only), 0/2/12/15 (boot-strapping), 1/3 (USB serial)
//
// Ethos: listens only. Conductor untouched. Behavior derives entirely from ESP-NOW packets.
//
// Requires: arduino-esp32 3.x (pin-based LEDC API: ledcAttach / ledcWriteTone)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ── slot config — the only section you need to edit ──────────────────────────
// Three slots: each listens to one MIDI channel (0-indexed: 0 = ch 1).
// Channels 0, 5, 10 match the Crick V1 config; change freely.
// "Channel present in every file" rule: ch 0 is the safe anchor.


#define NUM_AUDIO 3
const uint8_t AUDIO_CH[NUM_AUDIO] = {  0,  5, 10 };
const uint8_t BUZ_PIN[NUM_AUDIO]  = { 25, 26, 27 };
static int16_t activeNote[NUM_AUDIO];   // -1 = silent

// ── LED slots (visual only) ───────────────────────────────────────────────────
#define NUM_LEDS 3
const uint8_t LED_CH[NUM_LEDS]  = {  1,  4,  7 };   // pick channels with activity you like
const uint8_t LED_PIN[NUM_LEDS] = { 32, 33, 13 };
static float ledBright[NUM_LEDS] = {};

const uint8_t LISTEN_CH[NUM_SLOTS] = {  0,  5, 10 };  // MIDI channels (0-indexed)
const uint8_t BUZ_PIN[NUM_SLOTS]   = { 25, 26, 27 };  // passive piezo GPIO
const uint8_t LED_PIN[NUM_SLOTS]   = { 32, 33, 13 };  // indicator LED GPIO

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1

#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02

// ── LED decay ─────────────────────────────────────────────────────────────────
#define LED_DECAY_PER_MS 0.8f    // brightness lost per ms; tweak to taste

static float         ledBright[NUM_SLOTS] = {};
static unsigned long lastLedUpdate        = 0;

// ── active note tracking (per slot) ──────────────────────────────────────────
// Prevents a stray NOTE_OFF for a different note from cutting a still-ringing one.
// Matches Crick's active_note guard exactly.
static int16_t activeNote[NUM_SLOTS];   // -1 = silent

// ── MIDI note → Hz ───────────────────────────────────────────────────────────
static uint32_t midiToHz(uint8_t note) {
  // MIDI 69 = A4 = 440 Hz; 12 equal-tempered semitones per octave.
  return (uint32_t)(440.0f * powf(2.0f, (note - 69) / 12.0f));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != 6) return;
  uint8_t chk = data[0]^data[1]^data[2]^data[3]^data[4];
  if (chk != data[5]) return;

  uint8_t msgType = data[0];
  uint8_t ch      = data[1] & 0x0F;
  uint8_t note    = data[2];
  uint8_t vel     = data[3];

  if (msgType != MSG_NOTE_ON && msgType != MSG_NOTE_OFF) return;

  // ── audio ──────────────────────────────────────────────────────────────────
  for (int i = 0; i < NUM_AUDIO; i++) {
    if (ch != AUDIO_CH[i]) continue;
    bool isOn  = (msgType == MSG_NOTE_ON  && vel > 0);
    bool isOff = (msgType == MSG_NOTE_OFF) || (msgType == MSG_NOTE_ON && vel == 0);
    if (isOn) {
      ledcWriteTone(BUZ_PIN[i], midiToHz(note));
      activeNote[i] = note;
    } else if (isOff && activeNote[i] == (int16_t)note) {
      ledcWriteTone(BUZ_PIN[i], 0);
      activeNote[i] = -1;
    }
    break;
  }

  // ── LEDs ───────────────────────────────────────────────────────────────────
  for (int i = 0; i < NUM_LEDS; i++) {
    if (ch != LED_CH[i]) continue;
    if (msgType == MSG_NOTE_ON && vel > 0)
      ledBright[i] = 60.0f + (vel / 127.0f) * 160.0f;   // velocity-scaled brightness
    break;
  }
}
  // Verify XOR checksum (bytes 0–4 XOR'd must equal byte 5)
  uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
  if (chk != data[5]) return;

  uint8_t msgType = data[0];
  uint8_t ch      = data[1] & 0x0F;   // low nibble = MIDI channel
  uint8_t note    = data[2];
  uint8_t vel     = data[3];

  // Only handle NOTE_ON and NOTE_OFF
  if (msgType != MSG_NOTE_ON && msgType != MSG_NOTE_OFF) return;

  // Find the slot for this channel
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (ch != LISTEN_CH[i]) continue;

    bool isOn = (msgType == MSG_NOTE_ON && vel > 0);
    bool isOff = (msgType == MSG_NOTE_OFF) || (msgType == MSG_NOTE_ON && vel == 0);

    if (isOn) {
      ledcWriteTone(BUZ_PIN[i], midiToHz(note));
      activeNote[i]  = note;
      ledBright[i]   = 220.0f;   // kick the LED
    } else if (isOff) {
      // Only silence if the note-off matches the sounding note (Crick's guard)
      if (activeNote[i] == (int16_t)note) {
        ledcWriteTone(BUZ_PIN[i], 0);
        activeNote[i] = -1;
      }
    }
    break;
  }
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  // LEDs
  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(LED_PIN[i], OUTPUT);
    digitalWrite(LED_PIN[i], LOW);
    activeNote[i] = -1;
  }

  // Buzzers — attach LEDC, start silent
  // ledcAttach(pin, freq, resolution) — arduino-esp32 3.x pin-based API
  for (int i = 0; i < NUM_SLOTS; i++) {
    ledcAttach(BUZ_PIN[i], 440, 8);   // freq overridden per note; 8-bit res
    ledcWriteTone(BUZ_PIN[i], 0);     // silent at boot
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

  Serial.printf("ohgeeee ready  slots=%d  ch=[%d,%d,%d]  buz=[%d,%d,%d]  led=[%d,%d,%d]\n",
    NUM_SLOTS,
    LISTEN_CH[0], LISTEN_CH[1], LISTEN_CH[2],
    BUZ_PIN[0],   BUZ_PIN[1],   BUZ_PIN[2],
    LED_PIN[0],   LED_PIN[1],   LED_PIN[2]);
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  unsigned long dt  = now - lastLedUpdate;
  if (dt == 0) return;
  lastLedUpdate = now;

  for (int i = 0; i < NUM_SLOTS; i++) {
    ledBright[i] -= LED_DECAY_PER_MS * (float)dt;
    if (ledBright[i] < 0.0f) ledBright[i] = 0.0f;
    digitalWrite(LED_PIN[i], ledBright[i] > 0.0f ? HIGH : LOW);
  }
}
                             Y                          
