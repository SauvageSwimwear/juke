// Stage 4: MIDI jukebox with WiFi + MQTT control + HTTP file manager.
// Hardware: UART1 on pin 17, 31250 baud raw MIDI.
//
// MQTT broker: 10.0.1.110:1884
//   Subscribe  jukebox/control  ->  skip | pause | resume | play/<filename.mid>
//   Publish    jukebox/status   ->  JSON  {"state":"playing","song":"...","index":N,"total":N}
//
// HTTP file manager: http://jukebox.local
//   GET  /         -> file list + upload form
//   POST /upload   -> upload a .mid file
//   GET  /delete?f -> delete a file
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include "MidiOut.h"
#include "SmfPlayer.h"

// ---------- credentials (hardcoded; change here + OTA reflash if needed) ----------
static const char WIFI_SSID[]      = "SVG";
static const char WIFI_PASS[]      = "chocolate";
static const char MQTT_HOST[]      = "10.0.1.110";
static const int  MQTT_PORT        = 1884;
static const char MQTT_USER[]      = "mqttuser";
static const char MQTT_PASS[]      = "pibb";
static const char MQTT_CLIENT_ID[] = "jukebox";
static const char TOPIC_CTRL[]     = "jukebox/control";
static const char TOPIC_STATUS[]   = "jukebox/status";

// ---------- playlist ----------
#define MAX_PLAYLIST 256
static String playlist[MAX_PLAYLIST];
static int playlistCount = 0;
static int currentIndex  = -1;

SmfPlayer player;

// ---------- MQTT command flags (set in callback, consumed in loop) ----------

static bool gSkip    = false;
static bool gShuffle = false;
static bool gPause   = false;
static bool gResume  = false;
static String gPlayFile = "";
static int gVolume = -1;        // -1 = no pending change; 0-127 = send CC#7
static uint8_t currentVolume = 100;

#define LED_TEMPO_PIN 4
static unsigned long ledTempoOffAt = 0;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    httpServer(80);
static File  uploadFile;

// ---------- helpers ----------
static bool isMidiFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mid") || lower.endsWith(".midi");
}

static void buildPlaylist() {
  playlistCount = 0;
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f && playlistCount < MAX_PLAYLIST) {
    if (!f.isDirectory() && isMidiFile(f.name()))
      playlist[playlistCount++] = String(f.name());
    f = root.openNextFile();
  }
}

static void publishStatus(const char *state, const String &song) {
  if (!mqtt.connected()) return;
  char buf[192];
  snprintf(buf, sizeof(buf),
    "{\"state\":\"%s\",\"song\":\"%s\",\"index\":%d,\"total\":%d}",
    state, song.c_str(), currentIndex + 1, playlistCount);
  mqtt.publish(TOPIC_STATUS, buf, true); // retained so late subscribers see it
}

static void playNext() {
  if (playlistCount == 0) return;
  currentIndex = (currentIndex + 1) % playlistCount;
  String path = playlist[currentIndex];
  if (!path.startsWith("/")) path = "/" + path;
  Serial.printf("Now playing [%d/%d]: %s\n", currentIndex + 1, playlistCount, path.c_str());
  if (player.begin(LittleFS, path.c_str())) {
    player.setVolume(currentVolume);
    publishStatus("playing", path);
  } else {
    Serial.printf("  failed to open/parse %s, skipping\n", path.c_str());
  }
}

static void playRandom() {
  if (playlistCount == 0) return;
  int idx = playlistCount > 1 ? random(playlistCount) : 0;
  // Avoid repeating the same track twice in a row when there's a choice.
  if (playlistCount > 1 && idx == currentIndex) idx = (idx + 1) % playlistCount;
  currentIndex = idx;
  String path = playlist[currentIndex];
  if (!path.startsWith("/")) path = "/" + path;
  Serial.printf("Shuffle -> [%d/%d]: %s\n", currentIndex + 1, playlistCount, path.c_str());
  if (player.begin(LittleFS, path.c_str())) {
    player.setVolume(currentVolume);
    publishStatus("playing", path);
  } else {
    Serial.printf("  failed to open/parse %s, skipping\n", path.c_str());
  }
}

static void playFile(const String &filename) {
  // Find the file in the playlist so currentIndex stays accurate.
  String target = filename.startsWith("/") ? filename : "/" + filename;
  for (int i = 0; i < playlistCount; i++) {
    String p = playlist[i].startsWith("/") ? playlist[i] : "/" + playlist[i];
    if (p.equalsIgnoreCase(target)) {
      currentIndex = i;
      Serial.printf("Jump to [%d/%d]: %s\n", currentIndex + 1, playlistCount, target.c_str());
      if (player.begin(LittleFS, target.c_str())) {
        player.setVolume(currentVolume);
        publishStatus("playing", target);
      }
      return;
    }
  }
  Serial.printf("play: file not found in playlist: %s\n", target.c_str());
}

