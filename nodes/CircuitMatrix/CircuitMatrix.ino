import time
import random
import board
import neopixel
import wifi
import espnow

# ---- matrix config ----
MATRIX_W    = 8
MATRIX_H    = 8
NUM_PIXELS  = MATRIX_W * MATRIX_H
PIXEL_PIN   = board.D3
PIXEL_ORDER = neopixel.GRB
SERPENTINE  = True

# ---- render tuning ----
FRAME_S = 0.020   # ~50 fps

# Slightly faster than v1's rain (0.96/0.92) -- ring bursts light more
# pixels per event than single-point spawns, so a touch more decay keeps
# busy passages from muddying into a solid glow.
DECAY   = 0.44    # per-frame brightness retention (fade)
GRAVITY = 0.40    # fraction of brightness that drips to the row below

RING_TARGET_BASE = 0.05   # floor brightness a ring burst reaches, even at vel=1
RING_TARGET_VEL  = 0.25   # extra brightness scaled by velocity/127
# ~30% dimmer than the first pass (base/vel were 0.22/0.78) -- max burst
# brightness is now 0.70 instead of 1.0. Tune these two together if it
# still reads too hot/dim; keep VEL > BASE so quiet notes stay visibly
# quieter than loud ones.

# ---- monochrome hue drift ----
HUE_SAT             = 0.85
HUE_DRIFT_MIN       = 0.00015   # per-frame hue delta when totally quiet (~2min/cycle)
HUE_DRIFT_ACTIVITY  = 0.006     # extra per-frame delta at full activity (~3s/cycle)
ACTIVITY_BUMP       = 0.15      # activity EMA bump per note-on
ACTIVITY_DECAY      = 0.4     # activity EMA decay per frame (~1s smoothing window)

# ---- idle / boot: wandering blob (not a ring beacon) ----
# A discrete ring-flash idle always reads as a *blink*, however dim -- it's
# jumping between fixed shapes. A blob needs continuous position, not a
# ring index. So: one point random-walks the grid a single cell at a time,
# very rarely, with its OWN much slower decay than the note-burst DECAY --
# and deliberately skips gravity_frame() entirely, since a blob wandering
# in place shouldn't feel a constant downward pull the way a note-drip does.
# The old point is still fading out when the new one appears, which gives
# the "two pixels on" overlap for free without extra bookkeeping.
IDLE_TIMEOUT     = 0.50    # seconds of silence before blob mode kicks in
IDLE_DECAY       = 0.997  # per-frame retention -- ~4-5s fade per pulse
IDLE_TARGET      = 0.015  # brightness of a fresh blob step (dimmer than before)
IDLE_STEP_MIN    = 1.0    # seconds between wanders (randomized in this range,
IDLE_STEP_MAX    = 5.0    # so it doesn't feel like a metronome)

# ---- ESP-NOW ----
MSG_NOTE_ON  = 0x01
MSG_NOTE_OFF = 0x02
ESPNOW_WIFI_CHANNEL = 1

# — spiral coordinate map (outside → center, reversed to center → outside) —
def _spiral_coords():
    coords = []
    top, bottom, left, right = 0, MATRIX_H - 1, 0, MATRIX_W - 1
    while top <= bottom and left <= right:
        for x in range(left, right + 1):         coords.append((x, top))
        for y in range(top + 1, bottom + 1):     coords.append((right, y))
        if top < bottom:
            for x in range(right - 1, left - 1, -1): coords.append((x, bottom))
        if left < right:
            for y in range(bottom - 1, top, -1): coords.append((left, y))
        top += 1; bottom -= 1; left += 1; right -= 1
    coords.reverse()   # now center-outward
    return coords

SPIRAL = _spiral_coords()   # 64 (x, y) positions, index 0 = center


# ---- concentric square rings, innermost first ----
# For an 8x8 grid this yields 4 rings: inner 2x2 (4px), then 12, 20, 28px --
# same concentric-square decomposition v1's boot spiral walked pixel-by-pixel,
# just grouped into whole rings instead of a single 64-step path.
def _rings():
    rings = []
    top, bottom, left, right = 0, MATRIX_H - 1, 0, MATRIX_W - 1
    while top <= bottom and left <= right:
        ring = []
        for x in range(left, right + 1):          ring.append((x, top))
        for y in range(top + 1, bottom + 1):       ring.append((right, y))
        if top < bottom:
            for x in range(right - 1, left - 1, -1): ring.append((x, bottom))
        if left < right:
            for y in range(bottom - 1, top, -1):     ring.append((left, y))
        rings.append(ring)
        top += 1; bottom -= 1; left += 1; right -= 1
    rings.reverse()   # index 0 = innermost
    return rings

RINGS = _rings()
NUM_RINGS = len(RINGS)


def _hsv(h, s, v):
    """HSV -> (R, G, B) each 0-255. h/s/v each 0.0-1.0."""
    if s == 0:
        b = int(v * 255)
        return (b, b, b)
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    if   i == 0: r, g, b = v, t, p
    elif i == 1: r, g, b = q, v, p
    elif i == 2: r, g, b = p, v, t
    elif i == 3: r, g, b = p, q, v
    elif i == 4: r, g, b = t, p, v
    else:        r, g, b = v, p, q
    return (int(r * 255), int(g * 255), int(b * 255))


def ring_for_channel(ch):
    return ch % NUM_RINGS


# ---- pixel state ----
# Mutated in place every frame (no new list allocations) to avoid GC
# stutter on the C6's single core -- same principle as the render-loop
# guidance from earlier Matrixman prototyping.
brightness = [[0.0] * MATRIX_W for _ in range(MATRIX_H)]
hue = 0.0
activity = 0.0


