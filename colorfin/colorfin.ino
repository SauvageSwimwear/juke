/*
  AS7341 Color Finder  +  MQTT
  Board: ESP32 DevKit  |  Libraries: Adafruit AS7341, PubSubClient
  Wiring: SDA→GPIO21  SCL→GPIO22  VIN→3.3V  GND→GND

  Control (serial OR mqtt topic "colorfin/cmd"):
    s / start|stop|on|off   — pause / resume readings
    b / black|dark          — black calibrate (place on black card first)
    w / white|cal           — white calibrate (place on white card first)
    r / reset               — reset calibration
  Two-point calibration: do BOTH black and white for true reflectance.

  MQTT:
    colorfin/reading  — JSON of all bands + rgb + metadata (published each read)
    colorfin/status   — "online"/"offline" (retained, LWT)
    colorfin/cmd      — subscribe for the commands above
*/

#include <Wire.h>
#include <Adafruit_AS7341.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ── WiFi / MQTT config ───────────────────────────────────────────────────────
const char*    SSID          = "SVG";
const char*    PASSWORD      = "chocolate";
const char*    MQTT_HOST     = "10.0.1.110";
const uint16_t MQTT_PORT     = 1884;
const char*    MQTT_CLIENT_ID = "colorfin";
const char*    MQTT_USER     = "mqttuser";
const char*    MQTT_PASS     = "pibb";
const char*    TOPIC_READING = "colorfin/reading";
const char*    TOPIC_STATUS  = "colorfin/status";
const char*    TOPIC_CMD     = "colorfin/cmd";

WiFiClient   net;
PubSubClient mqtt(net);

// ── Sensor config ────────────────────────────────────────────────────────────
Adafruit_AS7341 as7341;

const uint8_t       ATIME = 29;
const uint16_t      ASTEP = 599;
const as7341_gain_t GAIN  = AS7341_GAIN_256X;
#define BAR_WIDTH     24
#define READ_INTERVAL 750     // ms between readings

// ── State ────────────────────────────────────────────────────────────────────
bool          running    = true;   // readings on by default (matches old behavior)
bool          haveWhite  = false;  // white reference captured
bool          haveBlack  = false;  // black reference captured
unsigned long lastRead   = 0;
unsigned long lastMqtt   = 0;

// ── Two-point calibration references (per spectral channel F1..F8) ───────────
float whiteRef[8];   // reading on a white card  (reflectance = 1)
float blackRef[8];   // reading on a black card  (reflectance = 0)

const as7341_color_channel_t SPECTRAL[8] = {
  AS7341_CHANNEL_415nm_F1, AS7341_CHANNEL_445nm_F2,
  AS7341_CHANNEL_480nm_F3, AS7341_CHANNEL_515nm_F4,
  AS7341_CHANNEL_555nm_F5, AS7341_CHANNEL_590nm_F6,
  AS7341_CHANNEL_630nm_F7, AS7341_CHANNEL_680nm_F8
};

// ── Helpers ──────────────────────────────────────────────────────────────────
float gammaCorrect(float v) {
  v = max(0.0f, min(1.0f, v));
  return (v <= 0.0031308f) ? 12.92f * v : 1.055f * powf(v, 1.0f/2.4f) - 0.055f;
}

bool takeReading(uint16_t *ch) {
  as7341.enableLED(true);          // flash LED to illuminate the sample
  delay(5);
  bool ok = as7341.readAllChannels(ch);
  as7341.enableLED(false);
  return ok;
}

void spectrumToRGB(uint16_t *ch, uint8_t &r, uint8_t &g, uint8_t &b) {
  // Per-band reflectance via two-point calibration:
  //   refl = (raw - black) / (white - black), clamped 0..1
  // Falls back to a nominal full-scale if white cal is missing.
  float refl[8];
  for (int i = 0; i < 8; i++) {
    float raw = (float)ch[SPECTRAL[i]];
    float lo  = haveBlack ? blackRef[i] : 0.0f;
    float hi  = haveWhite ? whiteRef[i] : 10000.0f;
    float span = max(hi - lo, 1.0f);
    refl[i] = constrain((raw - lo) / span, 0.0f, 1.0f);
  }

  // Group the 8 narrow bands into blue / green / red.
  float bl = (refl[0] + refl[1] + refl[2]) / 3.0f;  // 415 445 480 nm
  float gr = (refl[3] + refl[4])           / 2.0f;  // 515 555 nm
  float rd = (refl[5] + refl[6] + refl[7]) / 3.0f;  // 590 630 680 nm

  r = (uint8_t)(gammaCorrect(rd) * 255.0f + 0.5f);
  g = (uint8_t)(gammaCorrect(gr) * 255.0f + 0.5f);
  b = (uint8_t)(gammaCorrect(bl) * 255.0f + 0.5f);
}

