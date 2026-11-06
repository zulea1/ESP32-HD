/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file proc_runner.h
 * @brief Generic data-driven guided-procedure engine (shared by every module's multi-step special
 *        functions: ABS bleed, speedo/radio pairing, throttle sweep, PIN/FOB programming, ...).
 *
 * A procedure is a flat, ordered ProcStep[] table. The engine walks it one operator step at a time,
 * driven from the web UI (start -> step(answer/input) -> ... -> done), honouring on-screen prompts
 * before sending anything. It transmits real UDS: session (0x10), reads (0x22 incl. live 0x0200),
 * 2F InputOutputControl actuations, 2E writes, and an informational 0x27 01 seed read (no key is
 * ever sent - none of these HD procedures gate on a Security Access unlock). An optional wheel-speed
 * interlock blocks actuation while the bike is moving. Only one procedure runs at a time.
 */
#pragma once
#include <Arduino.h>

enum ProcKind : uint8_t {
    P_INFO = 0,   // operator instruction            -> Continue
    P_CONFIRM,    // operator yes/no question         -> Yes / No
    P_INPUT,      // operator enters a value          -> text/hex/number; stored for a later P_WRITE
    P_SESSION,    // 10 <sub> session                 -> SENT to target
    P_SEND,       // generic UDS (19/22F1xx/31/...)   -> SENT, logged (informational)
    P_SEED,       // 27 01 request seed               -> SENT, seed logged; NO key sent, never gates
    P_UNLOCK,     // 27 01 -> compute key (by target module algo) -> 27 02  (real Security Access unlock)
    P_LIVEREAD,   // 22 0200 live data                -> read + telemetry snapshot
    P_ACTUATE,    // 2F InputOutputControl            -> SENT (after optional wheel interlock)
    P_WRITE,      // 2E WriteDataByIdentifier         -> SENT; text may contain {V} = operator input
    P_BITWRITE,   // read-modify-write ONE bit of a config block. text="RD:WRPFX:POS:VAL" e.g. "3C16:3B16:4:1"
                  //  -> read RD, flip bit POS to VAL in the returned data, write WRPFX+data (KWP 3C/3B or UDS 22/2E)
    P_DWELL,      // non-blocking-looking hold: delay text=milliseconds (no CAN), e.g. pump-run dwell
    P_END         // 10 01 default session            -> SENT, procedure complete
};

// branch tags on CONFIRM steps: GATE_A "No" jumps to the GATE_B step; GATE_B "No" jumps to FINALIZE;
// COMPLETE_Q "No" jumps to END. Plain CONFIRMs just continue.
enum ProcTag : uint8_t { PT_NONE = 0, PT_GATE_A, PT_GATE_B, PT_FINALIZE, PT_COMPLETE_Q };

enum ProcFlags : uint8_t { PF_NONE = 0, PF_WHEEL_INTERLOCK = 1 };

struct ProcStep {
    uint8_t     kind;
    uint16_t    target;   // CAN id for command steps (0 for prompts/input)
    uint8_t     tag;
    const char *text;     // prompt (INFO/CONFIRM/INPUT) or UDS hex payload (command steps; {V}=input)
    const char *note;     // log/UI description; for P_INPUT: field spec "hex:N" | "num:MIN:MAX" | "text:N"
};

struct ProcDef {
    const char     *id;          // stable key, e.g. "abs_bleed" / "spdo_pair"
    const char     *name;        // UI title
    const char     *desc;        // UI description
    uint16_t        liveModule;  // module id for the 0x0200 telemetry snapshot (0 = none)
    uint8_t         flags;       // ProcFlags
    const ProcStep *steps;
    int             n;
};

class ProcRunner {
public:
    static const ProcDef *find(const char *id);         // look a procedure up by id
    static bool start(const ProcDef *def, char *out, size_t outLen);
    static bool step(int answer, const char *input, char *out, size_t outLen);
    static void abort(const char *reason, char *out, size_t outLen);
    static bool isActive();
    static const char *activeId();
    // Live master-cylinder pressure of the active procedure's circuit (front/rear), for the UI pressure gate.
    // JSON: {"ok":bool,"mc_kpa":<float, -1 if unavailable>,"pump":0|1}. Safe to call between steps.
    static bool liveJson(char *out, size_t outLen);
};