// ---------- MQTT ----------
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("MQTT [%s] -> %s\n", topic, msg.c_str());

  if (msg == "skip")        { gSkip    = true; }
  else if (msg == "shuffle" || msg == "random") { gShuffle = true; }
  else if (msg == "pause")  { gPause   = true; }
  else if (msg == "resume") { gResume  = true; }
  else if (msg.startsWith("play/")) {
    gPlayFile = msg.substring(5);
  } else if (msg.startsWith("volume/")) {
    int v = msg.substring(7).toInt();
    gVolume = constrain(v, 0, 127);
  }
}

static void mqttConnect() {
  while (!mqtt.connected()) {
    Serial.printf("Connecting to MQTT %s:%d ...", MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                     TOPIC_STATUS, 0, true, "{\"state\":\"offline\"}")) {
      Serial.println(" OK");
      mqtt.subscribe(TOPIC_CTRL);
      // Announce we're online without changing the current song field.
      publishStatus("online", currentIndex >= 0 ? playlist[currentIndex] : "");
    } else {
      Serial.printf(" failed (rc=%d), retry in 5s\n", mqtt.state());
      delay(5000);
    }
  }
}

// ---------- HTTP file manager ----------
static void handleRoot() {
  String html;
  html.reserve(8192);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Jukebox</title>"
            "<style>"
            "body{font-family:monospace;background:#111;color:#ddd;padding:1em;max-width:700px}"
            "h2{color:#f90}table{width:100%;border-collapse:collapse}"
            "td,th{padding:3px 6px;text-align:left}"
            "tr:nth-child(even){background:#1a1a1a}"
            ".now{color:#f90;font-weight:bold}"
            "button{background:#933;color:#fff;border:none;padding:1px 7px;cursor:pointer;border-radius:3px}"
            "input[type=submit]{background:#363;color:#fff;border:none;padding:4px 14px;cursor:pointer;border-radius:3px}"
            "</style></head><body>");
  html += "<h2>Jukebox</h2>";

  // Status
  if (currentIndex >= 0 && currentIndex < playlistCount) {
    String song = playlist[currentIndex];
    html += "<p>Now playing: <span class='now'>" + song + "</span>";
    html += player.isPaused() ? " [paused]" : " [playing]";
    html += "</p>";
  }

  // Volume slider (auto-submits on change, no JS libs needed)
  html += "<form method='GET' action='/volume'>Volume: <input type='range' name='v' "
          "min='0' max='127' value='" + String(currentVolume) + "' "
          "style='vertical-align:middle' onchange='this.form.submit()'> " +
          String(currentVolume) + "/127</form><br>";

  // Upload form
  html += F("<form method='POST' action='/upload' enctype='multipart/form-data'>"
            "<input type='file' name='file' accept='.mid,.midi'>&nbsp;"
            "<input type='submit' value='Upload'></form><br>");

  // File list
  html += "<table><tr><th>File</th><th>KB</th><th></th></tr>";
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  int count = 0;
  while (f) {
    if (!f.isDirectory() && isMidiFile(f.name())) {
      bool isCurrent = (currentIndex >= 0 && playlist[currentIndex] == String(f.name()));
      html += isCurrent ? "<tr class='now'>" : "<tr>";
      html += "<td>" + String(f.name()) + "</td>";
      html += "<td>" + String(f.size() / 1024) + "</td>";
      html += "<td><form method='GET' action='/delete' style='display:inline'>"
              "<input type='hidden' name='f' value='" + String(f.name()) + "'>"
              "<button>del</button></form></td></tr>";
      count++;
    }
    f = root.openNextFile();
  }
  html += "</table><p>" + String(count) + " file(s) &nbsp; ";

  // Free space
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  html += String((total - used) / 1024 / 1024) + " MB free</p>";
  html += "</body></html>";

  httpServer.send(200, "text/html", html);
}

static void handleVolume() {
  if (!httpServer.hasArg("v")) { httpServer.send(400, "text/plain", "missing ?v="); return; }
  int v = httpServer.arg("v").toInt();
  currentVolume = (uint8_t)constrain(v, 0, 127);
  Serial.printf("HTTP: volume %d\n", currentVolume);
  player.setVolume(currentVolume);
  httpServer.sendHeader("Location", "/");
  httpServer.send(303);
}

