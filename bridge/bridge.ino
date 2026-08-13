#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── config ────────────────────────────────────────────────────────────────────
#define PI_RX_PIN      16       // ESP32 RX ← Pi TX
#define PI_TX_PIN      17       // not used; Pi doesn't receive from bridge
#define PI_BAUD        115200
#define ESPNOW_CHANNEL 1        // must match AP channel AND all nodes

// ── globals ───────────────────────────────────────────────────────────────────
static uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─────────────────────────────────────────────────────────────────────────────
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    Serial.printf("espnow send %s\n", status == ESP_NOW_SEND_SUCCESS ? "ok" : "FAIL");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);   // USB debug

    Serial2.begin(PI_BAUD, SERIAL_8N1, PI_RX_PIN, PI_TX_PIN);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }

    esp_now_register_send_cb(onSent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel  = ESPNOW_CHANNEL;
    peer.encrypt  = false;
    esp_now_add_peer(&peer);

    Serial.printf("Bridge ready  channel=%d  rx_pin=%d\n",
                  ESPNOW_CHANNEL, PI_RX_PIN);
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    static uint8_t buf[6];
    static uint8_t pos = 0;

    while (Serial2.available()) {
        uint8_t b = Serial2.read();

        // at position 0, sync on a valid msg_type byte (0x01–0x05)
        if (pos == 0 && (b < 0x01 || b > 0x05)) {
            continue;
        }

        buf[pos++] = b;

        if (pos < 6) continue;
        pos = 0;

        // verify XOR checksum
        uint8_t chk = buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4];
        if (chk != buf[5]) {
            Serial.printf("checksum bad: got 0x%02X want 0x%02X — resyncing\n",
                          buf[5], chk);
            continue;   // pos=0, next byte will re-sync on msg_type
        }

        Serial.printf("pi rx type=0x%02X → espnow send\n", buf[0]);
        esp_now_send(BROADCAST, buf, 6);
    }
}
