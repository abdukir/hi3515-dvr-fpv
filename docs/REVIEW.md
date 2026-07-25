# REVIEW.md — architecture review, defects found, and what to build next

Written 2026-07-25 after a full pass over `device/dvr/` (the on-device DVR), `webapp2/`
(the PC console) and the flash/persistence path. It's a working document: the "fixed"
items were fixed in that same pass, the rest is a ranked backlog with enough detail to
pick up cold.

## 1. How the system actually fits together

Three programs, one data flow, and one hard constraint (ARM926 @ ~200 MHz, 43 MB RAM,
no libc, single MPP owner):

```
        TW2866 (4x composite)
              │
        VI (only the channels something drains — see 2b)
         ├──────────────► VO / VGA  ── the DVR's own monitor, codec-free, ~130 ms
         │                    ▲          glass-to-glass, plus an fb0 OSD + fb4 cursor
         │                    │
         └─► VENC (ch0 only) ─┼─► pump_encode() ─┬─► MPEG-TS file on SATA   (recording)
                              │                  └─► TCP 8091 fan-out       (live)
                    VDEC ◄────┘   (playback of a recording, on the same VEDU)

   control: TCP 8090  +  /dev/ttyAMA1 (MCU)   ──►  one record-state owner
   PC:      webapp2 ── 8090 control · 8091 media(→WebSocket→jMuxer) · 23 telnet · 8081 httpd
```

The single most important structural decision, and it's a good one: **capture and encode
happen once, and disk / network / display are independent consumers.** Adding a viewer
costs a `write()`; recording costs a `write()`; the VGA path doesn't touch the codec at
all. That's why a 200 MHz ARM9 can do all three at 704×240 and ~17 Mbps.

The second good decision: **the device stays dumb about presentation.** It emits raw
Annex-B and a line protocol; jMuxer/MSE in the browser does the muxing. There is no
ffmpeg in the live path on either side.

### Where the complexity actually lives

- `dvr.c` is ~2 600 lines and does MPP bring-up, recording, streaming, control, OSD,
  playback and thumbnails. `docs/DVR_DESIGN.md` still promises a split into
  `mpp.c`/`rec.c`/`net.c`/`ui.c`; that hasn't happened and the file is now past the point
  where it should.
- The MPP bring-up is a *byte-for-byte ioctl replay* of a trace from `app.out`, because
  the only findable SDK mismatches the device's. That means every struct is a magic byte
  array with an offset comment. It works, and it is unavoidable, but it's the part of the
  codebase that cannot be refactored casually — `docs/MPP_INIT_SEQUENCE.md` is the spec.
- Playback (`play_file`) runs a nested event loop that calls `ctl_poll()`, `ui_*_poll()`
  and `pump_encode()` from inside itself so recording and control keep working while a
  clip plays. It's the right answer for a single-threaded freestanding program, but it
  means "the main loop" exists in two places and they must stay in sync.

## 2. Defects found (and fixed in this pass)