static void handleDelete() {
  if (!httpServer.hasArg("f")) { httpServer.send(400, "text/plain", "missing ?f="); return; }
  String path = httpServer.arg("f");
  if (!path.startsWith("/")) path = "/" + path;

  // If deleting the current song, skip first so the file handle is closed.
  if (currentIndex >= 0) {
    String cur = playlist[currentIndex];
    if (!cur.startsWith("/")) cur = "/" + cur;
    if (cur.equalsIgnoreCase(path)) playNext();
  }

  LittleFS.remove(path);
  Serial.printf("HTTP: deleted %s\n", path.c_str());
  buildPlaylist();
  httpServer.sendHeader("Location", "/");
  httpServer.send(303);
}

static void handleUploadFinish() {
  httpServer.sendHeader("Location", "/");
  httpServer.send(303);
}

static void handleFileUpload() {
  HTTPUpload &upload = httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (!isMidiFile(filename)) { uploadFile = File(); return; } // ignore non-MIDI
    Serial.printf("HTTP upload: %s\n", filename.c_str());
    uploadFile = LittleFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("HTTP upload done: %u bytes\n", upload.totalSize);
      buildPlaylist();
    }
  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(esp_random()); // hardware RNG -- without this, random() repeats the same sequence every boot
  pinMode(LED_TEMPO_PIN, OUTPUT);
  ledsSetup();

  if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    Serial.println("LittleFS mount failed -- did you upload the data/ folder?");
    while (true) delay(1000);
  }

  buildPlaylist();
  Serial.printf("Found %d MIDI file(s) in LittleFS.\n", playlistCount);
  if (playlistCount == 0) {
    Serial.println("No .mid files found -- upload some via the LittleFS data upload tool.");
    while (true) delay(1000);
  }

  // WiFi
  Serial.printf("Connecting to WiFi '%s' ...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK  IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(" TIMEOUT -- running without MQTT");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  if (WiFi.status() == WL_CONNECTED) {
    mqttConnect();

    ArduinoOTA.setHostname("jukebox");
    ArduinoOTA.onStart([]() {
      player.end();   // silence + close file before flash starts
      Serial.println("OTA: starting");
    });
    ArduinoOTA.onEnd([]()    { Serial.println("OTA: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
    ArduinoOTA.begin();
    Serial.println("OTA ready");

    httpServer.on("/",       HTTP_GET,  handleRoot);
    httpServer.on("/volume", HTTP_GET,  handleVolume);
    httpServer.on("/delete", HTTP_GET,  handleDelete);
    httpServer.on("/upload", HTTP_POST, handleUploadFinish, handleFileUpload);
    httpServer.begin();
    Serial.println("HTTP file manager at http://jukebox.local");
  }

  player.setNoteOnCallback([](uint8_t ch, uint8_t vel){ ledsNoteOn(ch, vel); });
  playNext();
}

// ---------- loop ----------
void loop() {
  // WiFi + MQTT + OTA + HTTP keepalive
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    httpServer.handleClient();
    if (!mqtt.connected()) mqttConnect();
    mqtt.loop();
  }

  // Consume flags from MQTT callback
  if (gSkip) {
    gSkip = false;
    Serial.println("MQTT: skip");
    playNext();
    return;
  }
  if (gShuffle) {
    gShuffle = false;
    Serial.println("MQTT: shuffle");
    playRandom();
    return;
  }
  if (gPlayFile.length()) {
    String f = gPlayFile;
    gPlayFile = "";
    Serial.printf("MQTT: play/%s\n", f.c_str());
    playFile(f);
    return;
  }
  if (gPause) {
    gPause = false;
    if (!player.isPaused()) {
      player.pause();
      Serial.println("MQTT: pause");
      publishStatus("paused", currentIndex >= 0 ? playlist[currentIndex] : "");
    }
  }
  if (gResume) {
    gResume = false;
    if (player.isPaused()) {
      player.resume();
      Serial.println("MQTT: resume");
      publishStatus("playing", currentIndex >= 0 ? playlist[currentIndex] : "");
    }
  }
  if (gVolume >= 0) {
    currentVolume = (uint8_t)gVolume;
    Serial.printf("MQTT: volume %d\n", currentVolume);
    player.setVolume(currentVolume);
    gVolume = -1;
  }

  // LEDs
  ledsUpdate();
  if (player.consumeBeatActivity())  { digitalWrite(LED_TEMPO_PIN, HIGH); ledTempoOffAt = millis() + 60; }
  if (millis() >= ledTempoOffAt)       digitalWrite(LED_TEMPO_PIN, LOW);

  if (currentIndex == -1) return;
  if (!player.update()) {
    playNext();
  }
}
