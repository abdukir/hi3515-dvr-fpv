# FRONT_PANEL.md — the 18-pin front-panel header (buttons, LEDs, IR)

> **STATUS: the daughterboard is NOT used, and all of its code was removed (2026-07-25).**
> Its switches are worn and fire on their own, which is worse than having no buttons at all,
> so the buttons, the LEDs and the whole GPIO key-matrix scan are gone from `dvr.c` — about
> 12 KB of binary. The **buzzer stays**: it is on the MAIN board, not this daughterboard.
>
> Everything below is kept as the record of how the hardware works, in case the board is
> ever repaired or replaced. What survives in the firmware is only the GPIO helpers the
> buzzer needs (`gpio_map`/`gpio_rd`/`gpio_wr`/`gpio_bit`/`gpio_dir`) mapping a single
> block, plus the sound code (see "The buzzer" and "Sounds the firmware plays" below).
> Removed commands: `PANEL`, `PSCAN`, `PGWATCH`, `PGPIO`, `PDRIVE`, `GPIOSET`, `LED`,
> `LEDSET`, `LEDANIM`, `LEDPROBE`. Removed `dvr.conf` keys: `panel`, `panel_own`.

**Short answer: yes — no MCU needed. With one caveat that is still open.**

The stock daughterboard is not smart: no microcontroller, no I²C expander. It is a passive
**5×5 key matrix wired directly to the SoC's own GPIO pins**, scanned in software by the
vendor kernel module `panel_r9508.ko`. Buttons are electrically visible to us and our
firmware reads key codes today, so the RS-485/MCU plan is unnecessary.

**The caveat (tested on hardware 2026-07-25):** the *vendor driver* is limited, and our
attempt to replace it is not finished.

| | status |
|---|---|
| header carries a plain GPIO matrix, no MCU | **confirmed** |
| buttons visible at specific (row,col) intersections | **confirmed** — row3/col0, row3/col2, row2/col1 identified |
| firmware reads key codes | **works** — codes `0x3a`, `0x44`, `0x45` observed |
| all 8 buttons distinguishable | **no** — 8 buttons produce only ~3 distinct codes |
| short taps register | **no** — the driver parks its row scan when idle and only sweeps once it notices activity, so a brief press is missed. Hold ~1 s and it registers reliably. |
| our own scan replacing the driver | **not working yet** — see "Own scan" below |

So: you can wire buttons to the header and they will be seen, but until the own-scan work
below is finished you are limited to the intersections the vendor driver has table entries
for, and presses must be held rather than tapped.

## How it works

```
/dev/panel  (misc char device 10,138, module panel_r9508.ko, depends on hi_ir.ko)
     ^
     |  read() -> 0..n bytes, ONE KEY CODE PER BYTE, 0 bytes when nothing is queued
     |            never blocks; 0xFF marks an empty slot
     |
 key_scan_timer_func ──> key_scan_5x5()   drives one row low, reads five columns
                    └──> ir_key_scan()    IR codes via hi_ir_get_key() (IRQ 9, "Hi_IR")
                    └──> led_run_blink()  the five LEDs
```

The driver reaches the pins with `hs3515_rd` / `hs3515_wr` (exported by `tl_R9508.ko`) —
plain SoC register access, **not** I²C. `client_id=33` in its boot message is unrelated to
the keypad; the keypad is pure GPIO.

Scanning works the usual way: all rows are left as high-Z inputs except the active one,
which is switched to an output driving low; the columns are pulled up, so a pressed key
pulls its column low.

## The matrix (from `key_scan_5x5`, verified live on the board)

Rows are the **drive** side (direction register at `base+0x400` is toggled), columns are the
**sense** side. Offset `0x3FC` reads all eight data bits of a block at once — the Hi3515
GPIO data registers are address-masked, with `addr[9:2]` acting as the bit mask.