| # | Where | What |
|---|---|---|
| 1 | `dvr.c apply_cfg` | Config was read into a **600-byte** stack buffer, so every key past roughly line 15 of a commented `dvr.conf` was silently ignored — no error, no log line. A documented config file lost most of its settings. Now a 4 KB static buffer; verified by setting `qp=` at byte ~1500 and watching `INFO` report it. |
| 2 | `webapp2/dvr/telnet.js` | `exec()` resolved with partial output on timeout. The slow command's output then arrived later and was read as the *next* command's reply, so every subsequent `exec()` returned the previous one's text — permanently, silently. Now a timeout poisons the session and drops the socket so the owner reconnects. |
| 3 | `webapp2/server.js` | Opened a **new TCP connection per control command**. The device accepts `MAXCTL`(4) control clients; a couple of browser tabs polling every 3 s could starve it, and every command paid a connect plus a 200 ms quiet-timeout. Replaced with one persistent, queued connection (`dvr/control.js`). |
| 4 | `webapp2` live grid | Streamed **all four channels** although the firmware encodes only `enc_channels` (default 1). Three cells sat forever waiting for a keyframe while holding device client slots. The UI now learns `enc` from `INFO` and marks the rest "NOT ENCODED"; the server refuses those subscriptions with WS code 4404. |
| 5 | file transfer (`deploy.js`, `putFile`) | Two independent silent-truncation traps: `/tmp` is on the **12.5 MB RAM disk with ~1.9 MB free**, and busybox 1.1.2 `nc` stops accepting data around 2.5 MB per connection. A 5 MB image "transferred successfully" as 1.5 MB. Now staged next to the destination on SATA and sent in 1 MB chunks with a size check per chunk. |
| 6 | `putFile` | A backgrounded `nc -l` inherits its shell's stdin, so it forwards the *next telnet command* to the peer — and after we half-close, that write kills nc mid-transfer. Redirecting from `/dev/null` doesn't help (busybox nc treats stdin EOF as done). Fixed by giving the listener its own throwaway telnet session. |
| 7 | docs | `CONTROL_PROTOCOL.md` listed a command set several features out of date, `PROTOCOL2.md` said `MAXCLI` 4 (it's 12), `dvr.conf.example` documented `bitrate=2048 width=352 gop=30` when the real defaults are `12288 / 704 / 15` with FIXQP QP10. All corrected. |

## 2b. The live-video freeze — **ROOT-CAUSED AND FIXED 2026-07-25**

> **Cause: we brought up all four VI channels but only ever drained one.** The three idle
> channels captured into the shared VB pool (8 large blocks, `VBCONF`) and nothing consumed
> their frames, so the pool ran dry and VI could no longer get a buffer for the channel that
> mattered — starving the display and the encoder together.
>
> **Fix:** only enable/start the VI channels something actually consumes — the displayed one
> plus the encoded ones (`g_vi_all`, `dvr.conf vi_all=1` restores the old behaviour). This
> costs nothing: switching channels already restarts the pipeline.
>
> **Evidence.** With all four up, the stall appeared within ~5 minutes on every run and
> recurred (4 stalls in 10 min, 3 in 9 min), sometimes self-recovering after 2-4 minutes,
> sometimes not. With only the consumed channels up: **0 stalls in 14 minutes**, `packs`
> climbing steadily at `pps=36` throughout, live video confirmed moving by pixel comparison,
> and channel switching still works.
>
> Ruled out along the way, each by A/B test rather than argument: our 0.5 Hz GetScreenFrame
> probe (`vo_watchdog=0` — stalls persisted), our front-panel polling (`panel=0` — stalls
> persisted), and the analog signal (TW2866 stayed locked throughout, see the table below).

### What it looked like, and what was true during a stall

The frozen display and a dead encoder are **the same event**: the SoC's **VI stops
delivering captured frames**, which starves the VO video layer and VENC simultaneously.
Measured during a live stall:

| evidence | reading | means |
|---|---|---|
| TW2866 reg `0x00` | `0x68` — VDLOSS=0, HLOCK=1, SLOCK=1, VLOCK=1, NTSC | capture chip is **locked**; the analog signal is fine |
| TW2866 reg `0xFD` | `1` | channel 0 video present |
| `INFO packs=` | frozen | VENC gets nothing (`GetStream` → `0xa006800e` BUF_EMPTY) |
| two screen captures 6 s apart | 0.03 % of pixels differ (just the OSD clock) | the **display really is frozen** |
| `INFO still=` | `0` throughout | **our freeze detector said everything was fine** |

### Three things that are now known to be false

1. **`vo_frame_sig()` is not a liveness signal.** GetScreenFrame's buffer address and frame
   timestamp keep changing while the video layer is demonstrably frozen — proven by the
   pixel comparison above happening at the same moment `still=0`. That is why
   `ensure_live()` never caught this in the first place, and it invalidates the detector in
   §2c below. **`pps=` (encoder throughput) is the reliable signal** — when VI stalls, the
   encoder stalls, and that is measurable and unambiguous.
2. **VO-level repairs do not fix it.** `RELIVE` and `RELIVE HARD` (VO channel cycle, and
   unbind/rebind of VI→VO) were both fired at a live stall and neither restarted the flow.
   The fault is upstream of VO.
3. **A process restart does not fix it and can make it much worse.** Our "restart" is only a
   respawn; the MPP drivers keep their state, so the new instance inherits the wedged
   pipeline and comes up emitting two frames then permanent BUF_EMPTY. An automatic restart
   on stall therefore produced an endless restart loop — strictly worse than the stall.
   **Neither watchdog is allowed to restart anything any more.** Only a real kernel reboot
   clears MPP.

### Character of the fault

Transient: it stalls for roughly 2–4 minutes and then resumes on its own (observed twice —
`packs` frozen for 3.5 min, and again for ~2 min). So the user's "cycling channels fixed
it" may partly be coincidence with the self-recovery. It appears within a couple of minutes
of a fresh start and is easy to reproduce by leaving the box running.

### Recovery, once wedged

Nothing short of a kernel reboot revives it — worth knowing if it ever returns.
`VIKICK`/`VIKICK HARD` (unbind VI→VO, re-run select → SetChnAttr → EnableChn → framerate,
re-bind; `HARD` also tries `0x4905` as a guess at VI DisableChn) was fired at four separate
live stalls: every ioctl returned 0 and the encoder never restarted. `RELIVE`/`RELIVE HARD`
likewise did nothing, which is what pointed upstream of VO in the first place. Those
commands are kept for diagnosis, not as a cure.

Both watchdogs are therefore **detect-and-report only, and must stay that way** — see the
restart-loop warning above. The real fix is not to exhaust the pool.

## 2c. First (partly incorrect) analysis of the freeze — kept for the reasoning

**Symptom.** The VGA video layer latches on one frame. Everything else keeps working — OSD
clock ticks, menus respond, control port answers. Restarting did not help; playback did not
help (once it came back frozen, once black); cycling through all four channels and back
did fix it.

**Why restarting "didn't help".** Both the OSD *Reboot* item and a channel switch only set
`g_restart`, which exits our process so `boot.sh` respawns it. **The MPP drivers stay
loaded and keep their state across that** — it is not a kernel reboot. So the frozen VI→VO
binding survived every "reboot" the user tried. Cycling channels worked because it forced
the VO onto a *different* channel and back, which re-latched the binding.

**Why it was never caught automatically.** `ensure_live()` already knew how to detect this
(`vo_frame_sig()` hashes the displayed buffer address + frame timestamp from VO
GetScreenFrame — both advance on a live layer, both freeze on a latched one) and how to
repair it. It just **only ever ran once, at startup**. Nothing watched for a freeze that
developed later.

**Fix.** `video_watchdog()`, called at 1 Hz from the main loop:

- samples `vo_frame_sig()` once per second — against 25-30 fps video, two consecutive
  samples must differ, so N identical samples in a row is unambiguous (4 s of stillness);
- deliberately not pixel-based: a dark static scene looks identical frame-to-frame even
  when perfectly live;
- skipped while playback owns the VO, and an unreadable signature never counts as frozen;
- then escalates: `vo_relive()` → `vo_force_relive()` → restart the pipeline.

`vo_force_relive()` is new and is the part that actually matters: `vo_relive()` only ever
*binds* VI to VO, and a bind on an already-bound channel is a no-op — precisely the state a
latched display is in. The new one **unbinds first** (ioctl `0x40084919` on both VO devs,
the same direction `play_file()` uses to hand the VO to VDEC) and then re-sources. Both
paths were tested live on the board and the display survives them.

**Telling the two failure modes apart.** `INFO` now reports `pps=` (encoder frames/s),
`still=` (seconds the video layer has not advanced), `heals=` and `froze=`. If the display
freezes with `pps` still ~30 it is display-side; `pps` dropping to 0 means capture died and
needs a different fix. `VOSIG` samples the signature by hand, `RELIVE [HARD]` forces a heal.

## 2d. Recording-integrity audit — 2026-07-25

A pass over everything that touches recording or live video, firmware and web app. Each
finding was reproduced on the device before it was fixed and re-measured after; the
numbers below are from the bench, not from reading the code.

**A silent stop is worse than a loud failure.** Switching the VGA channel (`SHOW`, the
Monitor tab, the OSD Channel item) or the video standard applies by respawning the
process. The respawn is *clean* — `rec_stop()` flushes on the way out — and that is
exactly what made it dangerous: `rec=1000` became `rec=0000` with nothing said, so an
operator keeps flying believing they are still capturing. Measured: uptime 74 → 11 on one
`SHOW`. Both are now refused with `ERR recording` inside `switch_channel()` and the STD
handler, so the control port, the OSD and the web UI are covered by one guard. The web
buttons also no longer light up before the device has agreed.

**Playback while recording cost 31% of the recorded frames** — 20.8 fps against 29.97.
It looked like VDEC/VENC contention for the shared VEDU, which would have meant refusing
the combination. It was not: the playback loop drained the encoder only every 4th pacing
pass and every 8th decoded frame (~266 ms), so the VENC ring overflowed while we sat
waiting to present. Draining every pass restores 28.8 fps and playback is still smooth,
so the feature survives. Thumbnail generation was measured on the same suspicion and was
already fine (30.1 fps) — worth recording, because "it must be the shared codec" was
wrong twice.

**`ts_flush()` ignored short writes.** The 64 KB buffered write that replaced 188-byte
writes is far likelier to come up short, and dropping the remainder corrupts the stream
precisely where it hurts most — a disk filling up mid-flight. It now loops, and latches
the error so the recorder stops instead of producing an unplayable file.

**webapp2's control link could desync permanently.** Replies are matched *positionally*;
a timeout resolved `null` and moved on, so a late reply landed on the next command's slot
and every reply after was off by one. `PLAY` makes that reachable rather than theoretical
— it does not answer until playback *ends*. Reproduced: an `INFO` came back as
`"PLAY err"`. Timeouts are now fatal to the link (same rule, and same reason, as
`dvr/telnet.js`), and `PLAY` goes out on its own socket. 160 probes, 0 mismatches after.

**The live WebSocket had no backpressure.** A stalled browser does not slow the device
down — the proxy keeps reading — so `ws.send()` queued ~3 MB/s of heap per stuck viewer
until the server died, taking everyone's live view with it. Now bounded, dropping and
resuming at the next SPS so the decoder restarts cleanly rather than chewing a headless GOP.

**Soak, after the fixes:** 60.7 s recording with a live viewer attached throughout — 1739
frames, file duration 60.93 s (timeline matches the wall clock), 25.9 Mbps streamed
concurrently, 0 stalls, 0 heals, 0 freezes. The residual ~4% frame cost is what an
attached live viewer costs, and the encoder-clock timestamps report it honestly as gaps
rather than hiding it as speed-up.

## 3. Real risks that are still open

**R1 — Runtime state lives on a volatile filesystem.** `dvr.c` reads `/root/rec/dvr.conf`,
`/root/rec/std` and `/root/rec/dispch`. `/root/rec` is a directory on the **RAM disk**;
only `a1..a4` beneath it are SATA. So the video standard, the displayed channel and the
whole encoder config are lost on every power cycle. `flash/sata/boot.sh` papers over this
by copying the SATA copies into place at boot and copying them back after each run, but
the device should just read `/root/rec/a1/dvr.conf` directly and fall back to the RAM path.
Small change, removes a whole class of "why did my setting vanish".

**R2 — No retention policy for recordings.** There is a per-file size cap (`max_rec_mb`)
and a low-disk auto-stop (`min_free_mb`), and that's it. When the 28 GB partition fills,
recording *stops*. For a flight recorder the correct behaviour is the opposite: keep
recording and drop the oldest clip. At ~17 Mbps a partition holds about 3.5 hours, so this
is a real limit, not a theoretical one. `list_recordings()` already sorts by timestamp, so
"delete oldest until free ≥ `min_free_mb`" is a dozen lines.

**R3 — ~~Recording timestamps are synthesised.~~ RESOLVED 2026-07-25.** This was worse in
practice than the entry predicted: frames do *not* flow at a steady 30 fps whenever
recording is active (see R10), so every clip was stamped faster than it was captured and
played back short. An 8.0 s recording reported **5.13 s**. `ts_write()` now takes an
explicit PTS and `pump_encode()` supplies the VENC pack's own stamp — `pack[6..7]` is a u64
**microsecond** capture time, confirmed stepping 33367 µs (29.97 fps) between consecutive
frames. The same 8.0 s window now reports **8.64 s** with `tbr 29.97`. Dropped frames are
now honest gaps rather than a silent speed-up.

**R3b — ~~STOP truncated the tail of every recording.~~ RESOLVED 2026-07-25.** `rec_stop()`
ran at the *top* of `pump_encode()`, so the GetStream immediately below handed over the
frames still queued in the encoder — frames captured *before* the stop — with `rec_on`
already 0. They went to live viewers and never to disk. STOP now arms a drain that keeps
writing until the FIFO reports empty, bounded by `REC_DRAIN_MS`. Because the queue is
essentially never empty on a single poll at this load, the drain pumps the channel in a
tight inner loop rather than once per tick. Net effect on an 8 s recording: 154 → 205
frames. It now errs ~0.5 s *long* rather than ~1 s short, which is the right way round for
a flight recorder.

**R4 — Channel switching restarts the process.** `switch_channel()` persists the choice and
sets `g_restart`, costing a few seconds of black screen, with a comment saying a live
re-latch "needs more VO RE". That comment predates `vo_relive()`, which was written for the
playback-return path and does exactly the required dance (disable → `SetChnAttr` → enable on
both VO devs, then re-bind VI). Switching live is probably: re-select the VO channel on
`g_fvo`, `vo_relive(new)`, `ensure_live(new)`, and fall back to the restart if
`vo_frame_sig()` says it didn't take. Needs someone in front of the monitor.

**R5 — 32-bit counters.** `total_bytes` is `unsigned long` (32-bit here) and wraps at 4 GB;
it's only used for a log line, but `INFO packs=` shares the habit. Cosmetic until it isn't.

**R6 — `rec_start()` checks free space on `a1` only** while writing to `aN`. Today
`enc_channels=1` so it's always a1; it becomes wrong the moment multi-channel recording is
enabled.

**R7 — ~~The cold-boot path has never been exercised.~~ RESOLVED 2026-07-25.** After the
reflash `dvr` *is* the first thing to touch the hardware, and the MPP bring-up works
cold — no `app.out` warm-up needed. What the cold boot did expose was two things `app.out`
had silently been doing for us: setting the IP (from the mtd0 config blob, hence .108
rather than `net.sh`'s .114) and owning the serial console (so a getty was never needed).
Both now handled in `boot.sh`. **If anything else turns up, suspect the same class of
cause**: something the vendor app did on our behalf.

**R8 — ~~No serial console.~~ RESOLVED.** COM6 works; it gives a root shell and CTRL-C
during boot lands at the U-Boot prompt (`hilinux #`). Manual `bootcmd` replay boots Linux.

**R9 — U-Boot recovery is available but unrehearsed.** U-Boot has `tftp`/`erase`/`protect`/
`cp`/`cmp`, but **no `loadb`/`loady`** — so TFTP is the only way to get an image in, and
inbound UDP 69 is firewall-blocked on the PC. `py -3 tools/dvr.py recover --test` will
rehearse the whole chain without erasing anything, but it needs the rule added first
(one `netsh` line, in `docs/FLASH.md`). Until then the last-resort path is theory. It is
also the *only* path that matters if a future flash produces an unbootable rootfs — the
day-to-day fallbacks (`stock`, `noboot`, `flashback`, `restore`) all assume Linux runs.

**R10 — ~~Recording at the default `qp=10` drops ~20% of frames.~~ RESOLVED 2026-07-25.**
The recorder writes to vfat *synchronously, inside the loop that drains the encoder*
(`pump_encode`). The per-frame cost exceeded the 33 ms budget, the VENC FIFO overflowed,
and frames were lost before we ever saw them. The live stream stayed at full rate because
a socket write is cheap — which is exactly why this hid for so long: the monitor and the
browser look perfect while the file quietly gets less.

Found by recording and capturing `:8091` over the *same* window: live received 188 frames
where the file got 80, at identical bytes-per-frame. Same encoder, two sinks — so the
frames that landed were intact, there were simply fewer of them.

The instinct was to move disk I/O off the drain path (a ring + a writer). That turned out
to be unnecessary: the path was not I/O-bound at all, it was **CPU-bound on three avoidable
costs**, all in code nobody thought of as hot.

1. **Every 188-byte TS packet was its own `sys_write`** — ~520 syscalls per frame, ~15k/s,
   each traversing vfat. Now buffered into 64 KB blocks (`TS_OBUF`): ~2 writes per frame.
2. **`memcpy`/`memset` were byte-at-a-time loops.** At ~5 cycles/byte on this ARM926 and
   ~96 KB copied per frame, that alone was milliseconds of a 33 ms budget. Now word-wide
   when alignment allows (ARM926 cannot do unaligned word access, so same-alignment is
   required; byte fallback otherwise).
3. **Each packet was assembled in a local `pkt[188]` and then copied into the buffer** — a
   second full pass over every recorded byte, for a packet written exactly once. Packets
   are now built in place in the output buffer via `ts_slot()`.

Result at `qp=10` / ~26 Mbps, NTSC 704×240, measured the same way:

| | written | captured | notes |
|---|---|---|---|
| before | 23.7 fps | 29.97 fps | 8.0 s window → file said 5.13 s |
| after | **29.97 fps** | 29.97 fps | 8.0 s window → file says 7.97 s |

Sustained 15 s with a live viewer attached at the same time: 470 recorded frames at
29.97 fps *and* 525 live frames, concurrently, and a clean full decode with zero errors.

Worth remembering as a general lesson for this codebase: on a 200 MHz ARM9 with no libc,
the naive freestanding helpers are not free, and "obviously not the bottleneck" code on a
path that runs 15,000 times a second is exactly where the budget went.

## 4. What's worth building next

Ranked by value per unit of risk.

### P0 — makes the thing a better flight recorder

1. **Circular retention** (R2). Keep recording forever; delete the oldest clip when free
   space drops below `min_free_mb`. Add `retain_mb`/`keep_min_clips` to `dvr.conf`.
2. **Pre-roll buffer.** Keep the last N seconds of encoded GOPs in a RAM ring (a 4 MB ring
   ≈ 2 s at QP10, or ~8 s at a lower quality) and flush it into the file when recording
   starts. On an FPV field you always press record *after* the interesting thing starts.
   The encoder output already passes through one place (`pump_encode`), so this is a ring
   plus a "write the ring first" branch in `rec_start`.
3. **Auto-record.** `autorec=1` in `dvr.conf` → start recording as soon as video is present
   on the encoded channel (the TW2866 already reports per-channel presence in reg `0xFD`,
   `resolve_standard()` reads it). Powering the box on should be enough to capture a flight.
4. **Config from SATA** (R1).

### P1 — closes obvious gaps

5. **Instant channel switch** (R4).
6. **Hardware PTS** (R3).
7. **`CONF` control command** — get/set individual keys over port 8090 with persistence,
   so the web UI's config editor stops needing telnet at all. The control plane already
   covers everything else; `dvr.conf` is the last telnet dependency for normal operation
   (Files and Shell will always need it, by definition).
8. **Finish the MCU path.** `/dev/ttyAMA1` command handling is implemented and never
   tested against real hardware; the physical REC switch is the whole point of the front
   panel. Needs a loopback test at minimum, then the MCU.
9. **Web UI: disk + timeline.** A capacity bar (free/used/estimated recording time at the
   current bitrate) and a day-timeline for clips. `INFO` already returns `disk` and the
   encoder settings, so the estimate is free.

### P2 — quality of life

10. **Split `dvr.c`** along the seams `DVR_DESIGN.md` already names. The nested playback
    loop is the tricky part; extract `poll_all()` (control + mouse + keyboard + encoder)
    first so both loops call one function.
11. **Snapshot to the web UI.** `SHOT` already grabs the VO frame to `/root/rec/shot.yuv`;
    converting it to a JPEG the browser can show is a small addition and makes "what is the
    DVR showing right now" answerable without a monitor.
12. **Clip trimming.** The server already remuxes TS→MP4 with ffmpeg; `-ss`/`-t` on the
    same path gives cut-and-download without re-encoding.
13. **Multi-select delete** in the Clips tab (the `DEL` command exists, one file at a time).

### P3 — only if the itch demands it

14. Multi-channel recording at reduced width (the pipeline supports it; `enc_channels`
    already exists — it needs bandwidth measurements, not code).
15. A second stream profile (low-bitrate) for remote viewing over a phone hotspot.

## 4b. Ground-station roadmap — what this box could actually become

The backlog above is "finish what's started". This section is the other question: given
*this* hardware, what would make it a genuinely good FPV ground station rather than a CCTV
DVR that records flights? Ranked by value ÷ effort, with the hardware asset each one uses.

Hardware we have and have already proven: **4 composite inputs with per-channel
video-present detection** (TW2866 reg `0xFD`) and per-channel picture control, H.264
encode **and** decode, a VGA output with two graphics overlay layers, 117 GB SATA, USB host
(mouse + keyboard working; `usb_storage`/`vfat` already loaded), 100 M Ethernet, a
battery-backed RTC, a hardware watchdog, **a spare UART on `/dev/ttyAMA1`** (the rear
PTZ/MCU terminal, likely RS-485), and audio inputs on the TW2866 that we have never touched.

> **Recording stays manual — decided 2026-07-25.** Start and stop happen on command
> (`REC`/`STOP`, the web UI, the OSD, and soon a front-panel button) and nothing else. The
> whole reason for writing our own firmware was to *not* rebuild a CCTV DVR, so
> **auto-record on video-present and circular retention are declined**, not deferred. The
> low-disk guard stays (it prevents a full disk, it doesn't start anything). Anything below
> that implies the box deciding on its own when to record is out of scope.

### The two that change how it feels to use

1. **Signal-quality metering + a dropout log.** Analog FPV *is* signal quality. Sample the
   TW2866 sync/presence status continuously, draw a live bar on the OSD, log every dropout
   with a timestamp, and mark them on the clip timeline in the web UI so you can jump
   straight to "where I lost video". No off-the-shelf DVR does this, and the data is
   already sitting in a register we read at boot.
2. **Pre-roll buffer** *(optional — check before building)*. Keep the last N seconds of
   encoded GOPs in a RAM ring and flush it into the file when recording starts, so a manual
   press also captures the seconds before it. It does not change the manual model, but it
   is the kind of automation that was just ruled out, so ask first.

### The two that exploit the 4 inputs (this box's unfair advantage)

4. **Receiver diversity.** Two or more VRX modules on separate channels; the DVR
   continuously scores each input and switches the displayed *and recorded* channel to
   whichever is cleanest. That is a real diversity system assembled from capture inputs.
   Blocked on the instant channel switch (R4) — which `vo_relive()` has already mostly
   solved — plus the scoring from item 3.
5. **Quad view / picture-in-picture.** The stock app did 2×2 on the VO; we do 1-up. For a
   spectator or marshal station, seeing four pilots at once is the whole point. The VO
   supports multiple channel windows; it's layout work, not new plumbing.

### The two that make it a *ground station* rather than a recorder

6. **Telemetry in on `/dev/ttyAMA1`** — MAVLink / CRSF / whatever the link speaks. Overlay
   pack voltage, RSSI/LQ, altitude, speed, distance and GPS on the screen, and record it
   alongside the video so a clip can be replayed with its telemetry. The UART is already
   open and already carries our control protocol, so the transport is done.
7. **Antenna tracker output** on that same RS-485/PTZ terminal. The board was *designed* to
   drive PTZ heads; with GPS telemetry from item 6 you get a tracker essentially for free.
   This is the most "why didn't anyone do this" idea in the list.

### Field ergonomics — no laptop at the flying field

8. **Serve the UI from the device.** Today the browser needs the PC-hosted Node backend.
   Add an RFC 6455 handshake + framing to the existing TCP server (~150 lines on top of
   `net.h`) and serve the static files from the busybox httpd already running on :8081 —
   then a phone on the same network opens the DVR's IP and gets live video and controls
   directly. This is the highest-value item for actual field use.
9. **Export to USB from the OSD menu.** `usb_storage`, `vfat` and `sd_mod` are loaded and
   the menu system exists. "Copy last flight to the stick" is a menu item and a `cp`.
10. **Audible alerts.** Video lost, disk nearly full, recording started/stopped. Needs a
    GPIO/buzzer probe first — CCTV boards nearly always have one, we just haven't looked.

### Smaller, cheap, worth doing while nearby

11. **Bookmarks.** A button press drops a timestamped marker into the clip; the web UI
    lists them. "Mark that" mid-flight.
12. **Audio.** The TW2866 has audio ADCs we've never enabled — commentary or motor sound.
13. **Stills.** `SHOT` already grabs the VO frame; encoding it to JPEG makes "what is the
    DVR showing" answerable from the web UI, and gives a photo mode.
14. **Clip trimming on download** — the server already remuxes TS→MP4; `-ss`/`-t` on the
    same path is cut-and-download with no re-encode.
15. **Config profiles** — "flight" (QP10, pre-roll on) vs "long session" (VBR, retention on).

### Deliberately not worth it

- **Wi-Fi.** A USB dongle needs a driver for kernel 2.6.24. That road is all pain.
- **On-device transcoding** for phones. The VEDU is busy; let the browser decode.
- **4-channel simultaneous recording at full width.** The VIU can't sustain it; that's why
  `enc_channels` defaults to 1. Multi-channel means dropping resolution, which for a
  single-camera FPV recorder is the wrong trade.

## 5. What is genuinely good and should not be "improved"

- **The ioctl-replay bring-up.** It looks awful and it is exactly right: the SDK doesn't
  match the silicon, and the trace does. Don't refactor it into pretty structs.
- **Drop-on-slow streaming.** `cli_send()` gives a slow viewer ~6 ms of retry budget and
  then drops it. Recording latency is never held hostage to a browser. Keep that priority.
- **The keyframe-gated record start / ungated stop** in `pump_encode()`. Every file begins
  with SPS/PPS/IDR (so every clip is independently decodable and instantly seekable), but
  STOP doesn't wait for a keyframe that may never come. That asymmetry is deliberate and
  was a bug fix; the comment explains it, leave it.
- **`ensure_live()` / `vo_frame_sig()`.** Verifying the VGA output is *actually advancing*
  by watching the buffer address and frame timestamp — rather than trusting an ioctl return
  code or comparing pixels — is the kind of thing that only comes from being burned. It
  self-heals the intermittent frozen-first-frame bug.
- **The one-way flash design.** Putting everything mutable on SATA and making the flash
  change a single `exec` hook means this project needs exactly one reflash, ever.
