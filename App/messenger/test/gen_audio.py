#!/usr/bin/env python3
"""Simulated PA4 audio for the demodulator host test (test_demod.c).

Uses the channel model of the APRS RX overlay app (App/apps/aprsrx/test,
Armel F4HWN): sine AFSK of a real station or the Flipper square wave ->
discriminator + noise -> RAW or STD audio path -> 12-bit ADC at 9.6 kHz.

  gen_audio.py OUTDIR

writes OUTDIR/<case>.u16 (little-endian samples), OUTDIR/<case>.frames (each
expected frame as a 2-byte length + bytes) and OUTDIR/cases.txt.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "apps", "aprsrx", "test"))

from ax25 import build                                  # noqa: E402
from model_rx import channel, sine_wave, flipper_wave   # noqa: E402

MSG = build("DL1ABC-9", dst="APDR16", path=["WIDE1-1", "WIDE2-1"],
            info=":DL4PW    :Hallo Philipp, wie geht's?{42")
ACK = build("DL1ABC-9", dst="APDR16", path=["DB0XYZ-10*", "WIDE2*"],
            info=":DL4PW    :ack7")
GATED = build("DB0XYZ-10", dst="APRS", path=["WIDE1-1"],
              info="}DL2XYZ-5>APDR16,TCPIP,DB0XYZ-10*::DL4PW    :Gruss via Internet{a1")
POS = build("DL3QQQ", dst="APRS", path=["WIDE1-1"], info="!5130.00N/00730.00E-QRV")

bad = bytearray(MSG)
bad[30] ^= 0x10
BAD = bytes(bad)

# name: (signal builder, expected frames, mode, noise, offset, ppm)
CASES = {
    "msg_raw":        (lambda: sine_wave(MSG), [MSG], "raw", 1500, 0, 0),
    "msg_std":        (lambda: sine_wave(MSG), [MSG], "std", 1500, 0, 0),
    "msg_twist_p6":   (lambda: sine_wave(MSG, twist_db=6.0), [MSG], "std", 1500, 0, 0),
    "msg_twist_m6":   (lambda: sine_wave(MSG, twist_db=-6.0), [MSG], "std", 1500, 0, 0),
    # noise 2500 Hz: the C port and the reference model agree frame by frame
    # up to the limit (3000 Hz on this 80-byte frame: both miss 4 of 8 seeds)
    "msg_noise2500":  (lambda: sine_wave(MSG), [MSG], "std", 2500, 0, 0),
    "msg_offset":     (lambda: sine_wave(MSG), [MSG], "std", 1500, 3000, 0),
    "msg_clock_p1":   (lambda: sine_wave(MSG), [MSG], "std", 1500, 0, 10000),
    "msg_clock_m1":   (lambda: sine_wave(MSG), [MSG], "std", 1500, 0, -10000),
    "ack_digi":       (lambda: sine_wave(ACK), [ACK], "std", 1500, 0, 0),
    "gated":          (lambda: sine_wave(GATED), [GATED], "std", 1500, 0, 0),
    "flipper":        (lambda: flipper_wave(MSG), [MSG], "raw", 1500, 0, 0),
    "two_frames":     (lambda: sine_wave(POS) + sine_wave(MSG), [POS, MSG], "std", 1500, 0, 0),
    "bad_fcs":        (lambda: sine_wave(BAD), [], "std", 1500, 0, 0),
    "noise_only":     (lambda: [0.0] * 96000 * 3, [], "std", 6000, 0, 0),
}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "cases.txt"), "w") as idx:
        for name, (sig, frames, mode, noise, off, ppm) in CASES.items():
            adc = channel(sig(), mode, noise, off, ppm, seed=1)
            with open(os.path.join(out, name + ".u16"), "wb") as f:
                f.write(struct.pack("<%dH" % len(adc), *adc))
            with open(os.path.join(out, name + ".frames"), "wb") as f:
                for fr in frames:
                    f.write(struct.pack("<H", len(fr)) + fr)
            idx.write("%s %d\n" % (name, len(frames)))
            print("%-14s %7d samples, %d frame(s)" % (name, len(adc), len(frames)))


if __name__ == "__main__":
    main()
