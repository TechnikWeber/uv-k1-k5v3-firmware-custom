#!/usr/bin/env python3
"""Screenshots of overlay apps without the radio.

The built app ELF (Thumb, Cortex-M0+) runs in Unicorn on top of the built
Fusion firmware ELF. The firmware's own app_api table (const, in flash) gives
the real drawing services: fonts, UI_PrintString*, GUI_DisplaySmallest*, lines,
rectangles, big digits, draw_frequency, the battery icon, the decimal
formatters and the division helpers. The services that touch the hardware
(keys, blits, delays, BK4819, audio, flash, serial) are stubbed here. A blit
copies gStatusLine / gFrameBuffer to an emulated LCD, saved as a PNG at the
requested times.

Time T (ms) advances with delay_ms, key scans, blits and tones, so animations
run as on the radio. Nothing is compiled: the app must be built
(App/apps/<app>/<app>.elf and <app>_assets.bin) and so must the firmware
(build/Fusion/f4hwn.fusion.elf).

  python3 -m venv venv && venv/bin/pip install unicorn pyelftools
  venv/bin/python app_screenshot.py foxhunt geonav breakout
  venv/bin/python app_screenshot.py breakout --at 1000,4000 --keys 1500:MENU

--at  capture times in ms (default: the app's scenario below, else 1000,3000)
--keys  key presses, TIME:KEY[:DURATION] (KEY: 0-9, MENU, UP, DOWN, EXIT, STAR,
        F, PTT, SIDE1, SIDE2; DURATION 100 ms by default)
"""
import argparse, glob, os, random, re, struct, sys, zlib
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)
from elftools.elf.elffile import ELFFile

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, "..", "..", "build", "Fusion", "f4hwn.fusion.elf")

KEYS = {str(i): i for i in range(10)}
KEYS.update(MENU=10, UP=11, DOWN=12, EXIT=13, STAR=14, F=15, PTT=16, SIDE2=17, SIDE1=18)
KEY_INVALID = 19

# Services run by the firmware itself: they only draw into gStatusLine /
# gFrameBuffer or compute. Every other function pointer is stubbed below; data
# pointers (fb, status_line, sys_*) are kept as the firmware sets them.
REAL = {"display_clear", "status_clear", "draw_line", "draw_rect", "print_bold",
        "print_tiny", "print_normal", "print_inverse", "display_freq", "print_string",
        "draw_frequency", "draw_battery", "append_i32", "append_u32", "idivmod",
        "uidivmod", "nav_dir", "sys_battery_percent"}

FLASH, FLASH_SZ = 0x08000000, 0x20000
RAM, RAM_SZ = 0x20000000, 0x10000
OVERLAY_SZ = 0x1000
API = 0x20008000                 # the stubbed copy of app_api
STUB = 0x20009000                # one "bx lr" per service
MAGIC = 0x2000A000               # return address of app_main
SCRATCH = 0x2000B000

LCD_PAGE = 131                   # bytes per page for lcd_feed: 3 commands + 128 data
BLIT_PAGE_MS = 1.5               # one LCD page at SPI 750 kHz


