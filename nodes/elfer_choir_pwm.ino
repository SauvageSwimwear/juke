// elfer_choir_pwm.ino — ESP32-S3
// 8 PWM "buckets" (one per MIDI channel, first-come-first-served, like
// led_matrix.ino's column assignment) + 2 legacy on/off chase lights,
// both fed by the same ESP-NOW note stream from the conductor.
//
// Requires Arduino-ESP32 core 3.x for the pin-based ledcAttach/ledcWrite
// API. On core 2.x, swap for ledcSetup(channel,...) + ledcAttachPin(pin,
// channel) + ledcWrite(channel, duty) instead.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---- mesh config (must match bridge / led_matrix / every other node) ----
#define ESPNOW_CHANNEL 1

// ---- packet format (must match conductor/packet.py) ----
#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02
#define MSG_PROGRAM  0x03
#define MSG_CC       0x04
#define MSG_CONFIG   0x05

// ---- pin layout: 8 PWM buckets + 2 chase lights ----
const uint8_t BUCKET_PINS[] = {4, 5, 6, 7, 10, 11, 12, 13};
const uint8_t BUCKET_COUNT  = sizeof(BUCKET_PINS) / sizeof(BUCKET_PINS[0]);
const uint8_t CHASE_PINS[]  = {8, 9};
const uint8_t CHASE_COUNT   = sizeof(CHASE_PINS) / sizeof(CHASE_PINS[0]);

// ---- PWM config ----
#define PWM_FREQ_HZ   5000     // well above flicker threshold
#define PWM_RES_BITS  8        // 0-255 duty

// ---- bucket brightness behavior (tune these to taste) ----
#define BUMP_BASE  0.025f       // level added per note (floor -- soft notes still register)
#define BUMP_VEL   0.045f       // extra level scaled by velocity 0..1
#define DRAIN      0.0785f      // level lost per frame at ~33fps -- lower = slower fade

// ---- chase trigger tuning (same knobs as before) ----
#define TRIGGER_CHANNEL   0     // -1 = all channels, 0-15 = one channel only
#define NOTES_PER_STEP    1

#define DEBUG_STATUS 1
#define DEBUG_INTERVAL_MS 1000

// ── bucket state ──────────────────────────────────────────────────────────
static int8_t bucketOfChannel[16];        // MIDI channel -> bucket index, -1 = unassigned
static uint8_t nextBucket = 0;             // next free bucket, first channel to speak claims it
static float   bucketLevel[8];             // 0..1 per bucket, decays every frame

// ── chase state ───────────────────────────────────────────────────────────
static uint8_t  chasePos = 0;
static int8_t   chaseDir = 1;
static uint32_t chaseQualifying = 0;       // touched only in loop(), no locking needed

// ── incoming note queue (filled by ESP-NOW recv callback, drained in loop) ──
struct NoteEvent { uint8_t channel; uint8_t velocity; };
#define EVENT_BUF 32
static NoteEvent          eventBuf[EVENT_BUF];
static volatile uint8_t   evHead = 0, evTail = 0;
static portMUX_TYPE       evMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t  totalNotesSeen = 0;   // debug only

// Deliberately minimal -- filter and queue only, no heavy work here.
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != 6) return;
  uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
  if (chk != data[5]) return;
  if (data[0] != MSG_NOTE_ON) return;
  if (data[3] == 0) return;              // velocity 0 == note-off in disguise

  NoteEvent e = { (uint8_t)(data[1] & 0x0F), data[3] };
  taskENTER_CRITICAL(&evMux);
  totalNotesSeen++;
  uint8_t next = (evHead + 1) % EVENT_BUF;
  if (next != evTail) { eventBuf[evHead] = e; evHead = next; }
  taskEXIT_CRITICAL(&evMux);
}

static void setupPwm() {
  for (uint8_t i = 0; i < BUCKET_COUNT; i++) {
    ledcAttach(BUCKET_PINS[i], PWM_FREQ_HZ, PWM_RES_BITS);
    ledcWrite(BUCKET_PINS[i], 0);
  }
}

static void handleNoteForBuckets(const NoteEvent &e) {
  int8_t bucket = bucketOfChannel[e.channel];
  if (bucket < 0) {
    if (nextBucket >= BUCKET_COUNT) return;   // all 8 buckets claimed, ignore extra voices
    bucket = bucketOfChannel[e.channel] = nextBucket++;
  }
  bucketLevel[bucket] += BUMP_BASE + BUMP_VEL * (e.velocity / 127.0f);
  if (bucketLevel[bucket] > 1.0f) bucketLevel[bucket] = 1.0f;
}

static void handleNoteForChase(const NoteEvent &e) {
  if (TRIGGER_CHANNEL >= 0 && e.channel != TRIGGER_CHANNEL) return;
  chaseQualifying++;
}

static void advanceChase() {
  for (uint8_t i = 0; i < CHASE_COUNT; i++) digitalWrite(CHASE_PINS[i], LOW);
  digitalWrite(CHASE_PINS[chasePos], HIGH);

  if (chasePos == CHASE_COUNT - 1) chaseDir = -1;
  else if (chasePos == 0)          chaseDir = 1;

  chasePos += chaseDir;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  for (int8_t &c : bucketOfChannel) c = -1;
  for (uint8_t i = 0; i < BUCKET_COUNT; i++) bucketLevel[i] = 0.0f;

  setupPwm();
  for (uint8_t i = 0; i < CHASE_COUNT; i++) {
    pinMode(CHASE_PINS[i], OUTPUT);
    digitalWrite(CHASE_PINS[i], LOW);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed -- halting");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  Serial.printf("elfer ready  buckets=%d  chase_pins=%d  trigger_channel=%d\n",
                BUCKET_COUNT, CHASE_COUNT, TRIGGER_CHANNEL);
}

void loop() {
  // drain queued note events -- feeds both subsystems from one source
  while (true) {
    taskENTER_CRITICAL(&evMux);
    bool empty = (evHead == evTail);
    NoteEvent e = {};
    if (!empty) { e = eventBuf[evTail]; evTail = (evTail + 1) % EVENT_BUF; }
    taskEXIT_CRITICAL(&evMux);
    if (empty) break;
    handleNoteForBuckets(e);
    handleNoteForChase(e);
  }

  // frame-limited update: decay buckets, advance chase, push PWM
  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  if (now - lastFrame < 30) return;   // ~33fps
  lastFrame = now;

  for (uint8_t i = 0; i < BUCKET_COUNT; i++) {
    bucketLevel[i] = max(0.0f, bucketLevel[i] - DRAIN);
    ledcWrite(BUCKET_PINS[i], (uint32_t)(bucketLevel[i] * 255));
  }

  if (chaseQualifying >= NOTES_PER_STEP) {
    uint32_t steps = chaseQualifying / NOTES_PER_STEP;
    chaseQualifying -= steps * NOTES_PER_STEP;
    for (uint32_t s = 0; s < steps; s++) advanceChase();
  }

#if DEBUG_STATUS
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= DEBUG_INTERVAL_MS) {
    lastPrint = now;
    Serial.printf("notes=%lu  buckets=[", (unsigned long)totalNotesSeen);
    for (uint8_t i = 0; i < BUCKET_COUNT; i++) Serial.printf("%.2f ", bucketLevel[i]);
    Serial.printf("]  chasePos=%d\n", chasePos);
  }
#endif
}
