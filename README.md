# ESP32 UWB Pro — TDMA DS-TWR

Port of the initiator-driven multi-tag / multi-anchor protocol from
`dwm1001_tdma_twr` to **Makerfabs ESP32 UWB Pro with Display**
(ESP32-WROVER + DW1000).

**The original `dwm1001_tdma_twr` tree is not modified.** This is a separate
project. The Makerfabs `DW1000` library under `lib/DW1000` is vendored
unmodified from `mf_DW1000.zip`.

## Hardware

| Signal | GPIO |
|--------|------|
| SPI SCK/MISO/MOSI | 18 / 19 / 23 |
| UWB CS / RST / IRQ | 21 / 27 / 34 |
| OLED SDA / SCL | 4 / 5 |

## Build

```bash
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
cd ~/Documents/esp32_uwb_tdma_twr
pio run
```

## Flashing

Each board shows up as `/dev/cu.usbserial-XXXXXXXX` (a CP2104 USB-UART
bridge) when plugged in. List connected boards:

```bash
ls /dev/cu.usbserial-*
```

Flash one board (always pass `--upload-port` explicitly once more than one
board is plugged in — otherwise PlatformIO picks whichever port it finds
first, which is not deterministic):

```bash
pio run -t upload --upload-port /dev/cu.usbserial-XXXXXXXX
```

Reflashing does **not** erase a board's saved config (NVS survives across
flashes) — a board that was already provisioned keeps its role/identity
after a reflash. To watch the boot banner:

```bash
pio device monitor -b 921600 --port /dev/cu.usbserial-XXXXXXXX
```

**If a board goes silent (no banner, no `$RNG` lines) after being flashed,
reset, or after another process that had its port open was killed:** these
boards' auto-reset circuit (DTR/RTS → EN/GPIO0) can leave the ESP32 held in
reset or the ROM bootloader depending on whatever DTR/RTS line state was
left behind by whoever last touched the port. Force a clean reset:

```python
import serial, time
s = serial.Serial('/dev/cu.usbserial-XXXXXXXX', 921600, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setRTS(False); time.sleep(0.1)
```
(`scripts/stream_ranges.py` and `scripts/range_table.py` already do this on
every port they open, so this is only needed for manual/ad-hoc scripting.)

## Provisioning

The serial console uses `$` + payload + `*` + two-hex-digit XOR checksum,
CRLF terminated — e.g. `$SET,role,INIT*0A\r\n`. Any terminal program that can
send raw bytes at 921600 8N1 works; `pio device monitor` does not compute
checksums for you, so scripting is usually easier (see `scripts/` for the
checksum function).

Every board must be told, at minimum, its **role** and its **position in the
schedule**:

```
$RANGE,STOP*..            # required before changing role/SAVE while ranging
$SET,role,INIT*..         # INIT = tag, RESP = anchor, LSTN = passive listener
$SET,frame_index,N*..     # tags only: 0..init_count-1, unique per tag
$SET,slot_index,N*..      # anchors only: 0..resp_count-1, unique per anchor
$SAVE*..
$RESET*..                 # role changes need a reboot to take effect --
                           # $SET/$SAVE alone does not switch which loop
                           # (app_run_initiator/app_run_responder) is running
```

`node_id` self-assigns from the ESP32's factory MAC the first time a board
without a saved config boots, so it does not need to be set manually and is
guaranteed unique across boards.

**`init_count`/`resp_count` must match on every board** (they're part of the
config hash carried in every Poll — a mismatch silently drops that board's
frames and shows up as `hash_mismatch` in `$STAT`). Set them to the actual
number of tags/anchors you're running *right now*, not the eventual target —
the per-tag update rate is `1 / (init_count × frame_duration)`, so leaving
`init_count` at a large eventual-fleet value while bench-testing with two
boards reserves airtime for tags that don't exist and tanks the rate:

```
$SET,init_count,<number of tags>*..
$SET,resp_count,<number of anchors>*..
$AUTOTIME*..               # recompute the slot timing budget for that size
$SAVE*..
$RESET*..
```

Do this on **every** board in the network, including ones you don't
otherwise touch, whenever the fleet size changes — a stale `init_count`
mismatch is the single most common cause of a board silently not
participating.

Useful commands: `$STAT*..` (counters — `tx_late`, `hash_mismatch`,
`ss_fallback`, `sync` state), `$CFG*..` (dump full config), `$DEFAULT*..` +
`$SAVE*..` (wipe to firmware defaults — also requires `$RANGE,STOP` first).

## Defaults

