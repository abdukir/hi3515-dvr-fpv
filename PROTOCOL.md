# RR104P / TLNetDvr 8670 protocol (reverse-engineered)

DVR at `192.168.1.108`. Control **and** media both use TCP port **8670**
(the `MediaPort=9004` in the web JS is not actually used by this build — media
opens additional 8670 connections). Login user `admin`, password `000000`.

## Connection preamble (every connection, client's first bytes)
16-byte session magic, then 1 byte conn-type, then type-specific bytes:
```
61 78 da b5 d3 8e 43 db 9e d7 f2 20 78 36 18 79   <- fixed magic (constant)
01                                                 <- 0x01 constant
<TT>                                               <- 01 = control, 02 = media
...                                                <- type params (see below)
```
- **Control:** `...01 01 00 00` (20-byte preamble) then framed messages.
- **Media (StartRealPlay):** `...01 02 00 00 00 01 00 00...` padded to 100 bytes.
  The `00 00 00 01` after `01 02` selects channel/stream (needs confirming per
  channel + main/sub; capture more streams to map exactly).

## Control message frame (after the 20-byte preamble)
```
[len u32be][type u16be][cmd u16be][seq u16be][0x0100][payload...]
len   = total message length incl. this 12-byte header
type  = 0x0000 request(C->S) | 0x0002 reply(S->C) | 0x0001 event(S->C)
cmd   = command (requests & events; replies carry 0x0000 - match by seq)
seq   = sequence, client increments; reply echoes it
```

### Known commands (cmd, C->S request → S->C reply payload)
| cmd | name | reply payload |
|-----|------|---------------|
| 0x2711 | **Login** | req: `admin\0`(padded) + pass `000000` + MAC + clientIP(4). reply: `01 00 00 00` + user + rights-string (e.g. `1111...`) |
| 0x2713 | **GetDeviceInfo** | IP(4) `c0a8016c`, port(2) `21de`, name `NetDVR`, model `RR104P`, chan count `04`... |
| 0x2714 | video/VGA res | `62 02 c0 02 40 00 00 00` |
| 0x2715 | ? | `00 10 1f 40 02 82 00 28 ...` |
| 0x2716 | ? | `10 1f 40 02 82 00 ...` |
| 0x272c | net config | MAC ascii + IP `c0a8016c` port `21de` mask `ffffff00` gw `c0a80101` |
| 0x272d | substream/record params | `05 00 04 00 3c ...` |
| 0x274d | user/rights table | 468-byte struct (users + 17-char rights each) |
| 0x2763 | ? | `0a 00 01 03 04 05 06 07 08 09 0a ...` |
| 0x277b | full config blob | ~4KB |
| 0x275c | (req has 1-byte `01`) | ack |
| 0x4e27 | **event: channel status** (S->C push) | `02 01 0<ch>` per channel |

Login handshake order the OCX uses: 2713,2714,2715,2716,**2711(login)**,275c,
272c,2763,272a,2753,272d,277b.

## Media stream container (S->C on a media connection)
```
[8-byte stream prefix once: 00 00 00 00 00 00 00 09]
then per frame:
  [11-byte packet header, starts 0x62 0x19]
  [12-byte frame header: LEN u32be | FRAMENO u32be | TIMESTAMP u32be]
  [LEN bytes: Annex-B H.264]  (starts 00 00 00 01, real NALs)
```
- FRAMENO increments 1,2,3,...; TIMESTAMP = ms clock, +33/frame = 30 fps.
- Payload is clean H.264: I-frame = SPS(7)/PPS(8)/SEI(6)/IDR(5), P=slice(1).
  Baseline/High profile. Sub-stream = **352x240** (CIF NTSC); main = D1.
- De-framing (see `deframe.py`): strip 8-byte prefix; per frame skip 11-byte
  packet header + 12-byte frame header, emit LEN payload bytes. Resync on `62 19`.
- Result decodes in ffmpeg with zero errors -> `deframe.py` + ffmpeg is the
  proven live pipeline for the web UI (feed to fMP4/MSE over WebSocket).