| | GPIO block base | bit | conventional name* |
|---|---|---|---|
| **row 0** | `0x20160000` | 7 | GPIO2_7 |
| **row 1** | `0x20170000` | 0 | GPIO3_0 |
| **row 2** | `0x20170000` | 2 | GPIO3_2 |
| **row 3** | `0x20170000` | 4 | GPIO3_4 |
| **row 4** | `0x20180000` | 0 | GPIO4_0 |
| **col 0** | `0x20170000` | 1 | GPIO3_1 |
| **col 1** | `0x20170000` | 3 | GPIO3_3 |
| **col 2** | `0x20170000` | 5 | GPIO3_5 |
| **col 3** | `0x20180000` | 1 | GPIO4_1 |
| **col 4** | `0x20150000` | 4 | GPIO1_4 |

\* assuming the usual `0x20140000 + n × 0x10000` block numbering. The absolute addresses are
what the driver actually uses and are what matter; the names are a convenience. Note how
GPIO3 bits 0–5 alternate row/col/row/col — that is a connector-friendly layout, which is a
good hint about how the header itself is ordered.

5×5 gives up to **25 buttons**; the stock panel only populates 8.

## Verifying it on the live board

`PGPIO` (control port, or `py -3 tools/dvr.py ctl PGPIO`) reads those registers directly:

```
PGPIO cols=11111 rowdir=00001 rowval=11110
      │              │              └ row 4 is being driven LOW right now, rows 0-3 float high
      │              └ direction bits: only the active row is an output
      └ all five columns read HIGH => pulled up, nothing pressed
```

Seeing a **stable** `cols=11111` (rather than noise) is the evidence that the column
pull-ups are present and the scan is running. If you short a row pin to a column pin, the
corresponding `cols=` digit goes to `0` while that row is the active one.

## Wiring your own buttons

1. Identify the row/column pins on the 18-pin header. The reliable way, with the
   daughterboard unplugged: put a jumper between two candidate pins and watch
   `py -3 tools/dvr.py ctl PGPIO` — when you bridge a real row↔column pair, one `cols=`
   digit flickers to `0`. Do that until you have all ten lines mapped. (The header also
   carries the five LED lines, the IR receiver output, ground and 3.3 V; those will not
   move `cols=`.)
2. Wire each button between a row and a column. Any of the 25 intersections works.
3. **Keep the pull-ups.** If you build a replacement board rather than reusing the stock
   one, add ~10 kΩ from each column line to 3.3 V. The scan relies on the columns idling
   high; floating inputs give phantom keypresses.
4. Series resistors (~100 Ω) on the row lines are cheap insurance against shorting two
   driven rows together if you mis-wire.
5. **3.3 V logic only.** These are SoC pins with no level shifting.

Diodes per key are only needed if you want reliable multi-key rollover; for menu buttons
they are not worth it.

## Reading the keys

Our firmware drains `/dev/panel` at **20 Hz** (`panel_poll()`), logs each code to
`/root/rec/a1/dvr.log` as `[dvr] PANEL key 0xNN`, and keeps the last twelve for:

> The 20 Hz throttle matters. The first version polled on every main-loop iteration, which
> is thousands of `read()`s per second into a driver that shares the `tl_R9508` board glue
> with the `tw286x` capture path. `dvr.conf panel=0` disables the polling entirely if you
> ever need to rule it out.

```
py -3 tools/dvr.py ctl PANEL          -> PANEL total=3 recent: 12 04 12
py -3 tools/dvr.py ctl "PANEL CLEAR"  -> same, then empties the ring
```

**To map codes to buttons: press one, then run `PANEL`.** The code table lives in the
driver (`pkey_table` / `pir_key_table` in its `.data`), but reading codes off the hardware
is faster and cannot be wrong. IR remote codes arrive through the same interface, so the
stock remote works too if the receiver is connected — IRQ 9 (`Hi_IR`) counts those.

Nothing is bound to any action yet; wiring codes to menu actions is a small change in
`panel_poll()` in `device/dvr/dvr.c` (the same place `ui_key()` is fed by the USB keyboard).

## What the vendor driver actually does (Ghidra/objdump RE, 2026-07-25)

