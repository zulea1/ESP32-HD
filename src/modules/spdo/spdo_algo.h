/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_algo.h
 * @brief SPDO (module) UDS Security Access (0x27) seed -> key.
 *
 * Algorithm family 0x02: key16 = (6151 * seed16 ^ 8209) & 0xFFFF.
 * Plain arithmetic - no lookup table and no F1F4 flag. Verified: seed 0x7355 -> key 0x3F42.
 */
#pragma once
#include <stdint.h>

bool spdoComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len);