def flash_ring(ring_idx, target):
    for (x, y) in RINGS[ring_idx]:
        if target > brightness[y][x]:
            brightness[y][x] = target


def idle_decay():
    # Just fade -- no downward drip. A wandering blob shouldn't feel gravity.
    for row in range(MATRIX_H):
        for col in range(MATRIX_W):
            v = brightness[row][col] * IDLE_DECAY
            brightness[row][col] = v if v > 0.002 else 0.0


def gravity_frame():
    # drip brightness downward -- bottom-to-top so each row only moves one step
    for row in range(MATRIX_H - 1, 0, -1):
        for col in range(MATRIX_W):
            fall = brightness[row - 1][col] * GRAVITY
            if fall > brightness[row][col]:
                brightness[row][col] = fall
    # decay everything
    for row in range(MATRIX_H):
        for col in range(MATRIX_W):
            v = brightness[row][col] * DECAY
            brightness[row][col] = v if v > 0.004 else 0.0


def xy_to_pixel(x, y):
    x = max(0, min(MATRIX_W - 1, x))
    y = max(0, min(MATRIX_H - 1, y))
    if SERPENTINE and (y % 2 == 1):
        x = MATRIX_W - 1 - x
    return y * MATRIX_W + x


def render(px):
    for row in range(MATRIX_H):
        for col in range(MATRIX_W):
            brt = brightness[row][col]
            idx = xy_to_pixel(col, row)
            if brt < 0.004:
                px[idx] = (0, 0, 0)
            else:
                px[idx] = _hsv(hue, HUE_SAT, min(1.0, brt))
    px.show()


# ---- ESP-NOW init ----
wifi.radio.enabled = True
try:
    wifi.radio.start_ap("_espnow", password="", channel=ESPNOW_WIFI_CHANNEL)
    print("AP started on ch", ESPNOW_WIFI_CHANNEL)
except Exception as exc:
    print("start_ap failed:", exc)
    try:
        wifi.radio.start_station()
    except Exception:
        pass

e = espnow.ESPNow()
BROADCAST = b'\xff\xff\xff\xff\xff\xff'
e.peers.append(espnow.Peer(mac=BROADCAST, channel=ESPNOW_WIFI_CHANNEL, encrypted=False))

# ---- NeoPixel init ----
pixels = neopixel.NeoPixel(
    PIXEL_PIN, NUM_PIXELS,
    brightness=1.0, auto_write=False, pixel_order=PIXEL_ORDER,
)
pixels.fill((0, 0, 0))
pixels.show()

print("matrixman v2 ready -- ring bursts + mono drift, ch", ESPNOW_WIFI_CHANNEL)

_pkt_count   = 0
_last_report = time.monotonic()
last_frame   = time.monotonic()

# Backdate so idle blob mode (== the old boot animation) starts immediately
# at power-on instead of waiting IDLE_TIMEOUT seconds.
last_activity     = time.monotonic() - IDLE_TIMEOUT
last_idle_step    = time.monotonic() - IDLE_STEP_MIN
idle_step_interval = IDLE_STEP_MIN
idle_x, idle_y      = MATRIX_W // 2, MATRIX_H // 2

# boot animation — spiral unfolds from center with circle-of-fifths hues
for i, (x, y) in enumerate(SPIRAL):
    pixels[xy_to_pixel(x, y)] = _hsv(i / len(SPIRAL), 0.85, 1.0)
    pixels.show()
    time.sleep(0.018)
time.sleep(0.4)
pixels.fill((0, 0, 0))
pixels.show()
print("circuitmatrix ready — spiral+gravity, ch", ESPNOW_WIFI_CHANNEL)

_pkt_count  = 0
_last_report = time.monotonic()
last_frame   = time.monotonic()













# ---- main loop ----
while True:
    pkt = e.read()
    while pkt is not None:
        _pkt_count += 1
        data = pkt.msg
        if len(data) == 6:
            chk = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4]
            if chk == data[5]:
                msg = data[0]
                ch  = data[1] & 0x0F
                vel = data[3]
                if msg == MSG_NOTE_ON and vel > 0:
                    ring = ring_for_channel(ch)
                    target = RING_TARGET_BASE + RING_TARGET_VEL * (vel / 127.0)
                    flash_ring(ring, target)
                    activity = min(1.0, activity + ACTIVITY_BUMP)
                    last_activity = time.monotonic()
                # note-off intentionally does nothing extra -- bursts are
                # transient by construction, DECAY/GRAVITY already retire
                # them without needing an explicit release event.
        pkt = e.read()

    now = time.monotonic()
    if now - last_frame >= FRAME_S:
        last_frame = now
        idle = (now - last_activity > IDLE_TIMEOUT)

        if idle:
            if now - last_idle_step >= idle_step_interval:
                last_idle_step = now
                idle_step_interval = random.uniform(IDLE_STEP_MIN, IDLE_STEP_MAX)
                dx = random.choice((-1, 0, 1))
                dy = random.choice((-1, 0, 1))
                if dx == 0 and dy == 0:
                    dx = 1
                idle_x = max(0, min(MATRIX_W - 1, idle_x + dx))
                idle_y = max(0, min(MATRIX_H - 1, idle_y + dy))
                if IDLE_TARGET > brightness[idle_y][idle_x]:
                    brightness[idle_y][idle_x] = IDLE_TARGET
            idle_decay()
        else:
            gravity_frame()

        hue = (hue + HUE_DRIFT_MIN + HUE_DRIFT_ACTIVITY * activity) % 1.0
        activity *= ACTIVITY_DECAY

        render(pixels)

    if now - _last_report >= 5.0:
        print("pkts rx:", _pkt_count)
        _last_report = now