### `hs3515_rd` / `hs3515_wr` — the accessors (in `tl_R9508.ko`)

Not raw pokes: they take a **physical** address and translate it to the kernel virtual
address of a statically-mapped region, via a chain of range checks. Our GPIO blocks fall in
the `0x20100000-0x20200000` range. The practical consequence is the good one — **the
addresses in `key_scan_5x5` are real physical addresses**, so mapping them through
`/dev/mem` (what our firmware does) reaches exactly the same registers.

`tl_R9508.ko` also exports a set of board functions worth knowing about, none of which we
use yet:

```
buzz_control     alarm_out       alarm_in        rs485_control
screen_control   power_control   get_video_format  sys_software_reset
```

`buzz_control` answers a question from `docs/REVIEW.md` §4b: **there is a buzzer**, and it
has a driver entry point. `rs485_control` is the rear PTZ port.

### The row drive — the part that matters

`key_scan_5x5` at `0x68c`, the active-row branch:

```asm
bl  hs3515_rd            ; read DIR at base+0x400
orr r1, r0, r8, lsl r3   ; dir |= (1<<bit)          -> make the pin an OUTPUT
bl  hs3515_wr            ; write DIR                     ...... (1)
mov r1, #0               ; value = 0
add r0, r3, r0, lsl #2   ; base + ((1<<bit)<<2)      masked data address
bl  hs3515_wr            ; masked data write, value 0     ...... (2)
```

Inactive rows get only `dir &= ~(1<<bit)` — high-Z.

**Two things here are easy to get wrong, and both cost us a full debugging round:**

1. **Order.** Direction to output *first*, data bit low *second*. Parking the data bit while
   the pin is still an input does not stick. Symptom: `PDRIVE` shows `dir` set exactly as
   commanded but `val` stubbornly high, so no column can ever be pulled down.
2. **Repetition and dwell.** Step (2) runs *every* time the row is selected, and the driver
   leaves its active row driven **between timer ticks** — milliseconds, not microseconds.
   Driving all five rows inside one function call gives each line only a few microseconds,
   which is not enough to pull a column down through a key against its pull-up and the
   ribbon's capacitance. Symptom: a scan that looks perfect on a logic level and detects
   nothing at all.

Our `panel_scan_own()` now mirrors both: one row per 20 Hz call, left driven until the next
call (50 ms settle), full matrix every 250 ms.

### The buzzer

`buzz_control(on)` is one masked write:

```asm
r0 = 0x20150000 + 0x200      ; offset 0x200 = masked-data address for bit 7
r1 = on ? 0x80 : 0
bl hs3515_wr
```

**Buzzer = block `0x20150000`, bit 7, active high.** A whole-chip baseline shows that bit
parked low (silent), which agrees. Control:

```
py -3 tools/dvr.py ctl "BUZZ 250"        # non-blocking beep, 250 ms
py -3 tools/dvr.py ctl "TONE 2000 250"   # square wave; blocks while sounding
```

`BUZZ` uses a non-blocking pattern player ticked from the main loop, so it never stalls the
encoder — that is what the **record start/stop beeps** use (one short beep on start, two on
stop, in `rec_start()`/`rec_stop()`). `TONE` bit-bangs the pin and therefore busy-waits;
it is capped at 400 ms and only produces a real pitch if the transducer is passive. A
self-oscillating buzzer ignores the frequency and just sounds while driven.

### Sounds the firmware plays

All of these are tables of `{hz, ms}` pairs near the top of the buzzer section in `dvr.c`
(`hz` of 0 is a rest), so retuning is a one-line edit:

| event | motif | notes |
|---|---|---|
| record start | rising C5-E5-G5-C6 | `MEL_REC_START` |
| record stop | falling G5-E5-C5 | `MEL_REC_STOP` |
| menu move (up/down) | single 1200 Hz tick, 22 ms | `MEL_NAV` |
| select / go in | rising 1000 -> 1600 Hz | `MEL_ENTER` |
| back / exit | falling 1500 -> 900 Hz | `MEL_BACK` |
| value changed | 1800 Hz blip, 16 ms | `MEL_EDIT` |
| (spare) attention | two 1.5 kHz chirps | `MEL_ALERT`, unused |