## Full command map (from NetDvr2.dll disasm + capture)
Sender helper `0x10012250`: `send(session, cmd, reqPtr, reqLen, respBuf, respSize, g)`.
All commands are control frames on 8670 (type 0 req → type 2 reply, matched by seq).

| cmd | function | req | reply / notes |
|-----|----------|-----|---------------|
| 0x2711 | **Login** | 46B user+pass+mac+ip | 4B result(1=ok)+user+rights |
| 0x2712 | logout | 46B | — |
| 0x2713 | GetDeviceInfo | 0 | IP,port,name,model,chans |
| 0x2714 | GetVideoProperty | 0 | 8B |
| 0x2718 | **StartRealPlay** | 4B stream-id | (media flows on separate 8670 conn; our direct media handshake bypasses this) |
| 0x2719 | Open/CloseRealAudio | 4B | |
| 0x271b | **startFileDownload** | 4B | file download stream |
| 0x272a/b | get/setNetworkParams | 0/391 | |
| 0x272c | GetVGAsolution | 0 | 80B (resolutions) |
| 0x272d/e | get/setSystemParams | 0/61 | |
| 0x272f/30 | get/setRecordParams | 1B ch / 38B | record cfg per ch |
| 0x2731/2 | get/setSubStreamParam | 1B / 34B | |
| 0x2733/4 | get/setVideoParams | 1B ch / — | reply 97B: name"CH 01"+res+fps+bitrate |
| 0x273d/e | get/setRecordSCH | 2B / — | record schedule |
| 0x273f/40 | get/setMotionDetection | 1B / — | |
| 0x2751/2 | get/setSystemTime | 0 / 10B | 4B unix epoch |
| 0x2754 | **shutdown** · 0x2755 **reboot** | 0 | |
| 0x2756 | **PtzControl** | 3B `[ch, dir, speed]` dir=0 stop | |
| 0x2757 | **GetRecordState** | 0 | **4B, one byte per channel (0=off)** |
| 0x2758 | **SetRecordState** | **4B per-channel** | record start/stop → read-modify-write |
| 0x275a | **recFilesSearch** | 20B `[ch(2) startT(4) ? endT(4) type(2) ...]` | file list |
| 0x276d.. | startPlayByTime/playSeek/Next/fast/slow/mute | playback controls |
| 0x2779 | startTimeDownload | 4B | download by time range |
| 0x274d/e | Get/Add/Edit user | 0 / 58B | |
| 0x2781 | MakeKeyFrame | 8B | force IDR |

**Record start/stop (implemented):** GET 0x2757 → 4 bytes; set byte[channel]=1(on)/0(off); SET 0x2758 with the 4 bytes. Format-agnostic read-modify-write.
**PtzControl:** `[channel, direction, speed]`, direction 0 = stop (seen: 03=one dir, 01=another).

## Server-side dispatch (from `app.out` RE — authoritative)

The tables above came from the Windows client DLL. The **device** side is now decoded from `app.out`
(see `docs/FIRMWARE_RE.md` §7, `dump/ghidra/dispatch_8670.json`). The 8670 control thread invokes a
registered handler `FUN_00059840` (@0x59840) — a binary-search dispatch on the u16 cmd. Its **36
commands** and in-function handler addresses, with semantics recovered from log strings / callees:

| cmd | @handler | meaning (from app.out) |
|---|---|---|
| 0x2711 | 0x598dc | **Login** — checks user/pass vs config user table (`cfg_get_user`) |
| 0x2712 | 0x5e87c | **Logout** |
| 0x2715 | 0x5be8c | get (no log; reads config) — TBD |
| 0x271c | 0x5bd1c | get — TBD |
| 0x271e | 0x5e188 | **Playback: fast** (`remote fast play id:0x%x`) |
| 0x271f | 0x5ee58 | **Playback: slow** |
| 0x2720 | 0x5a424 | TBD |
| 0x2721 | 0x5cf80 | **Playback: pause** |
| 0x2727 | 0x5a804 | **Playback: seek** |
| 0x2729 | 0x5e4c8 | **Playback: progress/goto** |
| 0x272a | 0x5ec18 | net-related (`FUN_0277bc`) |
| 0x272b | 0x59d64 | **Set network** (`set network`) |
| 0x272c | 0x5d2f0 | TBD (client DLL: GetVGAsolution) |
| 0x2732 | 0x5b900 | substream/video (`FUN_02b078`) |
| 0x2738 | 0x5d010 | TBD (`sizeof=%d`) |
| 0x2739 | 0x5fc74 | `FUN_02bb48` |
| 0x273a | 0x5a254 | TBD |
| 0x273b | 0x5e13c | **Get alarm-notify param** |
| 0x273e | 0x59f04 | **Set record schedule** (`set rec sch param`, `copy2Weekday`) |
| 0x2741 | 0x5ef3c | **Email/alarm config** (`flag_email`) |
| 0x2742 | 0x59bb8 | TBD |
| 0x2743 | 0x5ce7c | `FUN_02e96c`,`FUN_02a2d4` |
| 0x2749 | 0x5bbcc | uses `FUN_02ed60` (shared w/ 0x274a,0x274b) |
| 0x274a | 0x5fea8 | uses `FUN_02ed60` |
| 0x274b | 0x5a65c | uses `FUN_02ed60` |
| 0x274c | 0x5d540 | TBD |
| 0x274f | 0x5ce18 | **Playback: resume** (`resume cmd got`) |
| 0x2755 | 0x5a9a4 | **Remote reboot** (`remote reboot!`) |
| 0x2757 | 0x5df08 | **GetRecordState** → `FUN_058880`: u32 **BE bitmask**, bit N = ch N recording |
| 0x2758 | 0x5eef0 | **SetRecordState** → `FUN_058918`: apply bitmask (lock-guarded) |
| 0x2759 | 0x59e34 | TBD |
| 0x275a | 0x5c908 | **recFilesSearch** |
| 0x2771 | 0x5b7d8 | per-channel (`chn=%d`) |
| 0x4e15 | 0x5f148 | **CTRL_CMD_DHCP_DECONFIG** |
| 0x4e1c | 0x5a544 | **CTRL_CMD_PPPOE_DISCONNECT** |
| 0x4e1d | 0x5dd78 | **CTRL_CMD_PPPOE_DOWN** |

**Note:** the 8670 control thread `0x774ac` registers exactly ONE command handler (`FUN_00059840`,
from `main` via `FUN_00076974` → global `0x3311ec`) and invokes it for the 36 commands above; the
worker itself additionally handles `0x4e21`→0x782f4 and `0x4e22`→0x78667. Unmatched commands return
0. So the **device's real 0x27xx command codes are the set above** — the earlier client-DLL-derived
numbers (get=X/set=X+1 pairs like 0x2733/4, 0x272f/30, 0x274d/e, 0x2751/2, 0x2756, 0x276d, 0x2781)
do **not** all map 1:1 to the firmware; several device commands are single codes with a sub-field
(e.g. 0x2732 covers video/substream via `FUN_02b078`; the no-log handlers 0x2715/0x271c/0x2720/
0x272c/0x273a/0x2742/0x2759/0x274c are the config *get* replies). Remaining work is decoding each
handler's **payload** (config struct offsets), not finding more commands.

**Correction:** GetRecordState/SetRecordState (0x2757/8) is a **u32 big-endian bitmask** (ch N = bit
N), not one-byte-per-channel — confirmed in `FUN_058880`/`FUN_058918` (matches CLAUDE.md).

## Still to capture/decode
- The other 8670 handler clusters (video-params, record-params, users, PTZ, time, download) — find
  each registered handler and extract its dispatch like `FUN_00059840`.
- Config payload struct offsets per `Set*` command (extends `docs/FIRMWARE_RE.md` §2 struct map).
- Recording file download format (custom .mp4, `rec/a1/fly%05d.mp4`)
