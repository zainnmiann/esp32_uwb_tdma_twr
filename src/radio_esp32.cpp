/**
 * @file radio_esp32.cpp
 * @brief radio.h implemented on Makerfabs DW1000Class (library unmodified).
 *
 * Absolute delayed TX/RX and RX frame-wait timeout use public writeBytes /
 * readBytes (Makerfabs ships //private: commented out). Antenna delay is
 * programmed via setAntennaDelay() + commitConfiguration().
 */

#include "radio.h"
#include "timing.h"
#include "board_pins.h"

#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <string.h>

#include <DW1000.h>

/* Registers / bits not exposed as high-level helpers. */
#ifndef RX_FWTO
#define RX_FWTO           0x0C
#define LEN_RX_FWTO       2
#endif
#ifndef RXWTOE_BIT
#define RXWTOE_BIT        28
#endif
#ifndef HPDWARN_BIT
#define HPDWARN_BIT       27
#endif
#ifndef TXPUTE_BIT
#define TXPUTE_BIT        10  /* byte 4 bit 2 => absolute bit 34; handled specially */
#endif

static uint16_t s_ant_tx = CFG_ANT_DELAY_DEFAULT;
static uint16_t s_ant_rx = CFG_ANT_DELAY_DEFAULT;
static uint32_t s_txpwr;
static bool     s_txpwr_otp;
static uint8_t  s_prf64;
static uint8_t  s_datarate;
static uint8_t  s_ns_sfd;
static uint32_t s_part_id;
static uint32_t s_device_id;

static uint32_t us_to_uus(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * 10000ULL) / 10256ULL);
}

static void write_u16(uint8_t reg, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    DW1000.writeBytes(reg, NO_SUB, b, 2);
}

static void write_u32(uint8_t reg, uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)(v), (uint8_t)(v >> 8),
        (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    DW1000.writeBytes(reg, NO_SUB, b, 4);
}

