SERIAL_PORT  = "/dev/ttyAMA0"   # Pi UART TX → bridge ESP32 RX
SERIAL_BAUD  = 115200

MQTT_HOST    = "10.0.1.110"
MQTT_PORT    = 1884
MQTT_USER    = "mqttuser"
MQTT_PASS    = "pibb"
MQTT_CTL     = "jukebox/control"
MQTT_STATUS  = "jukebox/status"

MIDI_DIR     = "/home/pibb/juke/data"

# load-shedding: drop quiet note_ons when serial TX buffer exceeds this
SHED_BACKLOG  = 512   # bytes
SHED_VELOCITY = 45    # notes quieter than this are dropped under load

# stuck-note handling
NOTE_TIMEOUT      = 8.0    # seconds a note may sound before the watchdog kills it
WATCHDOG_INTERVAL = 0.25   # how often the watchdog sweeps for overdue notes
PANIC_REPEAT      = 3      # times to resend a panic (beats ESP-NOW packet loss)
