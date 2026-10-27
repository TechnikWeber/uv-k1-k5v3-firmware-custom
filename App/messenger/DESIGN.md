# APRS messenger: design

Status (2026-10-27): implemented, host-tested, not yet run on a radio.
Target: UV-K1 and UV-K5 V3 (PY32F071 + BK4829), preset `Messenger` = `Fusion`
plus `ENABLE_MESSENGER`, minus `ENABLE_SPECTRUM` and the RF log. User guide: [README.md](README.md).

## Goal

The radio sits in a pocket and works as usual (voice channel, FM broadcast
radio). In the background it decodes APRS on 144.800 MHz. A message for the own
callsign is acknowledged automatically, stored in a mailbox in the external
flash, announced with a beep and an envelope icon. The user reads it later and
answers with multi-tap. Outgoing messages are retried until acknowledged.

Standard APRS (APRS 1.0.1 messages, `{id` acks, reply-acks, third-party frames
from iGates) keeps it compatible with every APRS station, APRSdroid, aprs.fi and
the digipeater / iGate / APRS-IS network. An own protocol would have none of
that infrastructure, so it could only reach as far as one radio hears another.

## Why resident firmware, not an overlay app

Overlay apps are modal: while one runs, the normal radio is suspended, and the
app is discarded when it exits (`App/apps/app_api.h`). Background reception has
to live in the resident firmware. The overlay apps `aprsrx`, `aprstx` and
`aprsedit` by Armel F4HWN were the reference for the demodulator, the AFSK
keying and the PA4 audio path; the derived code keeps their Apache 2.0 notice
and is credited in `NOTICE`.

Nothing is taken from the stock firmware dumps or the decompiled stock code in
`armel/k1-teardown/openocd/`: that is Quansheng's code, not open source.

## Files

| File | Role | Host test |
|---|---|---|
| `aprs_ax25.c` | AX.25 UI frames, addresses, FCS | `test_proto` |
| `aprs_msg.c` | APRS messages, acks, rejects, reply-acks, third-party, umlauts | `test_proto` |
| `aprs_demod.c` | Bell 202 demodulator + HDLC, block-wise (port of aprsrx) | `test_demod` (simulated audio, loopback) |
| `aprs_afsk.c` | TX bit stream: flags, bit stuffing | `test_demod` |
| `mailbox.c` | message log in the external flash | `test_mailbox` (simulated NOR) |
| `messenger.c` | settings, acks, outbox and retries, beacon, TX scheduler | `test_messenger` |
| `t9.c` | multi-tap text entry | `test_t9` |
| `msg_ui.c` | the messages screens | `ui_sim` (screens as images) |
| `msg_hw.c` | TIM3/ADC/DMA capture, AFSK transmitter | radio only |
| `msg_port.c` | glue: time, flash, carrier sense, beep, hooks | radio only |
| `msg_fw.h` | the only header the F4HWN code includes | |

Hooks in the F4HWN code, all under `#ifdef ENABLE_MESSENGER`: `main.c` (init),
`scheduler.c` (10 ms clock), `app/app.c` (10 ms task, key table, silent RX,
power save), `board.c` (battery ADC), `app/action.c` + `settings.h` +
`ui/menu.c` (action `MESSAGES` = 25), `ui/ui.c` + `ui/ui.h` (`DISPLAY_MESSENGER`),
`ui/status.c` (envelope).

## Receive path

TIM3 (free; TIM6 is voice, TIM7 backlight) triggers ADC1 on PA4 at 9.6 kHz.
DMA1 channel 1 (free) writes a circular buffer of 256 samples; its half and
full interrupts (priority 3, the lowest) hand 128 samples (13.3 ms) to the
demodulator. The main loop, flash writes and the display may stall for up to
13 ms without losing samples; the interrupt counts it when it was late (`OV`).

PA4 is held at mid-scale by the DAC with its output buffer off, as the EPIRB
406 lab found necessary. The battery measurement borrows the ADC: the capture
stops the timer, switches channel and DMA off for one conversion, and resumes.

The demodulator is the aprsrx one (DC removal, band-pass, sliding 1200/2200 Hz
correlators, per-tone AGC, 3 slicers with DPLL, NRZI, HDLC, FCS, UI check). On
the same simulated audio it decides frame by frame like the Python reference
model (`apps/aprsrx/test/model_rx.py`). Frames are limited to 200 bytes (a
message frame stays below ~170); a one-frame slot hands them to the main loop.

The capture runs while the messenger is on and a VFO is tuned to the APRS
frequency. Two firmware behaviours needed handling:

- **The BK4829 AF output is muted after every key beep** (and after other
  tones) until the next reception. The messenger switches it back to FM every
  100 ms while the radio idles on the APRS frequency; with the FM radio
  playing, only while the squelch is open, so receiver noise stays unheard.
- **An APRS burst opens the squelch**, which normally starts a reception:
  speaker on, FM radio interrupted. `MSG_IsSilentRx()` keeps the radio in the
  foreground instead (`CheckForIncoming`); only dual watch is held on the APRS
  VFO until the burst ends. With *APRS audio* on, nothing changes.

Power save is blocked while a VFO is on the APRS frequency: a sleeping BK4829
hears nothing, and many stations send only 200-300 ms of preamble.

## Transmit path