def api_fields():
    """(name, is_function) of every app_api_t field after the 4-byte prefix,
    in order, read from app_api.h."""
    src = open(os.path.join(HERE, "app_api.h")).read()
    body = src[src.index("typedef struct app_api {"):src.index("} app_api_t;")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    body = body.split("{", 1)[1]
    out = []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", decl)
        if m:
            out.append((m.group(1), True))
        else:
            out.append((re.findall(r"\w+", decl)[-1], False))
    assert [n for n, _ in out[:3]] == ["abi_major", "api_level", "api_size"], out[:3]
    return out[3:]


def load_segments(u, path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        segs = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
        sym = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        secs = {s.name: (s["sh_addr"], s.data() if s["sh_type"] != "SHT_NOBITS" else b"")
                for s in elf.iter_sections()}
    return segs, sym, secs


class Lcd:
    """The screen as the radio shows it: pages 0 (status) to 7."""
    def __init__(self):
        self.pages = [bytes(128)] * 8

    def rows(self):
        return [[self.pages[y >> 3][x] >> (y & 7) & 1 for x in range(128)] for y in range(64)]


def png(rows, path, scale=4, gap=16, plain=False):
    """LCD-look PNG (the colors of af_lcd.py), or a plain black-on-white one."""
    if plain:
        bg, fg, grid, frame, gap = (255, 255, 255), (0, 0, 0), (255, 255, 255), (255, 255, 255), 0
    else:
        bg, fg, grid, frame = (214, 216, 230), (38, 38, 70), (200, 202, 218), (60, 60, 60)
    cw, ch = 128 * scale, 64 * scale
    tw, th = cw + 2 * gap, ch + 2 * gap
    lines = []
    for y in range(th):
        line = bytearray()
        for x in range(tw):
            px, py = x - gap, y - gap
            if 0 <= px < cw and 0 <= py < ch:
                g = scale > 2 and (px % scale == scale - 1 or py % scale == scale - 1)
                line += bytes(fg if rows[py // scale][px // scale] else (grid if g else bg))
            else:
                line += bytes(frame)
        lines.append(b"\0" + bytes(line))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", tw, th, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(lines), 9)) + chunk(b"IEND", b""))


class Emu:
    def __init__(self, app_dir, firmware, scenario):
        self.sc = scenario
        self.T = 0.0
        self.lcd = Lcd()
        self.feed = 0
        self.keys = scenario.get("keys", [])
        self.shots = sorted(scenario["shots"], key=lambda s: s[1])
        self.taken = []
        self.bk = {}
        self.rng = random.Random(1)
        self.xs = 0x2545F491
        self.unknown = set()
        self.returned = False

        elfs = [p for p in glob.glob(os.path.join(app_dir, "*.elf"))]
        if len(elfs) != 1:
            sys.exit(f"{app_dir}: expected one built .elf, found {len(elfs)}")
        assets = glob.glob(os.path.join(app_dir, "*_assets.bin"))
        self.assets = open(assets[0], "rb").read() if assets else b""

        u = self.u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        u.mem_map(FLASH, FLASH_SZ)
        u.mem_map(RAM, RAM_SZ)
        u.mem_map(0x40000000, 0x30000)         # APB/AHB peripherals: writes go nowhere
        u.mem_map(0x50000000, 0x2000)          # GPIO
        u.mem_map(0xE0000000, 0x100000)        # system control space
        u.mem_map(0x1FFF0000, 0x10000)         # system memory: UID, flash size (zeros)

        segs, self.fw, secs = load_segments(u, firmware)
        fwa_addr, self.fwa = secs.get(".fw_assets", (0x90000000, b""))
        self.fwa_addr = fwa_addr
        for addr, data in segs:
            if addr >> 28 == 9:
                continue                       # .fw_assets lives in external flash
            u.mem_write(addr, data)
        vma = self.fw["__mb_workspace_start"]
        app_segs, self.app, _ = load_segments(u, elfs[0])
        if self.app.get("APP_VMA", vma) != vma:
            sys.exit(f"{elfs[0]}: APP_VMA {self.app['APP_VMA']:#x} != firmware overlay {vma:#x}, rebuild the app")
        u.mem_write(vma, bytes(OVERLAY_SZ))    # the loader zeroes the overlay (bss)
        for addr, data in app_segs:
            u.mem_write(addr, data)

        # The firmware's asset readers go to the external flash: patch them to
        # "bx lr" and copy from the .fw_assets section in the hook instead.
        self.patched = {}
        for name in ("FW_ASSETS_Read", "FW_ASSETS_ReadStr"):
            if name in self.fw:
                a = self.fw[name] & ~1
                u.mem_write(a, b"\x70\x47")
                self.patched[a] = name

        # A plausible status line: battery 5 bars and its percentage.
        for name, fmt, v in (("gBatteryDisplayLevel", "<B", 5), ("gBatteryVoltageAverage", "<H", 790),
                             ("gSetting_battery_text", "<B", 2)):
            if name in self.fw:
                u.mem_write(self.fw[name], struct.pack(fmt, v))

        # The stubbed copy of the firmware's app_api table.
        fields = api_fields()
        table = self.fw["app_api"]
        head = bytes(u.mem_read(table, 4))
        size = struct.unpack("<H", head[2:])[0]
        if size != 4 + 4 * len(fields):
            sys.exit(f"app_api.h has {len(fields)} services but the firmware table is {size} bytes: "
                     "rebuild the firmware")
        real = bytes(u.mem_read(table, size))
        tab = bytearray(head)
        self.stubs = {}
        self.fb = self.sl = None
        for i, (name, fn) in enumerate(fields):
            ptr = struct.unpack_from("<I", real, 4 + 4 * i)[0]
            if name == "fb":
                self.fb = ptr
            elif name == "status_line":
                self.sl = ptr
            if fn and ptr and name not in REAL:
                a = STUB + 4 * i
                u.mem_write(a, b"\x70\x47\x00\xbf")
                self.stubs[a] = name
                ptr = a | 1
            tab += struct.pack("<I", ptr)
        u.mem_write(API, bytes(tab))
        u.mem_write(MAGIC, b"\x00\xbf\x00\xbf")
        u.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUB, end=STUB + 4 * len(fields))
        for a in self.patched:
            u.hook_add(UC_HOOK_CODE, self.on_patched, begin=a, end=a + 1)
        self.t_end = self.shots[-1][1] + 50

    # ---- helpers ----
    def r(self, n):
        return self.u.reg_read((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)[n])

    def ret(self, v, hi=None):
        self.u.reg_write(UC_ARM_REG_R0, v & 0xFFFFFFFF)
        if hi is not None:
            self.u.reg_write(UC_ARM_REG_R1, hi & 0xFFFFFFFF)

    def page(self, p):
        if p == 0:
            self.lcd.pages[0] = bytes(self.u.mem_read(self.sl, 128))
        else:
            self.lcd.pages[p] = bytes(self.u.mem_read(self.fb + 128 * (p - 1), 128))

    def key_now(self):
        for t, k, d in self.keys:
            if t <= self.T < t + d:
                return k
        return KEY_INVALID

    def tick(self, ms):
        self.T += ms
        while self.shots and self.T >= self.shots[0][1]:
            label, t = self.shots.pop(0)
            self.taken.append((label, t, [row[:] for row in self.lcd.rows()]))
        if not self.shots and self.T >= self.t_end:
            self.u.emu_stop()

    # ---- hooks ----
    def on_patched(self, u, addr, size, data):
        name = self.patched[addr]
        if name == "FW_ASSETS_Read":
            src, dst, n = self.r(0), self.r(1), self.r(2)
        else:
            dst, src, n = self.r(0), self.r(1), self.r(2)
        o = src - self.fwa_addr
        d = self.fwa[o:o + n] if 0 <= o < len(self.fwa) else b""
        d = d + bytes(n - len(d))
        if name == "FW_ASSETS_ReadStr" and n:
            d = d[:n - 1] + b"\0"
        u.mem_write(dst, d)

    def on_stub(self, u, addr, size, data):
        name = self.stubs.get(addr)
        if name is None:
            return
        sc = self.sc
        if name == "get_key":
            self.tick(0.4)
            self.ret(self.key_now())
        elif name == "delay_ms":
            self.tick(self.r(0))
        elif name == "ticks_ms":
            self.tick(0.01)
            self.ret(int(self.T) // 10 * 10)
        elif name == "cycles_48mhz":
            self.tick(0.03)
            self.ret(int(self.T * 48000))
        elif name == "blit_full":
            for p in range(1, 8):
                self.page(p)
            self.tick(7 * BLIT_PAGE_MS)
        elif name == "blit_status":
            self.page(0)
            self.tick(BLIT_PAGE_MS)
        elif name == "blit_line":
            self.page(self.r(0) + 1)
            self.tick(BLIT_PAGE_MS)
        elif name == "blit_page":
            p = self.r(0)
            for q in (range(1, 8) if p == 8 else [p]):
                self.page(q)
            self.tick(BLIT_PAGE_MS * (7 if p == 8 else 1))
        elif name == "lcd_feed":
            n = self.r(0)
            if n == 0:
                self.feed = 0
            else:
                done = self.feed // LCD_PAGE
                self.feed = min(self.feed + n, 8 * LCD_PAGE)
                for p in range(done, self.feed // LCD_PAGE):
                    self.page(p)
                self.tick(n * 0.0107)
            self.ret(self.feed < 8 * LCD_PAGE)
        elif name in ("play_tone", "play_tone_raw"):
            self.tick(self.r(1))
        elif name == "rand32":
            x = self.xs
            x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
            self.xs = x
            self.ret(x)
        elif name == "asset_read":
            off, buf, n = self.r(0), self.r(1), self.r(2)
            d = self.assets[off:off + n]
            if d:
                u.mem_write(buf, d)
            self.ret(len(d))
        elif name == "cfg_load":
            buf, n = self.r(0), self.r(1)
            cfg = sc.get("cfg", b"")[:n]
            u.mem_write(buf, cfg + b"\xff" * (n - len(cfg)))
        elif name == "handoff_read":
            buf, mx, frm = self.r(0), self.r(1), self.r(2)
            msg = sc.get("handoff")
            if msg is None:
                self.ret(-1)
            else:
                if mx:
                    u.mem_write(buf, msg[:mx])
                if frm:
                    u.mem_write(frm, sc.get("handoff_from", "").encode()[:16] + b"\0")
                self.ret(len(msg))
        elif name == "rssi_dbm":
            self.ret(int(sc["rssi"](self.T, self.rng)) if "rssi" in sc else -120)
        elif name in ("rx_freq", "tx_freq"):
            self.ret(sc.get("freq", 14450000))
        elif name == "bk_write":
            self.bk[self.r(0) & 0xFF] = self.r(1) & 0xFFFF
        elif name == "bk_read":
            self.ret(self.bk.get(self.r(0) & 0xFF, 0))
        elif name == "af_capture_sample":
            self.tick(0.02)
            self.ret(2048 + self.rng.randint(-40, 40))
        elif name == "boot_callsign":
            buf, n = self.r(0), self.r(1)
            if n:
                u.mem_write(buf, sc.get("callsign", "F4HWN").encode()[:n - 1] + b"\0")
        elif name == "sys_storage_read":
            u.mem_write(self.r(1), b"\xff" * self.r(2))
        elif name in ("sys_stack_free_now", "sys_stack_free_min"):
            self.ret(2048)
        elif name == "mr":
            self.ret(-1)
        elif name in ("led", "backlight_on", "backlight_update", "battery_sample", "set_agc",
                      "set_af", "audio_path", "prepare_tone", "tones_off_rx", "cfg_save",
                      "audio_scope", "tx_set_params", "tx_tone", "tx_mute", "tx_end",
                      "tx_carrier", "tx_prepare", "viewer_update", "af_capture_open",
                      "af_capture_close", "fm_mute", "fm_exit", "fm_commit", "beam_get",
                      "beam_prepare", "beam_draw"):
            self.ret(0)
        else:
            if name not in self.unknown:
                self.unknown.add(name)
                print(f"  note: {name}() stubbed, returns 0")
            self.ret(0)

    def run(self):
        u = self.u
        u.reg_write(UC_ARM_REG_SP, self.fw.get("_estack", RAM + 0x4000))
        u.reg_write(UC_ARM_REG_R0, API)
        u.reg_write(UC_ARM_REG_LR, MAGIC | 1)
        try:
            u.emu_start(self.app["app_main"] | 1, MAGIC, timeout=120_000_000)
        except Exception as e:
            pc = u.reg_read(UC_ARM_REG_PC)
            print(f"  emulation stopped at T={self.T:.0f} ms, pc={pc:#x}: {e}")
        self.returned = u.reg_read(UC_ARM_REG_PC) & ~1 == MAGIC
        for label, t in self.shots:            # the app returned before these
            self.taken.append((label, t, self.lcd.rows()))
        return self


# ---- scenarios: the context an app needs for a telling screen ----

def aprs_home(lat_d, lat_m100, lon_d, lon_m100, hemi=0):
    """APRS TX profile prefix as APRS Edit saves it (aprs_profile.h)."""
    digits = [int(c) for c in f"{lat_d:02d}{lat_m100:04d}{lon_d:03d}{lon_m100:04d}"]
    return struct.pack("<I", 0x37435041) + bytes([0, 0, 0, hemi]) + bytes(digits)


def fox_rssi(t, rng):
    """A fox approached: -118 dBm rising to about -70 dBm, with fading."""
    return min(-118 + t / 120, -70) + rng.gauss(0, 2.5)


def cas(deg, minutes):
    return int(round((deg * 60 + minutes) * 6000))


SCENARIOS = {
    "foxhunt": dict(shots=[("main", 6000)], rssi=fox_rssi),
    "geonav": dict(shots=[("main", 1500)],
                   cfg=aprs_home(48, 5000, 2, 2000),
                   handoff=b"V4130237\0\0\0\0" + struct.pack("<iii", cas(48, 58.20), cas(2, 35.40), 12345)
                           + bytes(8),
                   handoff_from="Sonde RS41"),
    "breakout": dict(shots=[("title", 1400), ("game", 12000)], keys=[(2000, KEYS["MENU"], 100)]),
}


def parse_keys(s):
    out = []
    for item in filter(None, s.split(",")):
        p = item.split(":")
        out.append((float(p[0]), KEYS[p[1].upper()], float(p[2]) if len(p) > 2 else 100.0))
    return out


def find_app(name):
    d = os.path.join(HERE, name.lower())
    if os.path.isdir(d):
        return d
    if os.path.isdir(name):
        return name
    sys.exit(f"{name}: no such app folder in {HERE}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("apps", nargs="+")
    ap.add_argument("--at", help="capture times in ms, comma-separated")
    ap.add_argument("--keys", help="TIME:KEY[:DURATION],...")
    ap.add_argument("--firmware", default=FIRMWARE)
    ap.add_argument("--out", default=".")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--plain", action="store_true", help="black on white, no frame or grid")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name in a.apps:
        d = find_app(name)
        app = os.path.basename(os.path.normpath(d))
        sc = dict(SCENARIOS.get(app, dict(shots=[("1s", 1000), ("3s", 3000)])))
        if a.at:
            sc["shots"] = [(f"{t}ms", float(t)) for t in a.at.split(",")]
        if a.keys is not None:
            sc["keys"] = parse_keys(a.keys)
        print(f"{app}:")
        e = Emu(d, a.firmware, sc).run()
        for label, t, rows in e.taken:
            path = os.path.join(a.out, f"{app}-{label}.png")
            png(rows, path, scale=a.scale, plain=a.plain)
            print(f"  {path} (T={t:.0f} ms)")
        if e.returned:
            print("  app_main returned")


if __name__ == "__main__":
    main()