Menu feedback goes through one dispatcher, `ui_snd()`, which every input source calls — the
front-panel buttons, the USB keyboard, the mouse (left click = select, right click = back)
and the network `UI` command — so the same action always sounds the same however it was
triggered.

**Turn it off** with `dvr.conf ui_sound=0`, or at runtime:

```
py -3 tools/dvr.py ctl "SND 0"      # silence menu feedback (record melodies still play)
py -3 tools/dvr.py ctl "SND 1"
```
`INFO` reports the current setting as `snd=`.

**Cost: none measurable.** `buzz_tone()` busy-waits, so the design point is that no single
note is long enough to matter: notes are fired one per main-loop tick, with `pump_encode()`
running in the gaps, and UI notes are 16-28 ms. Measured over 24 rapid keypresses the
encoder stayed at `pps=36` with `packs` climbing normally. The VENC ring holds roughly a
second at our bitrate, so the worst case is about two orders of magnitude inside budget.

The **power-on beep** comes from `board_R9508_init` in `tl_R9508.ko`, which runs from
`init.sh` long before our program starts, so it cannot be suppressed from the DVR — it
would need the module patched on the SATA copy.

### Pins deliberately avoided

From the same module, so we never drive them by accident: `rs485_control` = `0x20180000`
bit 5 (rear PTZ transmit enable), and key-matrix row 0 = `0x20160000` bit 7.
`power_control` (`0x20150000` bit 4) and `screen_control` (`0x20180000` bit 0) only act when
`hardware_type == 0x68` and are inert here, but are left alone regardless.

### The LEDs

`led_run_blink()` is a plain masked GPIO write:

```asm
r0 = 0x20160000
orr r0, r0, r0, lsr #23   ; -> 0x20160040   (offset 0x40 = mask for bit 4)
r1 = 0 (or 16)            ; alternates on a ~90-tick cadence
bl  hs3515_wr
```

So the **run LED is block `0x20160000`, bit 4**, and an LED is driven exactly like a matrix
row: direction to output, then a masked data write. This was confirmed twice independently —
that bit was the only one reading `T` (toggling) in a whole-chip GPIO baseline while the
vendor driver ran, and it went static (`H`) the moment the driver was unloaded.

Drive any pin from the control port:

```
py -3 tools/dvr.py ctl "GPIOSET 16 4 1"     # run LED on   (block 0x20160000, bit 4)
py -3 tools/dvr.py ctl "GPIOSET 16 4 0"     # off
```

**Confirmed on the hardware** — the board has five LEDs, in this physical order:

| LED | driven by | state |
|---|---|---|
| link | the Ethernet PHY, **not** GPIO | always on |
| alarm | `alarm_out()` — a *different* block (`0x20180000` bit 7 or `0x201a0000` bit 3, chosen by `hardware_type`) | off |
| **rec** | **block `0x20160000`, bit 3** | software |
| **run** | **block `0x20160000`, bit 4** | software |
| PWR | the power rail, **not** GPIO | always on |

So only three are software-controllable, and only two (`rec`, `run`) are in this bank —
which is why a sweep across bits 0-4 lights exactly those two and nothing else. `link` and
`PWR` being hardwired is also why they never respond.

`rec` was identified by elimination: `run` is bit 4 from `led_run_blink()`, and in a
`0→1→2→3→4` sweep the two lit LEDs come up in the order rec-then-run, putting rec at bit 3.

**Normal duty** (`led_service()` in `dvr.c`): a short sweep across the bank at startup
because it looks good, then `rec` mirrors the recording state and `run` is a ~0.8 Hz
heartbeat so you can see at a glance that the DVR is alive.

