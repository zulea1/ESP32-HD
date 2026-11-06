/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_algo.h
 * @brief HD ECM (0x7E0) UDS Security Access (0x27) seed -> key algorithm.
 *
 * This is a table-lookup seed/key algorithm (algorithm family 0x01):
 *   key_hi = table[seed] >> 8                         (high byte, verbatim)
 *   key_lo = (table[seed] & 0xFF) ^ (flag & 0x4A)     (low byte, flag = F1F4 byte index 1)
 *
 * TWO tables exist and the correct one is chosen at runtime, from the ECM's own
 * identification responses:
 *   - ECM_SEEDKEY_TABLE   (ecm_algo_data.h)    = table 2 (the default; every
 *                                                 ECM observed so far lands here)
 *   - ECM_SEEDKEY_TABLE1  (ecm_algo_data_t1.h) = table 1 (only for the
 *                                                 table-1 part families: 41000326A / 41000509, gated by a
 *                                                 41000724 substring, a numeric window and the '4' digit)
 * Table selection reads the identification triplet 22F1EA / 22F1F4 / 22F1EC (buffers A / B / C). The low-byte
 * XOR flag is F1F4 byte index 1, same as before.
 */
#pragma once
#include <stdint.h>

// Pick the ECM seed->key table (1 or 2), from the three identification responses
// (each the full UDS reply "62 F1 xx <payload...>", payload at index 3).
// Any missing/short buffer falls back to table 2. f1ea=A, f1f4=B, f1ec=C.
uint8_t ecmSelectSecurityTable(const uint8_t *f1ea, uint8_t f1ea_len,
                               const uint8_t *f1f4, uint8_t f1f4_len,
                               const uint8_t *f1ec, uint8_t f1ec_len);

// Compute the key from an explicit table (1 or 2). flag is the F1F4 key-flag byte (byte index 1 of the payload).
bool ecmComputeSecurityKeyTable(uint8_t table, const uint8_t flag,
                                const uint8_t *seed, uint8_t seed_len,
                                uint8_t *key, uint8_t *key_len);

// Backward-compatible entry point: uses securityTable2 (table 2). This is the path LHC/RHC/proc_runner keep using.
bool ecmComputeSecurityKey(const uint8_t flag, const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len);
