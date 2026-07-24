import serial, serial.tools.list_ports, time, sys, os

# NOTE (handoff): this passive CDC monitor was a DEAD END for the wallpaper panic -
# on the T-Watch Ultra in ARDUINO_USB_MODE=0 (OTG) the ESP-IDF panic console is NOT
# on the TinyUSB CDC (COM20), so no backtrace ever prints here. Kept for reference /
# for capturing normal app serial. For the actual panic backtrace use a core dump to
# flash or the USB-Serial-JTAG console (see argusprompt.txt step 2).
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "panic_capture.txt")
DEADLINE = time.time() + 420  # 7 minutes of reset-cycling

logf = open(LOG, "a", encoding="utf-8")
def emit(line):
    print(line, flush=True)
    try:
        logf.write(line + "\n"); logf.flush()
    except Exception:
        pass

PANIC_MARKERS = ("Guru Meditation", "Backtrace:", "abort()", "assert failed",
                 "StoreProhibited", "LoadProhibited", "InstrFetchProhibited",
                 "Interrupt wdt", "IntegerDivideByZero", "CORRUPT HEAP",
                 "Cache disabled", "panic", "Panic", "Rebooting")

def find_port():
    # app CDC = 303A:8227; download = 303A:1001 (skip). Match by hwid.
    cands = []
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "303A:8227" in hwid:
            return p.device
        if "303A" in hwid and "1001" not in hwid:
            cands.append(p.device)
    return cands[0] if cands else None

emit(f"[monitor] listening (up to 7 min). Reset-cycle the watch now.")
while time.time() < DEADLINE:
    port = find_port()
    if not port:
        time.sleep(1); continue
    try:
        s = serial.Serial()
        s.port = port; s.baudrate = 115200
        s.dtr = False; s.rts = False; s.timeout = 1
        s.open()
        emit(f"[monitor] connected {port}")
    except Exception:
        time.sleep(1); continue
    buf = b""
    try:
        while time.time() < DEADLINE:
            data = s.read(512)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                txt = line.decode("utf-8", "replace").rstrip("\r")
                emit(txt)
                if any(m in txt for m in PANIC_MARKERS):
                    emit(f"[monitor] *** PANIC-MARKER *** {txt}")
    except Exception as e:
        emit(f"[monitor] port dropped ({e}); reconnecting")
        try: s.close()
        except Exception: pass
        time.sleep(0.4)
        continue
emit("[monitor] done (timeout)")