Like the APRS TX app: `RADIO_SetTxParameters()` on the APRS VFO (gCurrentVfo and
gRxVfo point to it for the call, so frequency, power, bandwidth and compander
are that VFO's; the compander is then forced off and CTCSS/DCS cleared), the
BK4829 tone generator on at level 66, REG_71 switched between 1200 and 2200 Hz
at each NRZI transition. Bit timing: TIM3 at exactly 1200 Hz, polled (a bit
that never comes aborts the frame instead of hanging). 40 flags of preamble
(267 ms), 3 tail flags; a 100-byte frame takes ~1 s, during which the main loop
waits. Then `RADIO_SetupRegisters(true)` restores reception and the FM radio
audio, and the capture restarts.

TX is refused like PTT is: TX lock outside the allowed bands, battery too low
or too high, non-FM modulation, serial configuration in progress.

## Messenger logic

- **Incoming message** for the own call (SSID must match): stored, beep, ack
  queued if it carries a number. A copy within 20 s (another digipeater) is
  ignored; a repeat later than that (the sender's retry, our ack got lost) is
  acknowledged again but not stored again (15 min window, 16 entries).
- **Acks / rejects** mark the outbox entry; a late ack after the last retry
  still marks the record. Acks from another SSID of the same call count.
- **Outbox:** up to 4 messages waiting for an ack. 5 transmissions, 30, 60, 120,
  240 s apart, then 120 s for the last ack, then failed. Without an APRS VFO a
  try is not used up (retried every 30 s). Message numbers 1-999, stored with
  the settings.
- **Broadcasts (channels):** APRS group bulletins `BLNnGROUP` (n = 0-9,
  rotating, so APRS clients that keep one line per number show them all).
  Received when GROUP is the own channel, or without a group when public
  bulletins are on; duplicates within 6 h are dropped (bulletins are often
  repeated for hours). Sent twice, 60 s apart; no ack exists for bulletins.
  Stored as their own record types, the group in the id field.
- **Notifications:** private messages beep with Beep ALL or PRIVATE, channel
  broadcasts with ALL, public bulletins never (envelope only).
- **Beacon:** status frame `>text`, 20 s after switching on, then every
  10/20/30/60 min (default 30). No position.
- **Scheduler:** the channel must be clear (APRS VFO, squelch closed or squelch
  0, no preamble in the decoder, 1.5 s since the last activity, no RX/TX/PTT/
  scan), then a random 0.7-3 s wait. At least 3 s between own frames; a token
  bucket allows 6 at once, then one per 30 s. Order: acks, messages, beacon.
- **Destination** `APZK5M` (APZ = experimental), path `WIDE1-1,WIDE2-1` by
  default.

## Encryption

Optional, for bands where encryption is allowed; refused on 144-146 and
430-440 MHz. Encrypt-then-MAC with standard primitives that are small on a
Cortex-M0 (`msg_crypto.c`, 1.9 KB): ChaCha20 (RFC 8439) and SipHash-2-4, both
checked against their published test vectors in `test_crypto`.

- **Key:** 32 bytes from the passphrase: the passphrase as a ChaCha20 key, then
  4096 times key = first half of the next block (a guess costs 4096 blocks).
  Stored in the settings record (now 128 bytes); the passphrase is not.
- **MAC key:** first 16 bytes of a ChaCha20 block under a fixed domain nonce.
- **Nonce:** 4 random bytes (sent) | SipHash(macKey, sender) (8 bytes), so two
  stations with the same key never share a nonce; a fresh one per transmission.
- **Tag:** SipHash-2-4 over sender, addressee, nonce and ciphertext, 32 bits.
- **Wire:** `@@` + base64 (no padding) of nonce | ciphertext | tag. 40 plain
  characters give 66 characters, under the 67 APRS allows. The `{id` and acks
  stay in the clear.
- **Mailbox:** plain text with the flag `MBX_F_ENCRYPTED`; encryption happens
  at each transmission, and a record that can no longer be encrypted is marked
  failed instead of being sent in the clear.

## Flash

External flash (map in `App/driver/mb_flash.h`): **0x1CE000-0x1DDFFF, 64 KiB**,
the top of the free expansion reserve, below the Sonic 4K reserve. Sector 0
holds the settings as a log of 64-byte records (an erase every 64 saves);
sectors 1-15 the mailbox, 480 records of 128 bytes. A record is written into
erased flash with its commit byte last; state changes (read, delivered,
failed, deleted) only clear bits. When the log wraps, the oldest sector is
erased (32 messages). Power-loss cases and a region full of garbage are in
`test_mailbox`. If upstream ever claims this area, the mailbox would lose its
contents but stays safe (CRC per record).

## Budget

| | Fusion | Messenger |
|---|---|---|
| Flash | 106 400 B (88 %) | 115 196 B (95 %) |
| RAM | 12 160 B (74 %) | 13 728 B (84 %) |

The messenger is ~24 KB of code (screens 7 KB, logic 4.6 KB, crypto 1.9 KB, demodulator 1.2 KB);
the spectrum analyser (~9 KB) and the RF log (~5 KB) were dropped to make
room. RAM: 3 slicers with
200-byte buffers, 512-byte DMA buffer, one frame slot. Worst stack depth of the
messenger ~700 bytes (`-fstack-usage`).

## Open points for the bench

See the bench tests in [README.md](README.md#bench-tests). The unknowns:
FM radio audio on PA4, decoder load with USB and display active, the battery
reading during capture, current draw, the level of the AF output at volume
settings, and whether the BK4829 mutes its AF output below the squelch.