static uint32_t read_u32(uint8_t reg)
{
    uint8_t b[4] = {0};
    DW1000.readBytes(reg, NO_SUB, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t read_u40(uint8_t reg)
{
    uint8_t b[5] = {0};
    DW1000.readBytes(reg, NO_SUB, b, 5);
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32);
}

static void write_dx_time_hi32(uint32_t tx_hi32)
{
    /* DX_TIME low 9 bits ignored; hi32 is bits [39:8] with bit0 of hi32 forced 0. */
    uint8_t dx[5] = {0};
    uint32_t v = tx_hi32 & 0xFFFFFFFEu;
    dx[1] = (uint8_t)(v & 0xFF);
    dx[2] = (uint8_t)((v >> 8) & 0xFF);
    dx[3] = (uint8_t)((v >> 16) & 0xFF);
    dx[4] = (uint8_t)((v >> 24) & 0xFF);
    DW1000.writeBytes(DX_TIME, NO_SUB, dx, LEN_DX_TIME);
}

static void set_bit_local(uint8_t *buf, uint16_t len, uint16_t bit, bool val)
{
    uint16_t idx = bit / 8;
    uint8_t  m   = (uint8_t)(1u << (bit % 8));
    if (idx >= len) { return; }
    if (val) { buf[idx] |= m; }
    else     { buf[idx] &= (uint8_t)~m; }
}

static void rx_set_timeout(uint32_t timeout_us)
{
    uint8_t syscfg[LEN_SYS_CFG];
    uint32_t uus = us_to_uus(timeout_us);

    DW1000.readBytes(SYS_CFG, NO_SUB, syscfg, LEN_SYS_CFG);

    if (timeout_us == 0u)
    {
        set_bit_local(syscfg, LEN_SYS_CFG, RXWTOE_BIT, false);
        DW1000.writeBytes(SYS_CFG, NO_SUB, syscfg, LEN_SYS_CFG);
        return;
    }

    if (uus == 0u) { uus = 1u; }
    if (uus > 65535u) { uus = 65535u; }
    write_u16(RX_FWTO, (uint16_t)uus);
    set_bit_local(syscfg, LEN_SYS_CFG, RXWTOE_BIT, true);
    DW1000.writeBytes(SYS_CFG, NO_SUB, syscfg, LEN_SYS_CFG);
}

static void clear_irq_mask(void)
{
    /* Polled SYS_STATUS — keep IRQ line quiet (ISR still attached by begin). */
    uint8_t mask[LEN_SYS_MASK];
    memset(mask, 0, sizeof(mask));
    DW1000.writeBytes(SYS_MASK, NO_SUB, mask, LEN_SYS_MASK);
}

static void apply_antenna(const cfg_t *c)
{
    uint16_t d = CFG_ANT_DELAY_DEFAULT;
    if (c->ant_delay_tx != CFG_ANT_DELAY_UNCAL)
    {
        d = c->ant_delay_tx;
    }
    else if (c->ant_delay_rx != CFG_ANT_DELAY_UNCAL)
    {
        d = c->ant_delay_rx;
    }
    s_ant_tx = d;
    s_ant_rx = (c->ant_delay_rx != CFG_ANT_DELAY_UNCAL) ? c->ant_delay_rx : d;

    /* Same value both directions (see DWM1001 radio.c commentary). */
    DW1000.setAntennaDelay(s_ant_tx);
    /* Force HW write: setAntennaDelay alone only updates RAM. */
    {
        uint8_t bytes[2] = { (uint8_t)(s_ant_tx & 0xFF), (uint8_t)(s_ant_tx >> 8) };
        DW1000.writeBytes(TX_ANTD, NO_SUB, bytes, LEN_TX_ANTD);
        DW1000.writeBytes(LDE_IF, LDE_RXANTD_SUB, bytes, LEN_LDE_RXANTD);
        if (s_ant_rx != s_ant_tx)
        {
            bytes[0] = (uint8_t)(s_ant_rx & 0xFF);
            bytes[1] = (uint8_t)(s_ant_rx >> 8);
            DW1000.writeBytes(LDE_IF, LDE_RXANTD_SUB, bytes, LEN_LDE_RXANTD);
        }
    }
}

static void apply_cfg(const cfg_t *c)
{
    const byte *mode = DW1000.MODE_SHORTDATA_FAST_ACCURACY;

    if (c->datarate == RATE_6800 && c->prf == PRF_64 && c->preamble_len == 128)
    {
        mode = DW1000.MODE_SHORTDATA_FAST_ACCURACY;
    }
    else if (c->datarate == RATE_6800 && c->prf == PRF_16 && c->preamble_len == 128)
    {
        mode = DW1000.MODE_SHORTDATA_FAST_LOWPOWER;
    }
    else if (c->datarate == RATE_110)
    {
        mode = DW1000.MODE_LONGDATA_RANGE_ACCURACY;
    }
    else
    {
        mode = DW1000.MODE_LONGDATA_FAST_ACCURACY;
    }

    s_prf64    = (c->prf == PRF_64) ? 1u : 0u;
    s_datarate = c->datarate;
    s_ns_sfd   = (c->sfd_mode == SFD_DW) ? 1u : 0u;

    DW1000.newConfiguration();
    DW1000.setDefaults();
    DW1000.enableMode(mode);
    DW1000.setChannel((byte)c->channel);
    DW1000.setPreambleCode((byte)c->preamble_code);
    DW1000.setNetworkId(c->pan_id);
    DW1000.setDeviceAddress(c->node_id);
    DW1000.commitConfiguration();

    apply_antenna(c);

    /* large_power_init after commit — Makerfabs Pro PA / GPIO tweaks. Sets
     * TX_POWER = 0x1F1F1F1F (max), appropriate as the TXP_AUTO default for a
     * board with an external PA. */
    DW1000.large_power_init();

    /* txpower_mode/coarse/fine/reg are live, settable serial keys ($STAT
     * reports the programmed register), but until here nothing ever read
     * them back -- TXP_MANUAL was silently a no-op, with no way to turn
     * power down for close-range bench testing. TXP_MANUAL semantics match
     * the DWM1001 reference's apply_txpower(): txpower_reg verbatim if
     * nonzero, else (coarse<<5)|fine replicated across all four bytes
     * (smart power is off, so all four must carry the same value). */
    if (c->txpower_mode == TXP_MANUAL)
    {
        uint32_t reg;
        if (c->txpower_reg != 0u)
        {
            reg = c->txpower_reg;
        }
        else
        {
            uint8_t b = (uint8_t)(((c->txpower_coarse & 0x07u) << 5) |
                                   (c->txpower_fine & 0x1Fu));
            reg = ((uint32_t)b << 24) | ((uint32_t)b << 16) |
                  ((uint32_t)b << 8) | (uint32_t)b;
        }
        write_u32(TX_POWER, reg);
    }

    /* Makerfabs tune() writes LDE_CFG1 = 0x0D (PMULT=0). Decawave's own
     * parameter table (PEAK_MULTPLIER 0x60 | N_STD_FACTOR 13 = 0x6D) is what
     * the DWM1001 reference firmware runs, and this field shifts the LDE
     * leading-edge threshold, i.e. every RX timestamp. A fleet mixing this
     * board against the DWM1001 default carries a fixed range bias unless
     * corrected here. */
    {
        uint8_t lde_cfg1 = 0x6D;
        DW1000.writeBytes(LDE_IF, LDE_CFG1_SUB, &lde_cfg1, LEN_LDE_CFG1);
    }

    /* setDefaults() (called above via newConfiguration path) leaves frame
     * filtering off. Software already checks dst address in handle_frame(),
     * so this is not a correctness bug, but at resp_count=8 every anchor
     * would otherwise read out and discard 7 other anchors' unicast
     * Responses/Reports on every frame -- real critical-path SPI time this
     * firmware's timing budget does not have room for. Listener role wants
     * everything, so it stays unfiltered. */
    if (c->role != ROLE_LSTN)
    {
        DW1000.setFrameFilter(true);
        DW1000.setFrameFilterAllowData(true);
        DW1000.writeSystemConfigurationRegister();
    }
    else
    {
        DW1000.setFrameFilter(false);
        DW1000.writeSystemConfigurationRegister();
    }

    s_txpwr = read_u32(TX_POWER);
    s_txpwr_otp = false;

    clear_irq_mask();
    rx_set_timeout(0);
}

bool radio_init(const cfg_t *c)
{
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    DW1000.begin(PIN_UWB_IRQ, PIN_UWB_RST);
    /* begin() unconditionally attaches handleInterrupt() on RISING, and that
     * handler performs its own SPI transactions -- if it ever fired mid
     * critical-path (arming a delayed TX/RX from the ranging loop, itself
     * doing SPI) it would corrupt the transaction. SYS_MASK is 0 out of
     * reset and clear_irq_mask() keeps it 0 outside the narrow SPI-only
     * config window in apply_cfg(), which currently makes this safe by
     * construction (nothing is ever receiving or transmitting during that
     * window). Detaching outright removes the dependency on that ordering
     * entirely -- this firmware is fully polled and never needs the ISR. */
    detachInterrupt(digitalPinToInterrupt(PIN_UWB_IRQ));
    DW1000.select(PIN_UWB_SS);

    /* dev=0x... in the boot banner exists as a hardware sanity check: every
     * genuine DW1000 reads back the fixed constant 0xDECA0130 from DEV_ID
     * (0x00), which is what the DWM1001 reference reports via
     * dwt_readdevid(). Hashing the printable identifier STRING instead (the
     * previous implementation here) produces a plausible-looking number
     * regardless of whether the chip identity is actually correct, which
     * defeats the check -- caught on real hardware, where it printed
     * 0xBA26FA53 for a board that was in fact working correctly. */
    s_device_id = read_u32(DEV_ID);

    /* cfg_apply_identity() folds this into node_id (the on-air address) when
     * node_id is unset, so it MUST be unique per board. It is not safe to
     * derive it from the DW1000's own identity here: confirmed on hardware
     * that two different boards produced the exact same part_id
     * (0xB8F941FD) and therefore the exact same auto-assigned node_id
     * (0xF904) -- these Makerfabs boards evidently ship with unprogrammed
     * OTP, so getPrintableExtendedUniqueIdentifier() reads back the same
     * default EUI on every unit, and hashing it just reproduces that
     * collision deterministically. The ESP32 itself carries a factory-burned
     * globally-unique MAC (Espressif-provisioned, not something this board
     * shares with any other), which is the only identity source here that
     * is actually guaranteed unique per physical unit. */
    {
        uint64_t mac = ESP.getEfuseMac();
        s_part_id = (uint32_t)(mac ^ (mac >> 32));
    }

    apply_cfg(c);
    return true;
}

bool radio_reconfigure(const cfg_t *c)
{
    DW1000.idle();
    apply_cfg(c);
    return true;
}

uint32_t radio_systime_hi32(void)
{
    uint64_t t = read_u40(SYS_TIME);
    return (uint32_t)((t >> 8) & 0xFFFFFFFFu);
}

int32_t radio_hi32_diff_us(uint32_t a, uint32_t b)
{
    int32_t d = (int32_t)(a - b);
    return (int32_t)(((int64_t)d * 1000LL) / 249600LL);
}

uint64_t radio_tx_ts_from_hi32(uint32_t tx_hi32)
{
    return ((((uint64_t)(tx_hi32 & 0xFFFFFFFEu)) << 8) + (uint64_t)s_ant_tx)
           & DTU_MASK_40;
}

int32_t radio_carrier_integrator(void)
{
    /* DRX_TUNE (0x27) sub 0x28, 3 bytes, 21-bit signed value -- register
     * layout verified against vendor/deca_driver's dwt_readcarrierintegrator()
     * (DRX_CARRIER_INT_OFFSET 0x28, LEN 3, sign bit 20). Only feeds the
     * SS-TWR fallback (ranging_k_hat()), used when a Report frame is lost;
     * the primary DS-TWR path (ranging_tof_dtu(), the default with
     * report_enable=1) does not read this at all. */
    uint8_t b[3] = {0};
    DW1000.readBytes(DRX_TUNE, 0x28, b, 3);
    uint32_t v = ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[0];
    if (v & 0x00100000u) { v |= 0xFFF00000u; }
    else                 { v &= 0x001FFFFFu; }
    return (int32_t)v;
}

void radio_idle(void) { DW1000.idle(); }

uint32_t radio_txpower_reg(void) { return s_txpwr; }
bool     radio_txpower_from_otp(void) { return s_txpwr_otp; }
uint16_t radio_ant_tx(void) { return s_ant_tx; }
uint16_t radio_ant_rx(void) { return s_ant_rx; }
uint32_t radio_part_id(void) { return s_part_id; }
uint32_t radio_device_id(void) { return s_device_id; }

static uint64_t read_rx_ts(void)
{
    /* The DW1000Time& overload silently calls correctTimestamp(), which does
     * an extra getReceivePower() SPI round trip and subtracts a power-
     * dependent range-bias table calibrated for Decawave's own reference
     * board. The DWM1001 firmware this is ported from reads the raw
     * timestamp and does its own antenna-delay calibration with no bias
     * table (ranging.c's round-to-nearest arithmetic assumes that). Stacking
     * this library's bias on top would silently corrupt every range by a
     * signal-strength-dependent amount and cost an unbudgeted SPI read on
     * every RX event. Use the raw byte[5] overload instead. */
    byte ts[5];
    DW1000.getReceiveTimestamp(ts);
    return ((uint64_t)ts[0]) | ((uint64_t)ts[1] << 8) | ((uint64_t)ts[2] << 16) |
           ((uint64_t)ts[3] << 24) | ((uint64_t)ts[4] << 32);
}

static void tx_status_clear(void)
{
    uint8_t st[LEN_SYS_STATUS];
    memset(st, 0xFF, sizeof(st));
    DW1000.writeBytes(SYS_STATUS, NO_SUB, st, LEN_SYS_STATUS);
}

bool radio_rx_arm(uint32_t timeout_us, bool delayed, uint32_t start_hi32)
{
    rx_set_timeout(timeout_us);
    DW1000.newReceive();
    DW1000.setDefaults();

    if (delayed)
    {
        write_dx_time_hi32(start_hi32);
        /* Arm RXDLYS via setDelay(0) path then overwrite absolute time. */
        DW1000Time z;
        z.setTimestamp((int64_t)0);
        (void)DW1000.setDelay(z);
        write_dx_time_hi32(start_hi32);

        /* If start already passed, chip will set HPDWARN on enable. */
        DW1000.startReceive();
        {
            uint8_t st[LEN_SYS_STATUS];
            DW1000.readBytes(SYS_STATUS, NO_SUB, st, LEN_SYS_STATUS);
            if ((st[3] & (1u << (HPDWARN_BIT - 24))) != 0)
            {
                DW1000.idle();
                return false;
            }
        }
        return true;
    }

    DW1000.startReceive();
    return true;
}

int radio_rx_poll(radio_rx_t *rx)
{
    uint8_t st[LEN_SYS_STATUS];
    DW1000.readBytes(SYS_STATUS, NO_SUB, st, LEN_SYS_STATUS);

    bool good = ((st[RXFCG_BIT / 8] & (1u << (RXFCG_BIT % 8))) != 0);
    bool to   = ((st[RXRFTO_BIT / 8] & (1u << (RXRFTO_BIT % 8))) != 0) ||
                ((st[RXPTO_BIT / 8] & (1u << (RXPTO_BIT % 8))) != 0) ||
                ((st[RXSFDTO_BIT / 8] & (1u << (RXSFDTO_BIT % 8))) != 0);
    bool err  = ((st[RXPHE_BIT / 8] & (1u << (RXPHE_BIT % 8))) != 0) ||
                ((st[RXFCE_BIT / 8] & (1u << (RXFCE_BIT % 8))) != 0) ||
                ((st[RXRFSL_BIT / 8] & (1u << (RXRFSL_BIT % 8))) != 0) ||
                ((st[LDEERR_BIT / 8] & (1u << (LDEERR_BIT % 8))) != 0);

    if (!good && !to && !err)
    {
        return RADIO_RX_PENDING;
    }

    rx->status = (uint32_t)st[0] | ((uint32_t)st[1] << 8) |
                 ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24);

    if (good)
    {
        uint8_t finfo[LEN_RX_FINFO];
        DW1000.readBytes(RX_FINFO, NO_SUB, finfo, LEN_RX_FINFO);
        uint16_t frame_len = (uint16_t)(finfo[0] | ((finfo[1] & 0x03) << 8));
        if (frame_len < (MHR_LEN + FCS_LEN) || frame_len > MAX_FRAME_LEN)
        {
            DW1000.idle();
            return RADIO_RX_ERROR;
        }
        rx->len = (uint16_t)(frame_len - FCS_LEN);
        DW1000.getData(rx->buf, rx->len);
        rx->rx_ts = read_rx_ts();

        /* Clear RX events */
        uint8_t clr[LEN_SYS_STATUS];
        memset(clr, 0, sizeof(clr));
        set_bit_local(clr, LEN_SYS_STATUS, RXFCG_BIT, true);
        DW1000.writeBytes(SYS_STATUS, NO_SUB, clr, LEN_SYS_STATUS);
        return RADIO_RX_FRAME;
    }

    if (to)
    {
        uint8_t clr[LEN_SYS_STATUS];
        memset(clr, 0xFF, sizeof(clr));
        DW1000.writeBytes(SYS_STATUS, NO_SUB, clr, LEN_SYS_STATUS);
        DW1000.idle();
        return RADIO_RX_TIMEOUT;
    }

    {
        uint8_t clr[LEN_SYS_STATUS];
        memset(clr, 0xFF, sizeof(clr));
        DW1000.writeBytes(SYS_STATUS, NO_SUB, clr, LEN_SYS_STATUS);
        DW1000.idle();
        return RADIO_RX_ERROR;
    }
}

