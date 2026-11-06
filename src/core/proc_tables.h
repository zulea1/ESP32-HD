/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file proc_tables.h
 * @brief Guided-procedure step tables for the shared ProcRunner engine (see proc_runner.h).
 *
 * Targets: ABS 0x7E1, its hydraulic companion 0x7E2, ECM 0x7E0, TSSM/security and
 * BCM per procedure. None of these HD procedures gate on a Security Access key (27 01 is informational).
 */
#pragma once
#include "proc_runner.h"

// ======================= ABS bleed / air-purge (front & rear) =======================
// Real 2F pump/solenoid commands, no Security Access. Split into two independent menu buttons (Front, Rear).
// The common safety/setup warnings, the bike-on-stand reminder and the "do a manual bleed first" reminder
// are all shown together in ONE popup before the engine is even started (see the confirm dialog built in
// procStart(), web_src/main.js), so none of that text is repeated here as a step. Each is a complete,
// standalone circuit bleed that returns the ABS to the default session when done. After bleeding, run
// "ABS: complete installation" to clear the service flag (C1158).

// --- Front circuit (steps 0-21). Read counts, security unlock before the pump run,
//     pump/valve OFF right after the run + a short dwell. -----------------
static const ProcStep ABS_BLEED_FRONT_STEPS[] = {
    { P_SEND,    0x7E1, PT_NONE, "190209", "fault status" },
    // Enter: establish session, valve init, read IDs, informational seed.
    { P_SESSION, 0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE, 0x7E1, PT_NONE, "2FFB06000001", "valve init" },
    { P_ACTUATE, 0x7E2, PT_NONE, "2F30060030", "valve init" },
    { P_SESSION, 0x7E1, PT_NONE, "1003", "session" },
    { P_SEND,    0x7E0, PT_NONE, "22F1EF", "id" },
    { P_SEND,    0x7E1, PT_NONE, "22F1EA", "id" },
    { P_SEED,    0x7E1, PT_NONE, "2701", "seed (info)" },
    { P_INFO, 0, PT_NONE, "Apply and hold the front brake lever. Maintain steady pressure.", "press" },
    // Cycle the ABS valves while the lever is held (read x2, valve, read x8, valve, read x4, valve, read x2).
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB06808014", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB06C8C80A", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB0600000A", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_INFO, 0, PT_NONE, "Release the front brake lever.", "release" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_CONFIRM, 0, PT_NONE, "Did the front brake lever collapse then push back to normal?", "" },
    { P_INFO, 0, PT_NONE, "Next, you will be instructed to pump the front brake lever while the ABS pump motor and valves are energized.", "" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_UNLOCK,   0x7E1, PT_NONE, "3", "security unlock" },
    // Start the ABS pump bleed cycle, then WAIT (pumpwait) while the module runs its own multi-pulse
    // sequence (~40-60 s) - the UI monitors live pressure/pump status and advances when it finishes.
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB0202FF0001", "pump run" },
    { P_INFO, 0, PT_NONE, "ABS pump cycling. Pump the front brake lever repeatedly until it stops (this can take up to a minute). Once it stops, stop pumping the front brake lever.", "pumpwait" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB06000000", "pump/valve off" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_CONFIRM, 0, PT_NONE, "Were pulsations felt in the front brake lever (pressure feedback) during the test?", "" },
    { P_INFO, 0, PT_NONE, "Perform another manual hydraulic front brake bleed.", "" },
    { P_INFO, 0, PT_NONE, "Apply the front brake lever to check for firmness.", "press" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_CONFIRM, 0, PT_NONE, "Is the front brake lever firm?", "" },
    { P_INFO, 0, PT_NONE, "Verify the master cylinder reservoir fluid level. Front bleed complete.", "" },
    { P_END, 0x7E1, PT_NONE, "", "default session" },
};

// --- Rear circuit (steps 22-42). Same structure, pedal wording, rear valve masks. ----
static const ProcStep ABS_BLEED_REAR_STEPS[] = {
    { P_SEND,    0x7E1, PT_NONE, "190209", "fault status" },
    // Enter: establish session, read IDs, informational seed.
    { P_SESSION, 0x7E1, PT_NONE, "1003", "session" },
    { P_SEND,    0x7E0, PT_NONE, "22F1EF", "id" },
    { P_SEND,    0x7E1, PT_NONE, "22F1EA", "id" },
    { P_SEED,    0x7E1, PT_NONE, "2701", "seed (info)" },
    { P_INFO, 0, PT_NONE, "Apply and hold the rear brake pedal. Maintain steady pressure.", "press" },
    // Cycle the ABS valves while the pedal is held (read x2, valve, read x7, valve, read x4, valve, read x1).
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB08202014", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB0638380A", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB0600000A", "valve" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_INFO, 0, PT_NONE, "Release the rear brake pedal.", "release" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_CONFIRM, 0, PT_NONE, "Did the rear brake pedal collapse then push back to normal?", "" },
    { P_INFO, 0, PT_NONE, "Next, you will be instructed to pump the rear brake pedal while the ABS pump motor and valves are energized.", "" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_UNLOCK,   0x7E1, PT_NONE, "3", "security unlock" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB0204FF0001", "pump run" },
    { P_INFO, 0, PT_NONE, "ABS pump cycling. Pump the rear brake pedal repeatedly until it stops (this can take up to a minute). Once it stops, stop pumping the rear brake pedal.", "pumpwait" },
    { P_SESSION,  0x7E1, PT_NONE, "1003", "session" },
    { P_ACTUATE,  0x7E1, PT_NONE, "2FFB06000000", "pump/valve off" },
    { P_ACTUATE,  0x7E2, PT_NONE, "2F30060030", "valve" },
    { P_CONFIRM, 0, PT_NONE, "Were pulsations felt in the rear brake pedal (pressure feedback) during the test?", "" },
    { P_INFO, 0, PT_NONE, "Perform another manual hydraulic rear brake bleed.", "" },
    { P_INFO, 0, PT_NONE, "Apply the rear brake pedal to check for firmness.", "press" },
    { P_LIVEREAD, 0x7E1, PT_NONE, "220200", "live" },
    { P_CONFIRM, 0, PT_NONE, "Is the rear brake pedal firm?", "" },
    { P_INFO, 0, PT_NONE, "Verify the master cylinder reservoir fluid level. Rear bleed complete.", "" },
    { P_END, 0x7E1, PT_NONE, "", "default session" },
};

// ECM: TPS reset / throttle relearn, and clear adaptations + relearn, both used to be guided-procedure step
// tables here. Both are a single one-shot UDS exchange (no mid-sequence operator input), so they are now
// plain special-function actions - see EcmSpecial::runTpsReset() / runClearAdaptations() in ecm_special.cpp.

// Speedometer pairing (identity/config blocks, DID 0157/0158) used to be a guided procedure here that made
// the operator type in two 24-hex-digit blocks captured from the OLD cluster with an external diagnostic
// tool. Now that this firmware can read those blocks itself (SpdoSpecial::readIdBlocks(), consensus-voted
// against dropped frames) and write them straight back (SpdoSpecial::writeIdBlocks()), both the read and the
// write live in the single "Identity blocks (0157/0158)" special-function card instead - see spdo_special.cpp.

// ========================= Key FOB assignment (BCM) =========================
// BCM 0x7E2. This procedure DOES unlock: Security Access with the BCM algorithm (algorithm family 1)
// is required before the serial write.
// Sequence: read the current serial (0170/0173, shown to the operator as "Original" before they type the new one -
// handled by the web UI's own read-before-edit, not a step here), identify (F1EF/F190/F1EA/F193), unlock
// (BCM algorithm), then the write+activate block: clear flag (2E 0178 00) + read back, write the serial,
// status (22 0200), routine start (2F 31 01 01 - same for every FOB), set flag (2E 0178 01) + read back,
// ACTIVATE this slot (2F 31 01 08 for FOB 1 / 10 for FOB 2 - the activation bits are cumulative), status
// (22 0203), finalize (clear the flag again). This UI engine's guided-procedure popup is NOT used for FOB
// assignment any more (the BCM special-function list uses its own "Original value / new value / confirm /
// place FOB near the bike" card flow instead - see web_src/main.js fob1Assign()/fob2Assign() and
// /api/bcm/fob_write in web_server.cpp); these two tables are kept for reference/direct-id testing only.
static const ProcStep FOB_ASSIGN1_STEPS[] = {
    { P_INFO,    0,      PT_NONE, "Key FOB assignment - 1 FOB. Have the FOB and its printed serial ready.", "prompt" },
    { P_SESSION, 0x7E2,  PT_NONE, "1003",       "extended session" },
    { P_SEND,    0x7E2,  PT_NONE, "220170",     "read current FOB-1 serial (shown as Original)" },
    { P_SEND,    0x7E0,  PT_NONE, "22F1EF",     "read ECM ID" },
    { P_SEND,    0x7E2,  PT_NONE, "22F190",     "read VIN" },
    { P_SEND,    0x7E2,  PT_NONE, "22F1EA",     "read BCM ID" },
    { P_SEND,    0x7E2,  PT_NONE, "22F193",     "read security code" },
    { P_UNLOCK,  0x7E2,  PT_NONE, "1",          "Security Access unlock (BCM algorithm)" },
    { P_INFO,    0,      PT_NONE, "Place FOB close to the Motorcycle.",                                           "prompt" },
    { P_INPUT,   0,      PT_NONE, "Hold the FOB near the bike. Enter its serial (6 blocks = 12 bytes, 24 hex).", "hex:24" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "clear FOB-programming flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_WRITE,   0x7E2,  PT_NONE, "2E0170{V}",  "write FOB 1 serial (DID 0170)" },
    { P_SEND,    0x7E2,  PT_NONE, "220200",     "status" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310101",   "routine start" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017801",   "set flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310108",   "ACTIVATE FOB 1" },
    { P_SEND,    0x7E2,  PT_NONE, "220203",     "status / confirm FOB assigned" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "finalize flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_END,     0x7E2,  PT_NONE, "",           "default session" },
};

// --- two FOBs (FOB 1 = DID 0170, FOB 2 = DID 0173) - same block run twice, slot-2 values on the 2nd pass ---
static const ProcStep FOB_ASSIGN2_STEPS[] = {
    { P_INFO,    0,      PT_NONE, "Key FOB assignment - 2 FOBs. Have BOTH FOBs and their printed serials ready.", "prompt" },
    { P_SESSION, 0x7E2,  PT_NONE, "1003",       "extended session" },
    { P_SEND,    0x7E2,  PT_NONE, "220170",     "read current FOB-1 serial (shown as Original)" },
    { P_SEND,    0x7E2,  PT_NONE, "220173",     "read current FOB-2 serial (shown as Original)" },
    { P_SEND,    0x7E0,  PT_NONE, "22F1EF",     "read ECM ID" },
    { P_SEND,    0x7E2,  PT_NONE, "22F190",     "read VIN" },
    { P_SEND,    0x7E2,  PT_NONE, "22F1EA",     "read BCM ID" },
    { P_SEND,    0x7E2,  PT_NONE, "22F193",     "read security code" },
    { P_UNLOCK,  0x7E2,  PT_NONE, "1",          "Security Access unlock (BCM algorithm)" },
    { P_INFO,    0,      PT_NONE, "FOB 1: place the first FOB close to the Motorcycle.",                        "prompt" },
    { P_INPUT,   0,      PT_NONE, "FOB 1: hold the first FOB near the bike, enter its serial (24 hex).", "hex:24" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "clear FOB-programming flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_WRITE,   0x7E2,  PT_NONE, "2E0170{V}",  "write FOB 1 serial (DID 0170)" },
    { P_SEND,    0x7E2,  PT_NONE, "220200",     "status" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310101",   "routine start" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017801",   "set flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310108",   "ACTIVATE FOB 1" },
    { P_SEND,    0x7E2,  PT_NONE, "220203",     "status / confirm FOB 1 assigned" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "finalize flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_INFO,    0,      PT_NONE, "FOB 2: place the second FOB close to the Motorcycle.",                       "prompt" },
    { P_INPUT,   0,      PT_NONE, "FOB 2: hold the second FOB near the bike, enter its serial (24 hex).", "hex:24" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "clear FOB-programming flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_WRITE,   0x7E2,  PT_NONE, "2E0173{V}",  "write FOB 2 serial (DID 0173)" },
    { P_SEND,    0x7E2,  PT_NONE, "220200",     "status" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310101",   "routine start" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017801",   "set flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_ACTUATE, 0x7E2,  PT_NONE, "2F310110",   "ACTIVATE FOB 2" },
    { P_SEND,    0x7E2,  PT_NONE, "220203",     "status / confirm FOB 2 assigned" },
    { P_SEND,    0x7E2,  PT_NONE, "2E017800",   "finalize flag" },
    { P_SEND,    0x7E2,  PT_NONE, "220178",     "read back flag" },
    { P_END,     0x7E2,  PT_NONE, "",           "default session" },
};

// ===================== Anti-theft auto-arm toggle (BCM) =====================
// BCM 0x7E2: unlock (BCM) -> read 22 0138 -> write 2E 0138 <01|00>.
static const ProcStep ANTITHEFT_ON_STEPS[] = {
    { P_INFO,    0,     PT_NONE, "Enable automatic anti-theft (auto-arm) on the BCM.", "prompt" },
    { P_SESSION, 0x7E2, PT_NONE, "1003",      "extended session" },
    { P_SEND,    0x7E2, PT_NONE, "22F1EA",    "read BCM ID" },
    { P_UNLOCK,  0x7E2, PT_NONE, "1",         "Security Access unlock (BCM)" },
    { P_SEND,    0x7E2, PT_NONE, "220138",    "read current auto-arm state" },
    { P_WRITE,   0x7E2, PT_NONE, "2E013801",  "enable auto-arm (DID 0138 = 01)" },
    { P_SEND,    0x7E2, PT_NONE, "220138",    "verify" },
    { P_END,     0x7E2, PT_NONE, "",          "default session" },
};
static const ProcStep ANTITHEFT_OFF_STEPS[] = {
    { P_INFO,    0,     PT_NONE, "Disable automatic anti-theft (auto-arm) on the BCM.", "prompt" },
    { P_SESSION, 0x7E2, PT_NONE, "1003",      "extended session" },
    { P_SEND,    0x7E2, PT_NONE, "22F1EA",    "read BCM ID" },
    { P_UNLOCK,  0x7E2, PT_NONE, "1",         "Security Access unlock (BCM)" },
    { P_SEND,    0x7E2, PT_NONE, "220138",    "read current auto-arm state" },
    { P_WRITE,   0x7E2, PT_NONE, "2E013800",  "disable auto-arm (DID 0138 = 00)" },
    { P_SEND,    0x7E2, PT_NONE, "220138",    "verify" },
    { P_END,     0x7E2, PT_NONE, "",          "default session" },
};

// ===================== EITMS / ACR config toggles (ECM) =====================
// ECM 0x7E0: config bits in local-ID 0x16 (read 3C16 / write 3B16,
// KWP-on-CAN). EITMS = bit 4, ACR = bit 7. Unlock = ECM algorithm (mode 0x0B).
static const ProcStep EITMS_ON_STEPS[] = {
    { P_INFO,     0,     PT_NONE, "Enable EITMS (idle engine-temperature management) on the ECM.", "prompt" },
    { P_SESSION,  0x7E0, PT_NONE, "1003",             "extended session" },
    { P_SEND,     0x7E0, PT_NONE, "22F1EA",           "read ECM ID" },
    { P_UNLOCK,   0x7E0, PT_NONE, "B",                "Security Access unlock (ECM)" },
    { P_BITWRITE, 0x7E0, PT_NONE, "3C16:3B16:4:1",    "EITMS" },
    { P_END,      0x7E0, PT_NONE, "",                 "default session" },
};
static const ProcStep EITMS_OFF_STEPS[] = {
    { P_INFO,     0,     PT_NONE, "Disable EITMS on the ECM.", "prompt" },
    { P_SESSION,  0x7E0, PT_NONE, "1003",             "extended session" },
    { P_SEND,     0x7E0, PT_NONE, "22F1EA",           "read ECM ID" },
    { P_UNLOCK,   0x7E0, PT_NONE, "B",                "Security Access unlock (ECM)" },
    { P_BITWRITE, 0x7E0, PT_NONE, "3C16:3B16:4:0",    "EITMS" },
    { P_END,      0x7E0, PT_NONE, "",                 "default session" },
};
static const ProcStep ACR_ON_STEPS[] = {
    { P_INFO,     0,     PT_NONE, "Enable automatic compression release (ACR) valves during starting.", "prompt" },
    { P_SESSION,  0x7E0, PT_NONE, "1003",             "extended session" },
    { P_SEND,     0x7E0, PT_NONE, "22F1EA",           "read ECM ID" },
    { P_UNLOCK,   0x7E0, PT_NONE, "B",                "Security Access unlock (ECM)" },
    { P_BITWRITE, 0x7E0, PT_NONE, "3C16:3B16:7:1",    "ACR" },
    { P_END,      0x7E0, PT_NONE, "",                 "default session" },
};
static const ProcStep ACR_OFF_STEPS[] = {
    { P_INFO,     0,     PT_NONE, "Disable automatic compression release (ACR).", "prompt" },
    { P_SESSION,  0x7E0, PT_NONE, "1003",             "extended session" },
    { P_SEND,     0x7E0, PT_NONE, "22F1EA",           "read ECM ID" },
    { P_UNLOCK,   0x7E0, PT_NONE, "B",                "Security Access unlock (ECM)" },
    { P_BITWRITE, 0x7E0, PT_NONE, "3C16:3B16:7:0",    "ACR" },
    { P_END,      0x7E0, PT_NONE, "",                 "default session" },
};

// VIN-based speedometer pairing (and ECM VIN write) used to be guided-procedure step tables here. Both are a
// single read-then-write exchange (no mid-sequence operator input beyond the VIN itself), so they are now
// plain Read/Write special-function actions - see EcmSpecial::readVin()/writeVin() (ecm_special.cpp) and
// SpdoSpecial::readVin()/writeVin() (spdo_special.cpp, "VIN write (Speedometer)" card).

// ===================== ABS: complete installation ===========================
// ABS 0x7E1 (020002D30004 final step): after an ABS bleed/replacement, write 2E 0151 00 to clear
// the "service required" flag (DTC C1158) and mark the installation complete.
static const ProcStep ABS_COMPLETE_STEPS[] = {
    { P_INFO,    0,     PT_NONE, "ABS complete installation: clears the service-required flag (C1158) after an ABS replacement/bleed. Do this only once the bleed is done.", "prompt" },
    { P_SESSION, 0x7E1, PT_NONE, "1003",      "extended session" },
    { P_WRITE,   0x7E1, PT_NONE, "2E015100",  "mark ABS installation complete (DID 0151 = 00)" },
    { P_SEND,    0x7E1, PT_NONE, "220151",    "verify" },
    { P_END,     0x7E1, PT_NONE, "",          "default session" },
};

// (The Hand Control Cruise / Push-to-Talk swap is NOT a guided procedure: it is a single two-value config
// option handled directly by the /api/lhc/special read/run/restore endpoints in web_server.cpp.)

// ============================ procedure registry ============================
static const ProcDef PROC_DEFS[] = {
    { "abs_bleed_front",
      "ABS bleed - Front circuit",
      "Guided front hydraulic bleed, real 2F pump/solenoid commands to the ABS (0x7E1) and its hydraulic "
      "companion (0x7E2). No Security Access needed. Keep the bike stationary, ignition on.",
      HD_ECU_ABS, PF_WHEEL_INTERLOCK, ABS_BLEED_FRONT_STEPS, (int)(sizeof(ABS_BLEED_FRONT_STEPS)/sizeof(ABS_BLEED_FRONT_STEPS[0])) },
    { "abs_bleed_rear",
      "ABS bleed - Rear circuit",
      "Guided rear hydraulic bleed, real 2F pump/solenoid commands to the ABS (0x7E1) and its hydraulic "
      "companion (0x7E2). No Security Access needed. Keep the bike stationary, ignition on.",
      HD_ECU_ABS, PF_WHEEL_INTERLOCK, ABS_BLEED_REAR_STEPS, (int)(sizeof(ABS_BLEED_REAR_STEPS)/sizeof(ABS_BLEED_REAR_STEPS[0])) },

    // "ecm_tps_reset" and "ecm_relearn" used to be guided procedures here. Both are a single one-shot UDS
    // exchange with nothing for the operator to do mid-sequence, so they are now plain special-function
    // actions (EcmSpecial::runTpsReset() / runClearAdaptations(), ecm_special.cpp) with their own Run button
    // and confirmation popup, instead of the multi-step popup wizard.

    // "spdo_pair" and "spdo_pair_vin" used to be guided procedures here - both now live in the "Identity
    // blocks (0157/0158)" and "VIN write (Speedometer)" special-function cards instead, see above.

    { "fob_assign_1", "Key FOB assignment (1 FOB)",
      "Unlocks the BCM (0x7E2, Security Access mode 1) and programs ONE key FOB serial (DID 0170), then runs "
      "the routine-start/activate sequence (2F 31 01 01, then 2F 31 01 08). Serial is printed on the FOB.",
      0, PF_NONE, FOB_ASSIGN1_STEPS, (int)(sizeof(FOB_ASSIGN1_STEPS)/sizeof(FOB_ASSIGN1_STEPS[0])) },

    { "fob_assign_2", "Key FOB assignment (2 FOBs)",
      "Unlocks the BCM (0x7E2, Security Access mode 1) and programs TWO key FOB serials (DID 0170 then 0173), "
      "each with its own routine-start/activate pass (2F 31 01 01, then 08/10). Enter each FOB's printed serial when prompted.",
      0, PF_NONE, FOB_ASSIGN2_STEPS, (int)(sizeof(FOB_ASSIGN2_STEPS)/sizeof(FOB_ASSIGN2_STEPS[0])) },

    { "antitheft_on",  "Auto-arm: enable",  "Enable automatic anti-theft (BCM DID 0138=01) after BCM unlock.",
      0, PF_NONE, ANTITHEFT_ON_STEPS,  (int)(sizeof(ANTITHEFT_ON_STEPS)/sizeof(ANTITHEFT_ON_STEPS[0])) },
    { "antitheft_off", "Auto-arm: disable", "Disable automatic anti-theft (BCM DID 0138=00) after BCM unlock.",
      0, PF_NONE, ANTITHEFT_OFF_STEPS, (int)(sizeof(ANTITHEFT_OFF_STEPS)/sizeof(ANTITHEFT_OFF_STEPS[0])) },

    { "eitms_on",  "EITMS: enable",  "Enable EITMS (ECM config bit 4 of local-ID 0x16, 3C16/3B16) after ECM unlock.",
      0, PF_NONE, EITMS_ON_STEPS,  (int)(sizeof(EITMS_ON_STEPS)/sizeof(EITMS_ON_STEPS[0])) },
    { "eitms_off", "EITMS: disable", "Disable EITMS (ECM config bit 4 of local-ID 0x16) after ECM unlock.",
      0, PF_NONE, EITMS_OFF_STEPS, (int)(sizeof(EITMS_OFF_STEPS)/sizeof(EITMS_OFF_STEPS[0])) },
    { "acr_on",  "ACR: enable",  "Enable automatic compression release (ECM config bit 7 of local-ID 0x16) after ECM unlock.",
      0, PF_NONE, ACR_ON_STEPS,  (int)(sizeof(ACR_ON_STEPS)/sizeof(ACR_ON_STEPS[0])) },
    { "acr_off", "ACR: disable", "Disable automatic compression release (ECM config bit 7 of local-ID 0x16) after ECM unlock.",
      0, PF_NONE, ACR_OFF_STEPS, (int)(sizeof(ACR_OFF_STEPS)/sizeof(ACR_OFF_STEPS[0])) },

    // "vin_write_ecm" used to be a guided procedure here. It is now a plain Read/Write special-function action
    // (EcmSpecial::readVin() / writeVin(), ecm_special.cpp) instead of the multi-step popup wizard.

    { "abs_complete", "ABS: complete installation",
      "Clears the ABS service-required flag / C1158 (2E 0151 00) after an ABS replacement and bleed.",
      0, PF_NONE, ABS_COMPLETE_STEPS, (int)(sizeof(ABS_COMPLETE_STEPS)/sizeof(ABS_COMPLETE_STEPS[0])) },
};
static const int PROC_DEFS_N = (int)(sizeof(PROC_DEFS)/sizeof(PROC_DEFS[0]));
