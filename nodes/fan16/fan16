// MIDI channel LEDs with velocity-based hold time and soft decay.
// Each MIDI channel maps to an LED via channel % NUM_LEDS.
// Louder notes (higher velocity) stay lit longer before fading out.

#define NUM_LEDS 8

const int ledPins[NUM_LEDS] = {6, 7, 15, 16, 18, 8, 3, 9};

// --- tuning ---
const float DECAY_PER_MS = 0.8f;   // higher = faster fade. try 0.3-2.0
const float MIN_BRIGHT   = 60.0f;  // floor: soft notes get this much hold time
const float MAX_BRIGHT   = 185.0f; // ceiling: loudest notes cap here

float ledBrightness[NUM_LEDS] = {0};
unsigned long lastLedUpdate = 0;

void ledsSetup() {
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
  lastLedUpdate = millis();
}

void ledsNoteOn(uint8_t channel, uint8_t velocity) {
  int led = channel % NUM_LEDS;
  float target = MIN_BRIGHT + (velocity / 127.0f) * (MAX_BRIGHT - MIN_BRIGHT);
  if (target > ledBrightness[led])
    ledBrightness[led] = target;
  else
    ledBrightness[led] = max(ledBrightness[led], target * 0.85f);
}

void ledsUpdate() {
  unsigned long now = millis();
  unsigned long dt = now - lastLedUpdate;
  if (dt == 0) return;
  lastLedUpdate = now;

  for (int i = 0; i < NUM_LEDS; i++) {
    ledBrightness[i] -= DECAY_PER_MS * dt;
    if (ledBrightness[i] < 0) ledBrightness[i] = 0;
    digitalWrite(ledPins[i], ledBrightness[i] > 0 ? HIGH : LOW);
  }
}
