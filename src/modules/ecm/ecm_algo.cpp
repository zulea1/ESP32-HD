/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_algo.cpp
 * @brief HD ECM seed -> key (see ecm_algo.h). Table lookup in table 1/2 + F1F4-byte1 flag on the low byte.
 */
#include "ecm_algo.h"
#include "ecm_algo_data.h"      // ECM_SEEDKEY_TABLE   == table 2
#include "ecm_algo_data_t1.h"   // ECM_SEEDKEY_TABLE1  == table 1
#include <pgmspace.h>
#include <string.h>

// Word offset into either table for a 16-bit seed. Both tables carry the same two padding entries at the
// 21845 / 43690 boundaries, so the +1 / +2 offsets apply identically: n<21845 -> n; 21845<=n<43690 -> n+1;
// n>=43690 -> n+2.
static inline uint32_t ecmWordIndex(uint16_t n) {
    if (n >= 21845u) return (uint32_t)((uint32_t)(n - 21845u) >= 21845u ? ((uint32_t)n + 2u) : ((uint32_t)n + 1u));
    return (uint32_t)n;
}

// strstr over a bounded, possibly non-terminated buffer.
static bool memContains(const uint8_t *hay, uint8_t hayLen, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || hayLen < nl) return false;
    for (uint8_t i = 0; i + nl <= hayLen; i++) {
        if (memcmp(hay + i, needle, nl) == 0) return true;
    }
    return false;
}

// Does the identification payload (buffer starting at [3]) match an exact part-number template?
static bool fieldEquals(const uint8_t *buf, uint8_t len, const char *tmpl) {
    size_t tl = strlen(tmpl);
    if ((size_t)len < 3 + tl) return false;
    if (memcmp(buf + 3, tmpl, tl) != 0) return false;
    // next char must not continue an alphanumeric token (the match must end exactly at the template)
    if (3 + tl < len) {
        uint8_t c = buf[3 + tl];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return false;
    }
    return true;
}

uint8_t ecmSelectSecurityTable(const uint8_t *f1ea, uint8_t f1ea_len,
                               const uint8_t *f1f4, uint8_t f1f4_len,
                               const uint8_t *f1ec, uint8_t f1ec_len) {
    // Table-selection logic (returns 1 -> table 1, else 2 -> table 2).
    // A = f1ea, B = f1f4, C = f1ec. Default to table 2 when identification is incomplete.
    if (!f1ea || f1ea_len < 4) return 2;

    // Haystack = C[3..10] + A[3..14], searched for "41000724".
    uint8_t hay[21]; uint8_t hp = 0;
    if (f1ec) for (uint8_t i = 3; i <= 10 && i < f1ec_len && hp < sizeof(hay); i++) hay[hp++] = f1ec[i];
    for (uint8_t i = 3; i <= 14 && i < f1ea_len && hp < sizeof(hay); i++) hay[hp++] = f1ea[i];
    const bool has724 = memContains(hay, hp, "41000724");

    const uint8_t a3 = f1ea[3];                       // first payload byte of F1EA, compared against '4'
    const uint16_t V = (f1f4 && f1f4_len >= 5)         // numeric window on F1F4 payload bytes [3],[4]
                           ? (uint16_t)(((uint16_t)f1f4[3] << 8) | (uint16_t)f1f4[4])
                           : 0u;

    bool match;
    if (has724) {
        if (a3 == '4') match = fieldEquals(f1ea, f1ea_len, "41000326A");
        else           return 2;
    } else if ((uint16_t)(V - 0x1B59u) < 0x885u) {     // 0x1B59 <= V < 0x23DE
        match = fieldEquals(f1ea, f1ea_len, "41000509");
    } else if (a3 != '4') {
        match = fieldEquals(f1ea, f1ea_len, "41000509");
    } else {
        match = fieldEquals(f1ea, f1ea_len, "41000326A");
    }
    return match ? 1 : 2;
}

bool ecmComputeSecurityKeyTable(uint8_t table, const uint8_t flag,
                                const uint8_t *seed, uint8_t seed_len,
                                uint8_t *key, uint8_t *key_len) {
    if (key_len) *key_len = 0;
    if (!seed || !key || !key_len || seed_len < 2) return false;

    const uint16_t n = (uint16_t)(((uint16_t)seed[0] << 8) | (uint16_t)seed[1]);
    const uint32_t wordIdx = ecmWordIndex(n);

    uint16_t key16;
    if (table == 1) {
        if (wordIdx >= ECM_SEEDKEY_TABLE1_LEN) return false;
        key16 = pgm_read_word(&ECM_SEEDKEY_TABLE1[wordIdx]);
    } else {
        if (wordIdx >= ECM_SEEDKEY_TABLE_LEN) return false;
        key16 = pgm_read_word(&ECM_SEEDKEY_TABLE[wordIdx]);
    }

    uint8_t hi = (uint8_t)(key16 >> 8);
    uint8_t lo = (uint8_t)(key16 & 0xFF);
    lo ^= (uint8_t)(flag & 0x4A);   // low-byte XOR; high byte passed through unchanged

    key[0] = hi;
    key[1] = lo;
    *key_len = 2;
    return true;
}

bool ecmComputeSecurityKey(const uint8_t flag, const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len) {
    // Default / backward-compatible path: securityTable2.
    return ecmComputeSecurityKeyTable(2, flag, seed, seed_len, key, key_len);
}
