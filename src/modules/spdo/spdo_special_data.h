// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// SPDO (0x7F2) special functions. Each function entry gives the read command (DID), the byte POSITION
// inside that DID's response (counted from the SID, so 3 = first data byte) and the bit formula. `pos` below
// is that position re-based to the data block (position - 3). The engine (cfg_special.cpp) reads the whole
// DID, changes only the named bits of that one byte and writes the whole block back.
//
//   menu  function                 DID     db-pos  bits   source expressions
//   01    ABS Telltale             0x0157  5       0x02   943BE0F60001 / 943BE0F60002
//         + linked second write    0x0158  3       0x01   943BE0F60003 (follows the On/Off choice)
//   02    Odometer unit            0x0158  8       0x01   943444F6001B / 943444F6001C
//   03    Instrument TPMS Option   0x0165  3       0x01   943444F60025 / 943444F6001C
//   04    Parking Brake, installed 0x0157  6       0x40   100000F60662 / 100000F60664
//   04    Parking Brake, signal    0x0158  6       0x02   100000F60663 / 100000F60665
//   05    vehicle type             0x0158  15      0x10   943444F60020 / 943444F60021 - the database's own
//         generic item name for menu 05 says "Instrument Traction Control Telltale", but its decoded option
//         labels and bit test are "Two Wheels"/"Trike": the behaviour is trusted here, not the label.
//
// NOT implemented: menu 06 "Speedo pairing" (odometer write to DID 0xF1EF) - flagged for its own legal
// sensitivity (odometer-tampering regulations); independently of that the database has no formula for its
// 17-byte write payload, only the reads around it.
#pragma once
#include <stdint.h>
#include "cfg_special.h"

static const CfgOpt SPDO_OPT_ABS_TELLTALE[] = { {"Off", 0x00, 0x00}, {"On", 0x02, 0x01} };
static const CfgOpt SPDO_OPT_ODO_UNIT[]     = { {"mile", 0x00, 0}, {"Km", 0x01, 0} };
static const CfgOpt SPDO_OPT_TPMS[]         = { {"Disable", 0x00, 0}, {"Enable", 0x01, 0} };
static const CfgOpt SPDO_OPT_PB_INSTALLED[] = { {"Installed", 0x40, 0}, {"Not Installed", 0x00, 0} };
static const CfgOpt SPDO_OPT_PB_SIGNAL[]    = { {"BCM", 0x02, 0}, {"Discrete Signal", 0x00, 0} };
static const CfgOpt SPDO_OPT_VEHICLE[]      = { {"Two Wheels", 0x00, 0}, {"Trike", 0x10, 0} };

static const CfgFn SPDO_SPC_FNS[] = {
    // key                 name                                desc                                                                                             did     pos mask  rmask did2    pos2 mask2 opts                   nopt warn
    {"abs_telltale",       "ABS Telltale",                     "ABS warning telltale on the instrument cluster.",
                                                                                                                                                                0x0157, 2,  0x02, 0x02, 0x0158, 0,   0x01, SPDO_OPT_ABS_TELLTALE, 2, ""},
    {"odometer_unit",      "Odometer unit",                    "Display unit for the odometer/trip readings on the instrument cluster.",                        0x0158, 5,  0x01, 0x01, 0,      0,   0,    SPDO_OPT_ODO_UNIT,     2, ""},
    {"tpms_option",        "Instrument TPMS Option",           "Whether the cluster displays Tire Pressure Monitoring Sensor status.",                           0x0165, 0,  0x01, 0x01, 0,      0,   0,    SPDO_OPT_TPMS,         2, ""},
    {"parking_brake",      "Parking Brake",                    "Whether a parking brake is installed (cluster side of the database's Parking Brake menu).",    0x0157, 3,  0x40, 0x40, 0,      0,   0,    SPDO_OPT_PB_INSTALLED, 2, ""},
    {"parking_brake_signal","Parking Brake signal source",     "Where the cluster takes the parking-brake state from: the BCM over CAN, or a discrete wire.",   0x0158, 3,  0x02, 0x02, 0,      0,   0,    SPDO_OPT_PB_SIGNAL,    2, ""},
    {"vehicle_type",       "Vehicle type (two wheels / trike)", "Whether the cluster is configured for a two-wheel motorcycle or a trike.",
                                                                                                                                                                0x0158, 12, 0x10, 0x10, 0,      0,   0,    SPDO_OPT_VEHICLE,      2, "This changes a vehicle-type configuration bit, not a simple display option. Only change it if you know the cluster's configured vehicle type is wrong."},
};
#define SPDO_SPC_FN_COUNT (sizeof(SPDO_SPC_FNS) / sizeof(SPDO_SPC_FNS[0]))

static const CfgProfile SPDO_SPC_PROFILES[] = { {"SPDO", SPDO_SPC_FNS, (uint8_t)SPDO_SPC_FN_COUNT} };
