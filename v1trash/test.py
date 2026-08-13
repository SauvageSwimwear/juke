import sys, serial, time, threading
sys.path.insert(0, '/home/pibb/juke/conductor')
from packet import encode, MSG_NOTE_ON, MSG_NOTE_OFF, MSG_PROGRAM

COFFEE = '/dev/ttyUSB0'
UART   = '/dev/ttyAMA0'

coffee_lines = []

def _open_no_reset(port, baud):
    """Open serial port without asserting RTS/DTR (which would reset an ESP32)."""
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.1
    s.rtscts = False
    s.dsrdtr = False
    s.open()
    s.setRTS(False)
    s.setDTR(False)
    return s

def read_coffee():
    try:
        c = _open_no_reset(COFFEE, 115200)
        print(f"[coffee] port open: {COFFEE}")
        while True:
            line = c.readline()
            if line:
                txt = line.decode(errors='replace').strip()
                coffee_lines.append(txt)
                print(f"[coffee] {txt}")
    except Exception as e:
        print(f"[coffee] ERROR: {e}")

t = threading.Thread(target=read_coffee, daemon=True)
t.start()
time.sleep(1)   # let coffee print its boot message

print()
print("[pi] opening UART …")
try:
    s = serial.Serial(UART, 115200, timeout=0)
    print(f"[pi] UART open: {UART}")
except Exception as e:
    print(f"[pi] UART open FAILED: {e}")
    sys.exit(1)

print("[pi] sending: program_change ch0 p=0 (Grand Piano)")
s.write(encode(MSG_PROGRAM, 0, 0, 0))
time.sleep(0.1)

print("[pi] sending: note_on  ch0 note=60 vel=100")
s.write(encode(MSG_NOTE_ON, 0, 60, 100))
time.sleep(2)

print("[pi] sending: note_off ch0 note=60")
s.write(encode(MSG_NOTE_OFF, 0, 60, 0))
s.close()

time.sleep(1)
print()
print("─── result ───────────────────────────────")
if any("recv" in l for l in coffee_lines):
    print("✓ coffee received ESP-NOW packets — chain works!")
elif any("ready" in l.lower() for l in coffee_lines):
    print("✗ coffee booted OK but got NO packets — chain broke at Pi→bridge or ESP-NOW")
else:
    print("✗ no output from coffee at all — check USB connection or firmware boot")
print("──────────────────────────────────────────")
