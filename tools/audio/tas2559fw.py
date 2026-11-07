#!/usr/bin/env python3
"""TI TAS2559 + TAS2560 firmware (the Poco F1's /vendor/firmware/tas2559_uCDSP.bin): list it, or flatten a program and
configuration into the register writes both amps need, as TI's driver (tas2559-core.c) plays them.

  tas2559fw.py FW.bin                                         programs, configurations, PLLs
  tas2559fw.py FW.bin --script id OUT                         reset, both chip ids (TAS2559 R3, TAS2560 R125)
  tas2559fw.py FW.bin --script status OUT                     reads only: power, gain, mute, faults of both amps
  tas2559fw.py FW.bin --script load OUT PROGRAM CONFIG [CAL]  tas2559_set_program(): resets, defaults, program, PLL,
                                                              coefficients, and this phone's calibration (CAL.bin)
  tas2559fw.py FW.bin --script on OUT CONFIG GAIN_A GAIN_B    tas2559_enable(true): startup, DAC gains (0..15), unmute
  tas2559fw.py FW.bin --script off OUT CONFIG                 tas2559_enable(false): the shutdown sequence

The script format is tas2557fw.py's (kern.sweet_audio_i2c, tools/sweetnet/sweeti2c on the phone). What differs from the Mi 8:

 - Two amps on one bus. TAS2559 (device A) at 0x4c has the DSP, which runs both channels and feeds the TAS2560 (device
   B) at 0x4d over its own serial link. Each keeps its own book and page.
 - Device A drives the earpiece (Re about 29 ohm on this phone) and device B the loudspeaker (Re about 6.6 ohm); the
   phone's calibration file says so. Android plays music through both. Configurations with devices = 1 are earpiece only.
 - TAS2560 has a different register map (the tables below). TI's firmware block 0x0e runs on it after power-up.
 - Android runs configuration 14 at a 3.072 MHz bit clock. The kernel's player runs 16-bit stereo at 1.536 MHz, so its
   match is configuration 4: the same coefficients as 14 byte for byte, on the 1.536 MHz PLL.

The firmware and calibration are Xiaomi's/TI's and stay out of git; this only reads them.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tas2557fw as fw                                  # noqa: E402  the file format is the same

A, B = 0x4c, 0x4d
DEV_A, DEV_B = 1, 2
PLL, PGM_A, PGM_B, COEFF_A, COEFF_B, PRE_A, PRE_B, POST_B = 0x00, 0x01, 0x08, 0x03, 0x0a, 0x04, 0x0b, 0x0e
BLOCK_NAMES = {**fw.BLOCK_NAMES, POST_B: 'PST_POWERUP_DEV_B'}
BLOCK_CHIP = {PLL: A, PGM_A: A, COEFF_A: A, PRE_A: A, PGM_B: B, PRE_B: B, POST_B: B}     # tas2559_load_block
MAX_SCRIPT = 65536                                      # kern.sweet_audio_amp and kern.sweet_audio_i2c

# tas2559-core.c tables, as (chip, book, page, reg, value), ('us', N) or ('us', N, chip)
DEFAULT = [(A, 0, 0, 21, 0x05), (A, 0, 0, 45, 0x21), (A, 0, 0, 46, 0x21), (B, 0, 0, 80, 0x21), (A, 0, 0, 37, 0x5a),
           (A, 0, 0, 44, 0x00), (B, 0, 0, 33, 0x00)]
IRQ_CONFIG = [(A, 0, 1, 106, 0x71), (A, 0, 1, 108, 0x11), (A, 0, 1, 109, 0x11), (A, 0, 1, 110, 0x11),
              (A, 0, 1, 111, 0x01), (B, 0, 0, 37, 0xff), (A, 0, 1, 64, 0x07), (B, 0, 0, 35, 0x41), (A, 0, 1, 114, 0x80),
              (B, 0, 0, 36, 0x80)]
STARTUP = [(A, 0, 1, 61, 0x01), (A, 0, 1, 62, 0x01), (A, 0, 0, 5, 0xa0), (A, 0, 0, 5, 0xa3), (A, 0, 0, 4, 0xf8),
           ('us', 2000), (B, 0, 0, 2, 0x02), (B, 0, 0, 7, 0x41), ('us', 2000), (A, 0, 0, 44, 0x2b), (B, 0, 0, 33, 0x0b)]
UNMUTE = [(A, 0, 0, 7, 0x00), (B, 0, 0, 7, 0x40), (A, 100, 0, 7, 0x00)]
SHUTDOWN = [(A, 0, 0, 44, 0x00), (B, 0, 0, 33, 0x00), (A, 100, 0, 7, 0x01), (B, 0, 0, 7, 0x41), (B, 0, 0, 7, 0x01),
            ('us', 10000), ('us', 20000, B), (A, 0, 0, 4, 0x60), ('us', 2000, A), (A, 0, 0, 7, 0x03), (A, 0, 0, 5, 0x00),
            (A, 0, 0, 4, 0x00), (B, 0, 0, 2, 0x01), (A, 0, 1, 61, 0x00), (A, 0, 1, 62, 0x00)]


def wanted(chip, devices):
    return bool(devices & (DEV_A if chip == A else DEV_B))


class Script:
    """the ops, with TI's book/page switching per chip (tas2559_change_book_page)"""

    def __init__(self):
        self.b = bytearray()
        self.where = {}
        self.notes = []

    def forget(self):
        self.where = {}

    def raw_write(self, chip, reg, data):
        assert len(data) < 255
        self.b += bytes([1, chip, 1 + len(data), reg]) + bytes(data)

    def bp(self, chip, book, page):
        now = self.where.get(chip)
        if now is None or now[0] != book:
            self.raw_write(chip, 0, [0])
            self.raw_write(chip, 127, [book])
            if page:
                self.raw_write(chip, 0, [page])
        elif now[1] != page:
            self.raw_write(chip, 0, [page])
        self.where[chip] = (book, page)

    def write(self, chip, book, page, reg, data):
        self.bp(chip, book, page)
        self.raw_write(chip, reg, data if isinstance(data, (list, bytes, bytearray)) else [data])

    def read(self, chip, book, page, reg, n=1, what=''):
        self.bp(chip, book, page)
        self.b += bytes([2, chip, reg, n])
        self.notes.append(f'{what or f"{chip:#x} B{book}P{page}R{reg}"} ({n})')

    def update(self, chip, book, page, reg, mask, value):
        self.bp(chip, book, page)
        self.b += bytes([5, chip, reg, mask, value])

    def reset_pin(self, level):
        self.b += bytes([3, 1 if level else 0])

    def sleep_ms(self, ms):
        ms = max(1, int(ms))
        self.b += bytes([4, ms & 0xff, ms >> 8])

    def table(self, rows, devices=DEV_A | DEV_B):
        for r in rows:
            if r[0] == 'us':
                if len(r) == 2 or wanted(r[2], devices):
                    self.sleep_ms((r[1] + 999) // 1000)
            elif wanted(r[0], devices):
                self.write(*r)

    def hw_reset(self):
        """tas2559_hw_reset: low 5 ms, high 2 ms; kernel op 03 moves both amps' pins together"""
        self.reset_pin(0)
        self.sleep_ms(5)
        self.reset_pin(1)
        self.sleep_ms(2)
        self.forget()

    def sw_reset(self):
        """TI writes R1 to both, then waits 1 ms"""
        self.write(A, 0, 0, 1, 1)
        self.write(B, 0, 0, 1, 1)
        self.forget()
        self.sleep_ms(1)

    def block(self, blk, label):
        """tas2559_load_block: 4-byte commands book page reg value; reg 0x81 = sleep (book << 8 | page) ms;
        reg 0x85 = burst of (book << 8 | page) bytes to the register in the next command, data from its 4th byte"""
        chip = BLOCK_CHIP[blk['type']]
        d = blk['data']
        check = blk['chk'] and blk['chk'][0]
        if check:
            self.write(chip, 0, 0, *((33, 1) if chip == A else (126, 1)))       # CRC reset
        n, i = len(d) // 4, 0
        while i < n:
            w = d[4 * i:4 * i + 4]
            i += 1
            if w[2] <= 0x7f:
                self.write(chip, w[0], w[1], w[2], w[3])
            elif w[2] == 0x81:
                self.sleep_ms((w[0] << 8) | w[1])
            elif w[2] == 0x85:
                length = (w[0] << 8) | w[1]
                nxt = d[4 * i:]
                book, page, reg = nxt[0], nxt[1], nxt[2]
                data = nxt[3:3 + length] if length > 1 else nxt[3:4]
                assert reg + len(data) <= 128, f'{label}: burst leaves the page: reg {reg}, {len(data)} bytes'
                for k in range(0, len(data), 240):      # one I2C write each, the register auto-increments
                    self.write(chip, book, page, reg + k, data[k:k + 240])
                i += 1 + (((length - 2) // 4) + 1 if length >= 2 else 0)
            else:
                raise ValueError(f'{label}: unknown command {w.hex()}')
        if check:
            self.read(chip, 0, 0, *((32,) if chip == A else (126,)), 1, f'{label} PRAM checksum, firmware says {blk["chk"][1]:#04x}')


def cal_read(cal):
    """the first value the calibration writes, for reading back: proves books, pages and bursts reach the DSP"""
    for blk in cal['data']['blocks']:
        d = blk['data']
        if blk['type'] == COEFF_A and len(d) >= 12 and d[2] == 0x85:
            return d[4], d[5], d[6], bytes(d[7:11])
    return None


def of_type(data, t):
    return [b for b in data['blocks'] if b['type'] == t]


def script(kind, parsed, args, cal=None):
    h, plls, progs, cfgs, cals = parsed
    s = Script()
    if kind == 'id':
        s.hw_reset()
        s.sw_reset()
        s.read(A, 0, 0, 3, 1, 'TAS2559 PGID')
        s.read(B, 0, 0, 125, 1, 'TAS2560 ID')
        s.read(A, 0, 0, 37, 1, 'TAS2559 safe guard (0x5a only after defaults)')
    elif kind == 'status':
        s.read(A, 0, 0, 3, 1, 'A PGID')
        s.read(A, 0, 0, 4, 2, 'A POWER_CTRL1/2')
        s.read(A, 0, 0, 6, 2, 'A SPK_CTRL, MUTE')
        s.read(A, 0, 0, 37, 1, 'A safe guard')
        s.read(A, 0, 0, 100, 1, 'A POWER_UP_FLAG')
        s.read(A, 0, 0, 104, 1, 'A FLAGS_1')
        s.read(A, 0, 0, 108, 1, 'A FLAGS_2')
        s.read(A, 100, 0, 7, 1, 'A SOFT_MUTE')
        s.read(B, 0, 0, 125, 1, 'B ID')
        s.read(B, 0, 0, 2, 1, 'B DEV_MODE')
        s.read(B, 0, 0, 4, 1, 'B SPK_CTRL')
        s.read(B, 0, 0, 7, 1, 'B MUTE')
        s.read(B, 0, 0, 38, 2, 'B FLAGS_1/2')
        s.read(B, 0, 0, 42, 1, 'B POWER_UP_FLAG')
    elif kind == 'load':
        prog, cfg = int(args[0]), int(args[1])
        p, c = progs[prog], cfgs[cfg]
        assert c['program'] == prog, f'configuration {cfg} is for program {c["program"]}'
        assert not of_type(c['data'], COEFF_B), 'TI loads no CFG_COEFF_DEV_B: its driver stops on one'
        s.hw_reset()
        s.sw_reset()
        s.table(DEFAULT)                                # tas2559_load_default
        s.table(IRQ_CONFIG)                             # load_platdata: configIRQ (both irq gpios are set)
        s.update(A, 0, 1, 1, 0x18, 0 << 3)              # set_bit_rate(16), ti,bit-rate = <16>
        s.update(B, 0, 0, 20, 0x03, 0)                  # the serial link between the amps is always 16-bit
        s.update(A, 0, 1, 1, 0x01, 0x01)                # DOUT tri-state for extra BCLKs
        s.update(B, 0, 0, 24, 0x02, 0x02)
        s.update(A, 0, 1, 80, 0x30, 0x30)               # interrupt pin: low-highZ, high active driven
        for t in (PGM_A, PGM_B):
            for blk in of_type(p['data'], t):
                s.block(blk, f'program {prog} {BLOCK_NAMES[t]}')
        s.block(plls[c['pll']]['block'], f'PLL {c["pll"]}')     # tas2559_load_coefficient(-1, config, false)
        for t, dev in ((PRE_A, DEV_A), (PRE_B, DEV_B), (COEFF_A, DEV_A)):
            if c['devices'] & dev:
                for blk in of_type(c['data'], t):
                    s.block(blk, f'config {cfg} {BLOCK_NAMES[t]}')
        if cal:                                         # tas2559_set_calibration: tuning mode only, on the DSP chip
            assert p['mode'] == 2, 'TI loads the calibration in tuning mode only'
            for blk in of_type(cal['data'], COEFF_A):
                s.block(blk, 'calibration')
            first = cal_read(cal)
            if first:
                s.read(A, first[0], first[1], first[2], 4, f'calibration read back, should be {first[3].hex()}')
        s.read(A, 0, 0, 37, 1, 'A safe guard (0x5a)')
    elif kind == 'on':
        cfg, gain_a, gain_b = int(args[0]), int(args[1]), int(args[2])
        assert 0 <= gain_a <= 15 and 0 <= gain_b <= 15
        c = cfgs[cfg]
        dev = c['devices']
        s.read(A, 0, 0, 104, 1, 'A FLAGS_1 (cleared by the read)')
        s.read(A, 0, 0, 108, 1, 'A FLAGS_2')
        s.read(B, 0, 0, 38, 2, 'B FLAGS_1/2')
        # tas2559_set_DAC_gain, before anything powers up: the program leaves both amps at 15 of 15
        if dev & DEV_A:
            s.update(A, 0, 0, 6, 0x78, gain_a << 3)
        if dev & DEV_B:
            s.update(B, 0, 0, 4, 0x0f, gain_b)
        s.table(STARTUP, 3 if dev == DEV_B else dev)    # tas2559_DevStartup
        if dev & DEV_B:
            for blk in of_type(c['data'], POST_B):
                s.block(blk, f'config {cfg} {BLOCK_NAMES[POST_B]}')
        s.table(UNMUTE, dev)
        # the kernel's log shows the last 8 bytes read
        s.read(A, 0, 0, 100, 1, 'A POWER_UP_FLAG (0xc0 set = up)')
        s.read(A, 0, 0, 104, 1, 'A FLAGS_1')
        s.read(A, 0, 0, 108, 1, 'A FLAGS_2')
        s.read(A, 0, 0, 6, 1, 'A SPK_CTRL')
        s.read(B, 0, 0, 42, 1, 'B POWER_UP_FLAG (0xc0 set = up)')
        s.read(B, 0, 0, 38, 2, 'B FLAGS_1/2')
        s.read(B, 0, 0, 4, 1, 'B SPK_CTRL')
    elif kind == 'off':
        dev = cfgs[int(args[0])]['devices']
        s.table(SHUTDOWN, dev)                          # tas2559_DevShutdown
        s.read(A, 0, 0, 4, 2, 'A POWER_CTRL1/2')
    else:
        raise SystemExit(f'unknown script {kind}')
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('fw')
    ap.add_argument('--script', nargs='+', metavar=('KIND OUT', 'ARG'))
    a = ap.parse_args()
    b = open(a.fw, 'rb').read()
    parsed = fw.parse(b, device=4)
    h, plls, progs, cfgs, cals = parsed
    print(f"size {h['size']} (file {len(b)}), PPC {h['ppc']:#x}, FW {h['fw']:#x}, driver {h['driver']:#x}, DDC '{h['ddc']}'")
    if a.script:
        kind, out, args = a.script[0], a.script[1], a.script[2:]
        cal = None
        if kind == 'load' and len(args) > 2:
            cals_file = fw.parse(open(args[2], 'rb').read(), device=4)[4]
            assert len(cals_file) == 1, 'expected one calibration'
            cal = cals_file[0]
        sc = script(kind, parsed, args, cal)
        assert len(sc.b) <= MAX_SCRIPT, f'{len(sc.b)} bytes: the kernel takes {MAX_SCRIPT}'
        open(out, 'wb').write(sc.b)
        print(f'{out}: {len(sc.b)} bytes; reads, in order: ' + '; '.join(sc.notes))
        return
    for i, p in enumerate(plls):
        print(f"PLL {i}: '{p['desc']}'")
    for i, p in enumerate(progs):
        bl = ', '.join(f"{BLOCK_NAMES.get(x['type'], hex(x['type']))}:{len(x['data']) // 4}" for x in p['data']['blocks'])
        print(f"program {i}: '{p['name']}' mode {fw.APP_MODES.get(p['mode'], p['mode'])} boost {p['boost']}: {bl}")
    for i, c in enumerate(cfgs):
        bl = ', '.join(f"{BLOCK_NAMES.get(x['type'], hex(x['type']))}:{len(x['data']) // 4}" for x in c['data']['blocks'])
        print(f"config {i}: '{c['name']}' devices {c['devices']} program {c['program']} PLL {c['pll']} {c['rate']} Hz, "
              f"bit clock {c.get('pllsrc_rate')}: {bl}")


if __name__ == '__main__':
    main()
