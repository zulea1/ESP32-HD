/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_outputs_data.h
 * @brief SPDO (0x7F2, Speedometer / Instrument Cluster) output test catalogue, ItemIndex 01-12.
 *
 * All 18 commands below are UDS 0x2F (IOControlByIdentifier) requests, verified byte-for-byte:
 *
 *   DID 0x0501 (bytes 2 Lamp/indicator register, ONOFF): a shared 4-byte bitmask. "on" sets one bit, the
 *   register is cleared back to all-zero (shared "off") and control is returned (2F 05 01 00) when the test
 *   stops. Covers every lamp/indicator item (02-11 except 07).
 *
 *   DID 0x0503 (LCD display test, ONOFF): a 22-byte segment mask, all-FF = every LCD segment lit, all-00 =
 *   cleared. Its own release sequence additionally returns control of DID 0x0501, so this is the one item
 *   needing a 3rd "end" step.
 *
 *   DID 0x0500 (needle sweep test, ADJUST): a combined structure, bytes [1:2] = speedometer (km/h x10,
 *   big-endian), bytes [3:4] = tachometer (rpm, big-endian). Item 01 (Speedometer) and item 12 (Tachometer)
 *   each only ever write their own half, leaving the other half zero. The 5 preset steps per item use these
 *   values, substituted into the templates:
 *     Speedometer : 2F05000300####0000   (0,50,100,150,200 km/h  -> 0000,01F4,03E8,05DC,07D0)
 *     Tachometer  : 2F050003000000####   (0,2000,4000,6000,8000 rpm -> 0000,07D7,0FA0,1777,1F40)
 *   Tachometer is the one item gated by Security Access (seed 2701 / key 2702) - see spdo_security.h.
 *
 * Only a single output-test profile is implemented here, which covers the large majority of SPDO vehicle
 * variants. Two other, smaller variant layouts exist (one used only by the legacy VPW/OBD-II netlayer, not
 * reachable over this firmware's CAN bus; and another used by a handful of CAN variants with a different
 * item layout) and are not decoded/implemented. There is also no verified calibration-ID-to-variant mapping
 * for SPDO (unlike ECM/ABS/BCM, the SPDO variant appears to be picked by VIN/model rather than by a live
 * calibration read), so there is no automatic profile-selection rule table below - the single profile is
 * always used, and a manual override exists for consistency with the other output-test pages even though it
 * currently has nothing else to switch to.
 */
#pragma once
#include <stdint.h>

// Numbered to match the web UI's shared mode convention (ecmOutModeText/ecmOutRowHtml in main.js):
// 1 = PULSE (not used by SPDO), 2 = ON/OFF, 3 = ADJUST.
enum SpdoOutMode : uint8_t { SPDOO_ONOFF = 2, SPDOO_ADJUST = 3 };

struct SpdoOutKind {
    const char* key;
    const char* name;
    uint8_t     mode;        // SpdoOutMode
    bool        secure;      // requires Security Access (0x27) before the first command
    const char* on;          // ONOFF: the "on" request.  ADJUST: unused (see cmds[])
    const char* end1;         // release step 1 (ONOFF: clear register. ADJUST: back to step 0)
    const char* end2;         // release step 2 (return control of the primary DID)
    const char* end3;         // release step 3, only item07 needs this (return control of a 2nd DID); "" otherwise
    uint8_t     steps;        // ADJUST only: number of entries in vals[]/cmds[] (0 for ONOFF)
    int16_t     vals[5];      // ADJUST only: the step values shown in the UI (km/h or rpm)
    const char* cmds[5];      // ADJUST only: the literal full command for each step (no runtime templating)
};

static const SpdoOutKind SPDO_OUT_KINDS[] = {
    // key                  name                                mode          secure  on                                               end1                                             end2        end3        steps  vals                            cmds
    {"speedometer",         "Speedometer",                       SPDOO_ADJUST, false, "",                                               "2F0500030000000000",                           "2F050000", "",         5, {0,50,100,150,200},
        {"2F0500030000000000","2F0500030001F40000","2F0500030003E80000","2F0500030005DC0000","2F0500030007D00000"}},
    {"battery_charge_lamp", "Battery charge lamp",               SPDOO_ONOFF,  false, "2F05010380000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"fuel_reserve_lamp",   "Fuel down to reserve warning symbol", SPDOO_ONOFF, false, "2F05010300010000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"engine_check_light",  "Engine check light",                 SPDOO_ONOFF, false, "2F05010301000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"sixth_gear",          "6th Gear indicator",                 SPDOO_ONOFF, false, "2F05010300000400",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"speed_pointer_led",   "Speed pointer LED",                  SPDOO_ONOFF, false, "2F05010300000002",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"lcd_display",         "LCD display (all segments)",         SPDOO_ONOFF, false, "2F050303FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",   "2F0503030000000000000000000000000000000000", "2F050300", "2F050100", 0, {0,0,0,0,0}, {"","","","",""}},
    {"turn_signal_left",    "Turn Signal Left lamp",              SPDOO_ONOFF, false, "2F05010300200000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"turn_signal_right",   "Turn Signal Right lamp",             SPDOO_ONOFF, false, "2F05010300100000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"neutral_indicator",   "Neutral Indicator lamp",             SPDOO_ONOFF, false, "2F05010300080000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"high_beam_lamp",      "High beam indicator lamp",           SPDOO_ONOFF, false, "2F05010300020000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"oil_pressure_lamp",   "Oil pressure warning symbol",        SPDOO_ONOFF, false, "2F05010300040000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"abs_safety_lamp",     "ABS Safety lamp",                    SPDOO_ONOFF, false, "2F05010300000200",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"security_lamp",       "Security Lamp",                      SPDOO_ONOFF, false, "2F05010340000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"coolant_temp_lamp",   "Coolant temperature warning light",  SPDOO_ONOFF, false, "2F05010302000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"cruise_enable_lamp",  "Cruise Enable lamp",                 SPDOO_ONOFF, false, "2F05010304000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"cruise_set_lamp",     "Cruise Set lamp",                    SPDOO_ONOFF, false, "2F05010308000000",                              "2F05010300000000",                             "2F050100", "",         0, {0,0,0,0,0}, {"","","","",""}},
    {"tachometer",          "Tachometer",                         SPDOO_ADJUST, true,  "",                                              "2F0500030000000000",                           "2F050000", "",         5, {0,2000,4000,6000,8000},
        {"2F0500030000000000","2F05000300000007D7","2F0500030000000FA0","2F0500030000001777","2F0500030000001F40"}},
};
#define SPDO_OUT_KIND_COUNT (sizeof(SPDO_OUT_KINDS) / sizeof(SPDO_OUT_KINDS[0]))

struct SpdoOutProfile { const char* name; uint8_t count; uint8_t kinds[18]; };
static const SpdoOutProfile SPDO_OUT_PROFILES[] = {
    {"Standard (CAN)", 18, {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17}},
};
#define SPDO_OUT_PROFILE_COUNT (sizeof(SPDO_OUT_PROFILES) / sizeof(SPDO_OUT_PROFILES[0]))

// No verified Parts-Number/calibration -> profile mapping exists for SPDO in the database (see file header),
// so there are no auto-detection rules - the single profile above is always used.
struct SpdoOutRule { const char* prefix; int8_t profile; };
static const SpdoOutRule SPDO_OUT_RULES[] = { {"", 0} };
#define SPDO_OUT_RULE_COUNT 0
#define SPDO_OUT_DEFAULT_PROFILE 0
