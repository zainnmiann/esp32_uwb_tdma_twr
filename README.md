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

*A parallel, more rigorous version of this procedure exists for the DWM1001
reference firmware — `FLASHING_AND_CALIBRATION.md` on the
`dwm1001-uwb-dstwr-firmware` branch of `d7-tech/HydroRL`. The scale and the
fleet-wide strategy below are reconciled with it; one step from it
(checking the chip's OTP for a factory-calibrated value before measuring)
does **not** carry over here — see the note after Step 0.*

**Both registers, same value.** `ant_delay_tx` and `ant_delay_rx` must
always be set to the same number on a given board. Each takes the full
one-way delay for its own direction; splitting the correction between them
under-compensates by roughly 77 m — measured on the DWM1001 hardware before
it was corrected, and the same math applies here.

**Direction.** Increasing a node's antenna delay *shortens* the ranges it
participates in; decreasing it lengthens them. If a pair reads longer than
true, the delay is too small.

**Scale.** One DTU (device time unit) is 15.650040 ps ≈ 4.69 mm of range —
about 213 units per metre. Because both ends of a measurement contribute:

| what you change | effect on the reported range |
|---|---|
| one board's delay, by 1 unit | ≈ 4.7 mm |
| both boards' delays, by 1 unit each | ≈ 9.4 mm |

The procedure below only ever changes one board at a time, so it uses the
single-board figure (4.69 mm/unit) throughout.

`ant_delay_tx`/`ant_delay_rx` are per-board config and **not** part of the
config hash — boards can be calibrated one at a time, and the rest of the
fleet doesn't need to be present.

### Step 0 — factory value: does not apply on this board

On the DWM1001 (a calibrated DWM1000 *module*), setting `ant_delay_tx` to
the uncalibrated sentinel (`0xFFFF`/65535) makes the firmware read back a
per-chip factory delay from OTP if one is programmed, which can remove the
need to measure at all. **This firmware's `apply_antenna()` does not attempt
that OTP read** — on the uncalibrated sentinel it always falls straight
through to 16436, unconditionally. This was a deliberate choice: these are
bare DW1000s behind an external PA and a non-Decawave antenna path, not
calibrated modules, so any OTP figure (if even programmed) would not reflect
this board's actual delay. Skip straight to Step 1 — there is nothing to
check for here.

### Step 1 — measure the error at a known distance

1. Place one tag and one anchor antenna-face to antenna-face, clear line of
   sight, nothing within about half a metre of the path, both at the same
   height. **Pick a distance close to your actual operating range** — see
   the nonlinearity note below for why.
2. Write down the true distance (tape measure, ±5 mm is fine).
3. `$RANGE,START` on both, let it settle a few seconds, then take the mean
   over a stable window — not a single reading:
   ```bash
   ./range_table.py -w 15
   ```
   The std dev shown should be a couple of cm; if it's much worse, fix the
   physical setup before calibrating — the reading isn't stable enough to
   calibrate against yet.

### Step 2 — work out the correction

```
error_mm  = measured_mean_mm - true_distance_mm
delta_dtu = round(error_mm / 4.69)        # single board moving
new_delay = old_delay + delta_dtu
```

Worked example — true 5.000 m, reported 5.187 m:
```
error = +187 mm   (reads long -> delay is too small)
delta = round(187 / 4.69) = 40
new   = 16436 + 40 = 16476
```

### Step 3 — apply to the tag only, and save

Keep the reference anchor fixed as your truth reference for the whole
campaign — only adjust the tag:
```
$RANGE,STOP*..
$SET,ant_delay_tx,<new_delay>*..
$SET,ant_delay_rx,<new_delay>*..
$SAVE*..
$RESET*..
```
Re-measure. One pass is usually enough; if the residual is still more than a
couple of cm, repeat Step 2 with the new residual error.

### Step 4 — confirm the scale is behaving (recommended)

Before trusting a value across the fleet: change one board's delay by a
large, deliberate amount — e.g. **+213 units**, which should shorten that
pair by about **1 m** — measure, confirm the shift is close to 1 m, then put
it back. If it isn't close, stop and re-check the setup before calibrating
further; deriving a value from arithmetic alone (rather than checking it
against hardware) is exactly how a halving-style error gets baked into a
whole fleet.

### Step 5 — the rest of the fleet

The Step 1 measurement fixes the *sum* of two boards' offsets — it can't
tell you how much belonged to each on its own. Two strategies:

- **Quick — one value everywhere.** Apply the calibrated number to every
  board. Removes the bulk of the error in one measurement, but leaves
  board-to-board differences uncorrected (on the DWM1001 fleet this showed
  up as two tags reading the same anchor up to 28 cm apart).
- **Better — one fixed reference board.** Nominate a board (anchor or tag)
  as the reference and never change it again. For every other board, survey
  one distance to the reference and adjust *only that board* until it reads
  true, using the single-board figure (4.69 mm/unit). More measurements, but
  this is what makes different tags agree with each other on the same
  anchor.

### A caveat worth knowing

On the DWM1001 hardware, the reported distance also carries a smaller error
that varies *with* distance rather than being constant — roughly 19 cm
across a 0.15–6 m span, strongest at close range (a known DW1000 near-field
characteristic, not specific to that board). A constant antenna-delay
correction cannot absorb it. This has not been separately measured on this
ESP32 board's antenna/PA path, but the same underlying DW1000 effect likely
applies to some degree. Two practical consequences either way:

- Calibrate at a distance close to where the system will actually operate —
  a delay fitted at 1 m will be off by a bit more than a decimetre at 5 m,
  and the reverse.
- If accuracy matters across a wide span, calibrate at both a near and a far
  distance and treat the difference as this second term rather than folding
  it into the antenna delay.

### Record what you set

Antenna delays live only in each board's flash and are written down nowhere
else in this repo. `$DEFAULT` + `$SAVE`, or a full config wipe during
provisioning, will erase a calibration. Keep a note — board, value, the
distance it was measured at, and the date.

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