void printBar(const char *label, uint16_t val, uint16_t peak) {
  int filled = peak ? (int)((float)val / peak * BAR_WIDTH + 0.5f) : 0;
  filled = min(filled, BAR_WIDTH);
  Serial.printf("  %s |", label);
  for (int i = 0; i < BAR_WIDTH; i++) Serial.print(i < filled ? '#' : '.');
  Serial.printf("| %5d", val);
  if (val >= 65535) Serial.print(" <SAT>");
  Serial.println();
}

void doWhiteCal() {
  Serial.println("  Calibrating white...");
  uint16_t ch[12] = {0};
  if (!takeReading(ch)) { Serial.println("  Read error!"); return; }
  for (int i = 0; i < 8; i++) whiteRef[i] = (float)ch[SPECTRAL[i]];
  haveWhite = true;
  Serial.printf("  White cal done.%s\n\n",
    haveBlack ? " Two-point cal active." : " Now do BLACK ('b') for true reflectance.");
}

void doBlackCal() {
  Serial.println("  Calibrating black...");
  uint16_t ch[12] = {0};
  if (!takeReading(ch)) { Serial.println("  Read error!"); return; }
  for (int i = 0; i < 8; i++) blackRef[i] = (float)ch[SPECTRAL[i]];
  haveBlack = true;
  Serial.printf("  Black cal done.%s\n\n",
    haveWhite ? " Two-point cal active." : " Now do WHITE ('w') for true reflectance.");
}

// ── Command handling (shared by serial + mqtt) ───────────────────────────────
void applyCommand(const char *cmd) {
  if      (!strcmp(cmd,"s")||!strcmp(cmd,"start")||!strcmp(cmd,"stop")||
           !strcmp(cmd,"on")||!strcmp(cmd,"off")) {
    // explicit start/stop, or 's'/toggle
    if      (!strcmp(cmd,"start")||!strcmp(cmd,"on"))  running = true;
    else if (!strcmp(cmd,"stop") ||!strcmp(cmd,"off")) running = false;
    else running = !running;                       // bare 's' toggles
    Serial.println(running ? "  >> Running.\n" : "  >> Stopped.\n");
  }
  else if (!strcmp(cmd,"w")||!strcmp(cmd,"white")||!strcmp(cmd,"cal")) doWhiteCal();
  else if (!strcmp(cmd,"b")||!strcmp(cmd,"black")||!strcmp(cmd,"dark")) doBlackCal();
  else if (!strcmp(cmd,"r")||!strcmp(cmd,"reset")) {
    haveWhite = false; haveBlack = false; Serial.println("  Cal reset.\n");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = tolower(Serial.read());
    if (c=='s'||c=='w'||c=='b'||c=='r') { char cmd[2]={c,0}; applyCommand(cmd); }
  }
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  char cmd[16];
  unsigned int n = len < sizeof(cmd)-1 ? len : sizeof(cmd)-1;
  for (unsigned int i=0;i<n;i++) cmd[i] = tolower(payload[i]);
  cmd[n] = 0;
  Serial.printf("  [mqtt cmd] %s\n", cmd);
  applyCommand(cmd);
}

// ── Networking ───────────────────────────────────────────────────────────────
void wifiConnect() {
  Serial.printf("  WiFi: connecting to %s", SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t < 10000) { delay(250); Serial.print("."); }
  if (WiFi.status()==WL_CONNECTED)
    Serial.printf(" OK  IP=%s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println(" FAILED (running offline; will retry)");
}

void mqttConnect() {
  if (WiFi.status()!=WL_CONNECTED || mqtt.connected()) return;
  if (millis()-lastMqtt < 3000) return;            // throttle attempts
  lastMqtt = millis();
  Serial.print("  MQTT: connecting... ");
  // LWT: retained "offline" on unexpected disconnect
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, TOPIC_STATUS, 0, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD);
  } else {
    Serial.printf("failed rc=%d (retry in 3s)\n", mqtt.state());
  }
}

