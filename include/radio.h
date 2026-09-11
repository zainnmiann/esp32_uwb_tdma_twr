/**
 * @file radio.h
 * @brief Section 12 — DW1000 library mapping (ESP32 / Makerfabs DW1000Class).
 */
#ifndef RADIO_H_
#define RADIO_H_

#include <stdint.h>
#include <stdbool.h>
#include "cfg.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t  buf[MAX_FRAME_LEN];
    uint16_t len;
    uint64_t rx_ts;
    uint32_t status;
} radio_rx_t;

#define RADIO_RX_FRAME      1
#define RADIO_RX_TIMEOUT    0
#define RADIO_RX_ERROR    (-1)
#define RADIO_RX_PENDING    2

#define RADIO_TX_OK         0
#define RADIO_TX_LATE     (-1)

bool     radio_init(const cfg_t *c);
bool     radio_reconfigure(const cfg_t *c);
int      radio_rx(radio_rx_t *rx, uint32_t timeout_us, bool delayed, uint32_t start_hi32);
bool     radio_rx_arm(uint32_t timeout_us, bool delayed, uint32_t start_hi32);
int      radio_rx_poll(radio_rx_t *rx);
int      radio_tx_at(const uint8_t *buf, uint16_t len, uint32_t tx_hi32);
void     radio_tx_wait(void);
void     radio_idle(void);

uint32_t radio_systime_hi32(void);
int32_t  radio_hi32_diff_us(uint32_t a, uint32_t b);
uint64_t radio_tx_ts_from_hi32(uint32_t tx_hi32);
int32_t  radio_carrier_integrator(void);

typedef struct
{
    int16_t  rx_dbm10;
    int16_t  fp_dbm10;
    uint16_t std_noise;
    uint16_t rxpacc;
} radio_quality_t;

void     radio_quality(radio_quality_t *q);
uint32_t radio_txpower_reg(void);
bool     radio_txpower_from_otp(void);
uint16_t radio_ant_tx(void);
uint16_t radio_ant_rx(void);
uint32_t radio_part_id(void);
uint32_t radio_device_id(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H_ */
