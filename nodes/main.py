import network, time, ujson
from machine import I2C, Pin
from umqtt.simple import MQTTClient
from lcd_i2c import LCD

# --- WiFi ---
wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.connect("SVG", "chocolate")
while not wlan.isconnected():
    time.sleep(0.5)

# --- LCD ---
i2c = I2C(0, scl=Pin(5), sda=Pin(4), freq=400000)
lcd = LCD(addr=0x27, cols=16, rows=2, i2c=i2c)
lcd.begin()

def show(song, state, index, total):
    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.print(song[:16])
    lcd.set_cursor(0, 1)
    lcd.print(f"{state} {index}/{total}"[:16])

def on_msg(topic, msg):
    try:
        d = ujson.loads(msg)
        show(d.get("song", ""), d.get("state", ""), d.get("index", 0), d.get("total", 0))
    except Exception as e:
        print("bad payload:", e)

# --- MQTT ---
client = MQTTClient("jukebox-lcd", "10.0.1.110", port=1884,
                     user="mqttuser", password="pibb")
client.set_callback(on_msg)
client.connect()
client.subscribe("jukebox/status")

while True:
    client.wait_msg()   # blocks until a message arrives; retained msg fires immediately on subscribe