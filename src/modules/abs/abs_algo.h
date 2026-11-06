/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_algo.h
 * @brief ABS (module) UDS Security Access (0x27) seed -> key.
 *
 * Algorithm family 0x03: plain 32-bit arithmetic, no table/flag -
 * m = ((seed16 - 25110) << 4) + 160; key = [(m>>8)&0xFF, m&0xFF]. Verified for all 65536 seeds
 * (seed 0x9AED -> key 0x8E10). Verify on the bus by log before running any ABS procedure.
 */
#pragma once
#include <stdint.h>

bool absComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len);