void publishReading(uint16_t *ch, uint8_t r, uint8_t g, uint8_t b, bool sat) {
  if (!mqtt.connected()) return;
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"f1\":%u,\"f2\":%u,\"f3\":%u,\"f4\":%u,\"f5\":%u,\"f6\":%u,\"f7\":%u,\"f8\":%u,"
    "\"clear\":%u,\"nir\":%u,\"r\":%u,\"g\":%u,\"b\":%u,"
    "\"gain\":256,\"atime\":%u,\"astep\":%u,\"white\":%s,\"black\":%s,\"sat\":%s}",
    ch[AS7341_CHANNEL_415nm_F1], ch[AS7341_CHANNEL_445nm_F2],
    ch[AS7341_CHANNEL_480nm_F3], ch[AS7341_CHANNEL_515nm_F4],
    ch[AS7341_CHANNEL_555nm_F5], ch[AS7341_CHANNEL_590nm_F6],
    ch[AS7341_CHANNEL_630nm_F7], ch[AS7341_CHANNEL_680nm_F8],
    ch[AS7341_CHANNEL_CLEAR],    ch[AS7341_CHANNEL_NIR],
    r, g, b, (unsigned)ATIME, (unsigned)ASTEP,
    haveWhite ? "true":"false", haveBlack ? "true":"false", sat ? "true":"false");
  mqtt.publish(TOPIC_READING, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("\n-- AS7341 Color Finder + MQTT -------------------------------");
  Serial.println("  s = start/stop   b = black cal   w = white cal   r = reset\n");

  if (!as7341.begin()) {
    Serial.println("ERROR: AS7341 not found -- check wiring");
    while (1) delay(500);
  }
  as7341.setATIME(ATIME);
  as7341.setASTEP(ASTEP);
  as7341.setGain(GAIN);
  as7341.setLEDCurrent(10);        // 10mA — bump to 20 if readings are weak

  Serial.printf("  Integration: ~%.0f ms  |  Gain: 256x  |  LED: 10mA\n",
    (ATIME+1.0f)*(ASTEP+1.0f)*0.00278f);

  wifiConnect();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(512);
  mqttConnect();

  Serial.println("  ** Calibrate: 'b' on black card, then 'w' on white card **\n");
}

void loop() {
  handleSerial();
  mqttConnect();                   // reconnects if needed (throttled)
  mqtt.loop();                     // process incoming cmds + keepalive

  if (!running) return;
  if (millis() - lastRead < READ_INTERVAL) return;
  lastRead = millis();

  uint16_t ch[12] = {0};
  if (!takeReading(ch)) { Serial.println("Read error -- retrying..."); return; }

  uint16_t allCh[10] = {
    ch[AS7341_CHANNEL_415nm_F1], ch[AS7341_CHANNEL_445nm_F2],
    ch[AS7341_CHANNEL_480nm_F3], ch[AS7341_CHANNEL_515nm_F4],
    ch[AS7341_CHANNEL_555nm_F5], ch[AS7341_CHANNEL_590nm_F6],
    ch[AS7341_CHANNEL_630nm_F7], ch[AS7341_CHANNEL_680nm_F8],
    ch[AS7341_CHANNEL_CLEAR],    ch[AS7341_CHANNEL_NIR]
  };
  uint16_t peak = 0;
  bool sat = false;
  for (int i = 0; i < 10; i++) { peak = max(peak, allCh[i]); if (allCh[i] >= 65535) sat = true; }

  uint8_t r, g, b;
  spectrumToRGB(ch, r, g, b);

  publishReading(ch, r, g, b, sat);

  Serial.println("-------------------------------------------------------------");
  Serial.printf("  RGB #%02X%02X%02X  ->  %d, %d, %d  %s%s\n\n",
    r, g, b, r, g, b,
    (haveWhite && haveBlack) ? "" : "(uncalibrated - do b + w)",
    mqtt.connected() ? "" : "  [mqtt offline]");

  printBar("415nm F1", ch[AS7341_CHANNEL_415nm_F1], peak);
  printBar("445nm F2", ch[AS7341_CHANNEL_445nm_F2], peak);
  printBar("480nm F3", ch[AS7341_CHANNEL_480nm_F3], peak);
  printBar("515nm F4", ch[AS7341_CHANNEL_515nm_F4], peak);
  printBar("555nm F5", ch[AS7341_CHANNEL_555nm_F5], peak);
  printBar("590nm F6", ch[AS7341_CHANNEL_590nm_F6], peak);
  printBar("630nm F7", ch[AS7341_CHANNEL_630nm_F7], peak);
  printBar("680nm F8", ch[AS7341_CHANNEL_680nm_F8], peak);
  Serial.println("  ........");
  printBar("Clear   ", ch[AS7341_CHANNEL_CLEAR],    peak);
  printBar("NIR     ", ch[AS7341_CHANNEL_NIR],       peak);
  Serial.println();
}
