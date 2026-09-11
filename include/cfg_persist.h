/**
 * @file cfg_persist.h
 * @brief NVS-backed cfg blob (replaces nRF flash page).
 */
#ifndef CFG_PERSIST_H_
#define CFG_PERSIST_H_

#include <stdint.h>
#include <stdbool.h>
#include "cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

bool cfg_persist_load(cfg_t *out);
bool cfg_persist_save(const cfg_t *in);

#ifdef __cplusplus
}
#endif

#endif /* CFG_PERSIST_H_ */