int radio_rx(radio_rx_t *rx, uint32_t timeout_us, bool delayed, uint32_t start_hi32)
{
    int r;
    if (!radio_rx_arm(timeout_us, delayed, start_hi32))
    {
        return RADIO_RX_TIMEOUT;
    }
    do { r = radio_rx_poll(rx); } while (r == RADIO_RX_PENDING);
    return r;
}

int radio_tx_at(const uint8_t *buf, uint16_t len, uint32_t tx_hi32)
{
    /* len includes FCS (2). Makerfabs setData expects payload without FCS when
     * frame check is enabled (default). */
    uint16_t payload = (len >= FCS_LEN) ? (uint16_t)(len - FCS_LEN) : len;

    tx_status_clear();
    DW1000.newTransmit();
    DW1000.setDefaults();
    DW1000.setData((byte *)buf, payload);

    DW1000Time z;
    z.setTimestamp((int64_t)1000); /* force TXDLYS path */
    (void)DW1000.setDelay(z);
    write_dx_time_hi32(tx_hi32);

    DW1000.startTransmit();

    /* Late? HPDWARN / TXPUTE */
    {
        uint8_t st[LEN_SYS_STATUS];
        DW1000.readBytes(SYS_STATUS, NO_SUB, st, LEN_SYS_STATUS);
        bool late = ((st[3] & (1u << (HPDWARN_BIT - 24))) != 0) ||
                    ((st[4] & 0x04u) != 0); /* TXPUTE */
        if (late)
        {
            DW1000.idle();
            tx_status_clear();
            return RADIO_TX_LATE;
        }
    }
    return RADIO_TX_OK;
}

