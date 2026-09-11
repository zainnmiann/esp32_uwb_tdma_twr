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
pio run -t upload
pio device monitor -b 921600
```

## Defaults

- `init_count=16`, `resp_count=8`, `report_enable=1`
- ESP32 slot budget verified on hardware via `$AUTOTIME` (`frame_dur_us=33047`, `proc_us=1500`)
- Serial `$RNG` CSV (same framing as DWM1001) for Jetson → MQTT

Provision with `$SET` / `$ANCHOR` / `$TAG`, then `$AUTOTIME`, `$SAVE`,
`$RANGE,START`. Use `$MEASURE` on hardware to tighten timing.