- `init_count=16`, `resp_count=8`, `report_enable=1` (the eventual 16×8
  target — resize for whatever you're actually running, see Provisioning)
- ESP32 slot budget verified on hardware via `$AUTOTIME` (`frame_dur_us=33047`,
  `proc_us=1500` at N_R=8; shrinks a lot at smaller `resp_count`)
- Serial `$RNG` CSV (same framing as DWM1001) for Jetson → MQTT

## Calibration

Ranges are systematically off by a fixed amount until antenna delay is
calibrated — this firmware ships with `ant_delay_tx`/`ant_delay_rx` at the
DWM1001's factory figure (16436), which is not correct for this board's
antenna/PA path. There is no dedicated `$CAL` command; calibration is a
manual measure-and-adjust loop against `ant_delay_tx`/`ant_delay_rx`.

**Direction** (confirmed by the DWM1001 reference's own measured result —
halving one node's antenna delay compensation, instead of applying the full
value to both TX and RX, made a ~1 m true distance read as 77.8 m, and
restoring the full value on both fixed it): **increasing** a node's
`ant_delay` **shortens** the ranges it participates in; decreasing it
lengthens them. The magnitude per DTU is on the order of `DISTANCE_OF_RADIO`
(≈4.69 mm, one DTU ≈ 15.65 ps × speed of light in air) but is not pinned
down exactly for a small single-node adjustment through the full DS-TWR
combination — treat the delta below as a first-order estimate and iterate;
one or two passes converges either way. Both `ant_delay_tx` and
`ant_delay_rx` must always be set to the **same value** on a given board.

**Procedure:**

1. Pick one reference anchor and one reference tag. Place them at a
   precisely measured line-of-sight baseline (tape measure, antennas at the
   same height, no obstructions) — e.g. exactly 5.000 m.
2. `$RANGE,START` on both, let it run a few seconds to settle, then read the
   mean measured range over a stable window:
   ```bash
   ./range_table.py -w 15
   ```
3. Compute the error and the delay correction:
   ```
   error_mm  = measured_mean_mm - true_distance_mm
   delta_dtu = round(error_mm / 4.6918)
   ```
   Positive error (reading too long) → positive `delta_dtu` → **increase**
   the tag's antenna delay. Negative error (reading too short) → decrease it.
4. Apply to the **tag** only (keep the reference anchor fixed as your
   truth reference throughout the whole campaign):
   ```
   $RANGE,STOP*..
   $SET,ant_delay_tx,<old_value + delta_dtu>*..
   $SET,ant_delay_rx,<old_value + delta_dtu>*..
   $SAVE*..
   $RESET*..
   ```
5. Re-measure. One pass is usually enough (the relationship is linear); if
   the residual is still more than a couple of cm, repeat step 3-4 with the
   new residual error.
6. Repeat this whole procedure for every other tag against the **same**
   reference anchor, one at a time. Once every tag is calibrated against it,
   verify each additional anchor against one already-calibrated tag at a
   fresh measured baseline — adjust that anchor's own `ant_delay_tx/rx` only
   if it doesn't already read correctly (antenna delay variance between
   boards of the same design is usually small).

`ant_delay_tx` and `ant_delay_rx` are per-board config (not hashed), so
different boards can carry different calibrated values without affecting
network compatibility.

## Live telemetry (host tools)

```bash
./stream_ranges.py     # merged JSON stream from every connected tag
./range_table.py       # live per-tag/per-anchor rate + mean range table
```

Both auto-discover every `/dev/cu.usbserial-*` port and pick up boards
plugged in after startup (rescans every 2s). Only one process may hold a
given serial port open at a time, so run `stream_ranges.py` first — it owns
the ports and also writes every record to `/tmp/uwb_ranges.jsonl`;
`range_table.py` tails that file by default instead of opening the ports
itself, so both can run at once in separate terminals. Pass `--serial` to
`range_table.py` to have it open the ports directly instead (only when
`stream_ranges.py` is not already running).

`tag_label`/`anchor_label` in both tools are derived from `frame_index`/
`slot` (`T%02d`/`A%02d`) — the same position-based scheme the OLED display
uses — not from `node_id`, which is an arbitrary MAC-derived value with no
pattern.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `$STAT` shows `hash_mismatch` climbing | `init_count`/`resp_count`/timing keys don't match across boards — reprovision all of them together (see Provisioning) |
| `$STAT` shows `tx_late` climbing | slot timing too tight for this board's actual SPI cost — `$SET,proc_us,<higher>`, `$AUTOTIME`, `$SAVE`, `$RESET` |
| Tag reads `RESP_LOST` for every anchor slot | that slot has no anchor powered/in range, or its `slot_index` collides with another anchor (`$STAT dup_index`) |
| Every range is SS-TWR fallback (`$RNG` flags bit0 set) | the Report frame isn't completing — usually the same tight-timing cause as `tx_late`; check the tag's `finals_tx`/anchor's `finals_rx` in `$STAT` |
| Board went silent after a flash/reset/killed script | DTR/RTS auto-reset glitch — see the reset snippet in Flashing |
| `$SET`/`$SAVE`/`$DEFAULT` returns `$ERR,5,...,ranging active` | `$RANGE,STOP` first |
| Changed `role` but the board keeps behaving like its old role | role dispatch happens once at boot — `$RESET` after `$SET,role,...` + `$SAVE` |
