# APRS messenger: design

Status: draft, 2026-10-27. Nothing of this is built yet. Target hardware:
UV-K1 and UV-K5 V3 (PY32F071 + BK4829), base preset `Fusion`.

## Goal

The radio sits in a pocket and behaves as usual (voice channel, FM broadcast
radio). In the background it decodes APRS on 144.800 MHz. A message addressed to
the own callsign is acknowledged automatically, stored in a mailbox in the
external flash, announced with a short beep and an envelope icon. The user reads
it later and answers with T9. Outgoing messages are retried until acknowledged.

Standard APRS (APRS 1.0.1 messages, `{id` acknowledgements, reply-ack) keeps the
messages compatible with every APRS station, APRSdroid, aprs.fi and the
digipeater / iGate / APRS-IS network.

## Why resident firmware, not an overlay app

Overlay apps are modal: while an app runs, the normal radio is suspended, and
the app is discarded when it exits (`App/apps/app_api.h`). Background reception
therefore has to live in the resident firmware. The existing apps
`aprsrx`, `aprstx` and `aprsedit` are the reference for the demodulator, the AFSK
generator and the multi-tap editor; code taken from them keeps its Apache 2.0
header and is credited in `NOTICE`.

Do not use anything from the stock firmware dumps or the decompiled stock code
in `armel/k1-teardown/openocd/`: that is Quansheng's code, not open source.

## Building blocks

### 1. Audio capture (new, the core change)

`aprsrx` samples by busy-waiting on the cycle counter (`af_capture_sample`, one
blocking conversion per 104 µs). Resident code cannot block like that.

Plan: a free timer (TIM3 or TIM15; TIM6 is voice, TIM7 is backlight) triggers
ADC1 on PA4 at 9.6 kHz, DMA writes into a circular buffer of 256 × uint16
(512 B, 26.7 ms). The DMA half/full interrupt runs at the lowest priority and
feeds 128 samples to the demodulator. So the main loop, SPI-flash writes and
display updates can stall for up to ~13 ms without losing samples.

Budget: 48 MHz / 9.6 kHz = 5000 cycles per sample. The 3-slicer demodulator
must stay well below that; measure it with `cycles_48mhz` before deciding on 1
or 3 slicers.

Shared resources to solve:

- **ADC1** also measures the battery. Today `APP_AfCaptureYieldAdc()` hands the
  ADC back for one conversion. In the resident version, insert the battery
  conversion between two DMA blocks (or use an injected channel if the PY32 ADC
  offers one).
- **PA4** is the voice DAC pin, biased by DAC1 while capturing. `ENABLE_VOICE` is
  off in `Fusion`, so there is no conflict there.

### 2. Demodulator and frame filter

Port of `aprsrx`: DC removal, band-pass, I/Q correlators for 1200 / 2200 Hz,
per-tone AGC, slicers with DPLL, NRZI, HDLC, CRC-16/X.25, UI-frame check.

On top of it a filter that keeps only what the messenger needs:

- messages `:DL4PW    :text{id` addressed to the own callsign (with or without
  SSID, as configured) → mailbox + ack;
- acks / rejects for messages in the outbox → mark delivered;
- everything else is dropped (no station list in the first version).

Duplicate check by (source, message id) over the last few minutes, because the
sender retries and several digipeaters repeat the same frame.

RAM: one frame buffer (330 B), demodulator state (~200 B), DMA buffer (512 B),
dedupe table (~100 B). Fusion leaves ~4 KiB of RAM beyond its 1 KiB stack and
512 B heap.

### 3. Transmitter

Port of `aprstx`: the BK4829 tone generator (REG_70 / REG_71) switches between
1200 and 2200 Hz at each NRZI transition. A frame of ~100 bytes takes ~1 s on the
air. Blocking for that second is acceptable, like a short PTT press, but only:

- when the channel is free (no carrier, squelch closed),
- not while the user is transmitting or the radio is receiving voice,
- with a random delay of 0.5 to 3 s first, so two radios do not collide.

Sent frames: acks (immediately after a received message), own messages with
retries, the periodic beacon.

### 4. Mailbox in the external flash

The PY25Q16 map is in `App/driver/mb_flash.h`. Free space today is the
"expansion reserve" between the app slots (end 0x162000) and the Sonic 4K
reserve (0x1DE000). Proposal: **0x1CE000 to 0x1DDFFF, 64 KiB, 16 sectors**, at
the top of the reserve, as far as possible from where upstream adds app slots.
Before using it, ask upstream to reserve it officially.

Records of 128 bytes (header, timestamp, source, message id, up to 67
characters, flags: unread / read / outbox / delivered), appended as a log; a
sector is erased when the log wraps. Capacity: ~500 messages. Power-off safe:
a record counts only once its commit byte is written.

### 5. User interface

- Status bar: envelope icon while unread mail exists.
- New message: short beep (respects the beep setting), optional backlight
  pulse.
- Mailbox screen: inbox / outbox, newest first, unread in bold; open, delete,
  reply.
- Editor: T9 multi-tap from `aprsedit` (letters, digits, a few punctuation
  marks), up to 67 characters, recipient field with the last callsigns as
  suggestions.
- Settings menu: messenger on/off, callsign + SSID, beacon interval and type
  (status only, or position typed in by hand; the radio has no GPS), path
  (`WIDE1-1` or `WIDE1-1,WIDE2-1`), beep on/off.

### 6. Beacon

iGates route a message from APRS-IS to RF only if they heard the recipient on
RF recently. The radio therefore sends a beacon at a fixed interval
(default 30 min, minimum 10 min). Status-only by default, so no position leaves
the radio unless the user enters one.

### 7. Operating modes

| Mode | APRS reception | Notes |
|---|---|---|
| Pocket mode: VFO on 144.800 | continuous | most reliable, voice squelch stays closed for data |
| Dual watch: voice channel + 144.800 | partial | frames are lost while the other VFO is watched; retries make up for some of it |
| FM broadcast radio + APRS | continuous, if the audio paths are separate | BK1080 has its own path; check on the bench that broadcast audio does not reach PA4 |

APRS frames must never open the speaker: the decoder works on PA4, before the
amplifier, with the speaker muted.

### 8. Power saving

Battery save puts the BK4829 to sleep for `BATTERY_SAVE × 100 ms`, and the F4HWN
deep sleep for up to several seconds. An APRS preamble is often only 200 to
300 ms. While the messenger is on, the sleep time is capped (target: 100 ms
sleep, wake on carrier). Measure the current draw with and without the
messenger.

## Flash budget

`Fusion` today: 106 388 of 118 KiB (88 %), RAM 12 160 of 16 KiB. Estimate for
the messenger: 8 to 12 KiB of code. If it does not fit, the `Messenger` preset
drops `ENABLE_SPECTRUM` first.

## Order of work

1. New preset `Messenger` (= Fusion), builds unchanged.
2. Timer + ADC + DMA capture, measure level and CPU load on the radio
   (debug output over the USB serial port).
3. Demodulator port, receive frames in the background and log them over USB.
4. Message filter, mailbox in flash, beep and envelope icon.
5. Transmitter, automatic acks.
6. Mailbox screen and T9 editor, sending own messages with retries.
7. Beacon, settings menu, power saving.
8. Field test between the two V3 radios, then via digipeater / iGate.

## Bench tests that need the radios

- Does FM broadcast audio leak into PA4?
- Demodulator CPU load inside the interrupt, with USB and display active.
- Battery measurement while capturing.
- Current draw: plain standby vs messenger on.
- First frames from a real station (APRSdroid + KISS TNC app on the second
  radio is the easiest test sender).
