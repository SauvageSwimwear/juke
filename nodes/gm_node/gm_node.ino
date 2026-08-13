#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "MidiOut.h"

// ── config ────────────────────────────────────────────────────────────────────
#define MIDI_TX_PIN    17       // UART1 TX → wavetable module RX
#define ESPNOW_CHANNEL 1        // must match bridge and AP channel

// ── packet constants ──────────────────────────────────────────────────────────
#define MSG_NOTE_ON  0x01
#define MSG_NOTE_OFF 0x02
#define MSG_PROGRAM  0x03
#define MSG_CC       0x04
#define MSG_CONFIG   0x05

// ─────────────────────────────────────────────────────────────────────────────
MidiOut midi;

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != 6) {
        Serial.printf("recv bad len=%d\n", len);
        return;
    }

    uint8_t chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4];
    if (chk != data[5]) {
        Serial.printf("recv bad chk got=0x%02X want=0x%02X\n", data[5], chk);
        return;
    }

    uint8_t msg_type = data[0];
    uint8_t channel  = data[1] & 0x0F;
    uint8_t d1       = data[2];
    uint8_t d2       = data[3];

    Serial.printf("recv type=0x%02X ch=%d d1=%d d2=%d\n", msg_type, channel, d1, d2);

    switch (msg_type) {
        case MSG_NOTE_ON:  midi.noteOn(d1, d2, channel);  break;
        case MSG_NOTE_OFF: midi.noteOff(d1, channel);     break;
        case MSG_PROGRAM:  midi.program(d1, channel);     break;
        case MSG_CC:       midi.cc(d1, d2, channel);      break;
        case MSG_CONFIG:   /* phase 2 */                  break;
    }
}

void setup() {
    Serial.begin(115200);

    midi.begin(MIDI_TX_PIN);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed — halting");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onRecv);

    Serial.printf("GM node ready  channel=%d  midi_tx=%d\n",
                  ESPNOW_CHANNEL, MIDI_TX_PIN);
}

void loop() {
    // everything happens in onRecv
}