```
py -3 tools/dvr.py ctl "LED 3 1"           # rec LED on
py -3 tools/dvr.py ctl "LEDANIM 1 140"     # manual back-and-forth sweep (overrides duty)
py -3 tools/dvr.py ctl "LEDANIM 0"         # back to normal duty
py -3 tools/dvr.py ctl "LEDPROBE 1500"     # light each candidate bit in turn, to identify
py -3 tools/dvr.py ctl "GPIOSET 18 7 1"    # try the alarm LED (hardware_type dependent)
```

`panel_write()` (userspace `write()` of `{cmd,val}` pairs, commands `0x80` and `0x20`) is
the vendor's own LED entry point and is only half-decoded — driving the pins directly is
simpler and is what we do.

## How many buttons are actually on the matrix

**About three of the eight.** Four independent methods agree, which is why this is stated as
a finding rather than a suspicion:

| method | intersections seen |
|---|---|
| vendor driver key codes | 3 distinct (`0x3a`, `0x44`, `0x45`) |
| passive PSCAN (piggyback on the vendor sweep) | row3/col2, row2/col1 |
| whole-chip GPIO diff while holding buttons | row3/col0, row3/col2, row2/col1 |
| our own scan | row3/col0 (code `0x30`) |

Critically, the whole-chip watch covered **all eight GPIO blocks, every bit**, and no pin
outside `0x20170000` bits 1-5 and `0x20180000` bit 0 ever moved during a press. So the
remaining five buttons are **not connected to any SoC GPIO** on this board — they are not
simply on rows/columns we failed to identify.

Where they go is still open. The plausible candidates, in order: they are read through the
panel board's own logic and reported over a path we have not traced; they are wired to the
IR/`hi_ir` input; or they are unpopulated/unconnected on this particular unit. Note the
stock `app.out` also held `/dev/ttyAMA1` open, so a serial panel MCU has not been fully
ruled out for those five.

**Practical position:** the three matrix buttons work today and any button you wire yourself
between a row and a column pin will work — that is the useful result. You are not limited to
the vendor's key table, because our own scan gives every intersection a code.

### Current button bindings (`panel_action()` in `dvr.c`)

Confirmed working on the hardware:

| physical button | code | action |
|---|---|---|
| left | `0x30` (row3/col0) | **start / stop recording** — beeps and lights the rec LED |
| right | `0x32` / vendor | **open the menu** |
| menu | vendor `0x3a` | **close the menu** |
| — | vendor `0x44` / `0x45` | up / down |

Rebinding is a one-line change in `panel_action()`.

## Own scan — status

`dvr.conf panel_own=1` switches the firmware to scanning the matrix itself at 20 Hz with
its own debounce, giving every one of the 25 intersections a code (`0x10*row + col`) and
catching short taps. `boot.sh` unloads `panel_r9508` first (two scanners driving the same
rows would fight), and the firmware skips opening `/dev/panel` in this mode — an open fd
holds a module reference and `rmmod` fails with EAGAIN.

**It works.** After matching the vendor's write order and dwell time (above), rows drive
low correctly and the scan reports presses:

```
PDRIVE row0  dir=10000 val=01110     <- row 0 low
PDRIVE row3  dir=00010 val=11100     <- row 3 low
PGPIO (consecutive)  rowdir=00001/01000/00010/00100   <- rotating, one row parked per 50 ms
PANEL  -> code 0x30                  <- row3/col0 detected by our scan
```

`boot.sh` leaves `panel_r9508` **loaded** by default (`touch /root/rec/a1/panel_rmmod` to
unload it). Both scanners coexist safely — each only ever drives a row low or releases it to
high-Z, so there is no push-pull fight — and running both gives the widest button coverage:
the vendor path contributes `0x3a`/`0x44`/`0x45`, ours contributes every intersection it
sees. Unloading the driver also stops the run LED blinking, which is a useful tell.

## LEDs

`panel_write()` takes pairs of bytes and `led_run_blink()` drives the five LEDs, so
`write(/dev/panel, {reg, value}, 2)` is the shape of the interface. It has not been mapped
yet — a small job once the buttons are in place, and the obvious use is a recording
indicator.