#define TX_WAIT_MAX_US 20000u

void radio_tx_wait(void)
{
    uint32_t start = radio_systime_hi32();
    uint32_t n = 0;
    for (;;)
    {
        uint8_t st[LEN_SYS_STATUS];
        DW1000.readBytes(SYS_STATUS, NO_SUB, st, LEN_SYS_STATUS);
        if ((st[0] & (1u << TXFRS_BIT)) != 0)
        {
            uint8_t clr[LEN_SYS_STATUS];
            memset(clr, 0, sizeof(clr));
            set_bit_local(clr, LEN_SYS_STATUS, TXFRS_BIT, true);
            DW1000.writeBytes(SYS_STATUS, NO_SUB, clr, LEN_SYS_STATUS);
            return;
        }
        if ((++n & 0x3Fu) == 0u &&
            radio_hi32_diff_us(radio_systime_hi32(), start) > (int32_t)TX_WAIT_MAX_US)
        {
            DW1000.idle();
            tx_status_clear();
            return;
        }
    }
}

void radio_quality(radio_quality_t *q)
{
    /* DW1000.getReceivePower()/getFirstPathPower() apply an extra "Fig. 22"
     * correction (estPwr += (estPwr+88)*corrFac above -88 dBm) that the
     * DWM1001 reference's radio_quality() never applies. That correction is
     * affine with the same intercept on both quantities, so it scales
     * (rx_dbm10 - fp_dbm10) by (1+corrFac) -- ~2.17x at 64 MHz PRF -- above
     * -88 dBm, which is the common indoor case. The reference's
     * NLOS_THRESHOLD_DBM10 (10.0 dB) is calibrated against the uncorrected
     * figure, so feeding it the library's corrected numbers would trip NLOS
     * far too easily. Compute the raw formula directly from registers
     * instead, matching radio.c's radio_quality() verbatim. */
    memset(q, 0, sizeof(*q));

    uint8_t fq[8] = {0};
    DW1000.readBytes(RX_FQUAL, NO_SUB, fq, 8);
    uint16_t std_noise = (uint16_t)fq[0] | ((uint16_t)fq[1] << 8);
    uint16_t f2        = (uint16_t)fq[2] | ((uint16_t)fq[3] << 8);
    uint16_t f3        = (uint16_t)fq[4] | ((uint16_t)fq[5] << 8);
    uint16_t cir_pwr    = (uint16_t)fq[6] | ((uint16_t)fq[7] << 8);

    uint8_t fp1[2] = {0};
    DW1000.readBytes(RX_TIME, FP_AMPL1_SUB, fp1, 2);
    uint16_t f1 = (uint16_t)fp1[0] | ((uint16_t)fp1[1] << 8);

    uint8_t finfo[4] = {0};
    DW1000.readBytes(RX_FINFO, NO_SUB, finfo, 4);
    uint32_t rxpacc = ((((uint32_t)finfo[2]) >> 4) & 0xFFu) | (((uint32_t)finfo[3]) << 4);

    uint32_t adj;
    if (s_datarate == RATE_110)      { adj = 64u; }
    else if (s_ns_sfd == 0u)         { adj = 5u; }
    else if (s_datarate == RATE_850) { adj = 8u; }
    else                             { adj = 10u; }

    q->std_noise = std_noise;
    q->rxpacc    = (uint16_t)rxpacc;

    if (rxpacc <= adj) { return; }
    float n = (float)(rxpacc - adj);
    float a = s_prf64 ? 121.74f : 113.77f;

    if (cir_pwr > 0u)
    {
        float v = 10.0f * log10f(((float)cir_pwr * 131072.0f) / (n * n)) - a;
        q->rx_dbm10 = (int16_t)lroundf(v * 10.0f);
    }
    if ((uint32_t)f1 + f2 + f3 > 0u)
    {
        float v = 10.0f * log10f(((float)f1 * f1 + (float)f2 * f2 + (float)f3 * f3) / (n * n)) - a;
        q->fp_dbm10 = (int16_t)lroundf(v * 10.0f);
    }
}
