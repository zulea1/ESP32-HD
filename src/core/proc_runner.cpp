/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file proc_runner.cpp
 * @brief Generic guided-procedure engine (see proc_runner.h).
 */
#include "proc_runner.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include "bcm_algo.h"      // seed->key per module for P_UNLOCK
#include "spdo_algo.h"
#include "abs_algo.h"
#include "ecm_algo.h"
#include "proc_tables.h"   // after config.h: the step tables reference HD_ECU_* ids
#include <string.h>
#include <stdio.h>

// ---- runtime state (one procedure at a time) -------------------------------
static const ProcDef *s_def = nullptr;
static bool s_active = false, s_done = false, s_aborted = false;
static int  s_pos = 0;
static char s_telem[220] = {0};
static char s_note[200]  = {0};
static char s_input[40]  = {0};   // last operator-entered value (hex digits for {V} substitution)
static char s_pumpReply[48] = {0}; // reply to the ABS pump-run command, for the pump-wait test readout

// ---- helpers ---------------------------------------------------------------
static const char *nrcText(uint8_t nrc) {
    switch (nrc) {
        case 0x12: return "sub-function not supported";
        case 0x13: return "incorrect length";
        case 0x22: return "conditions not correct";
        case 0x24: return "request sequence error";
        case 0x31: return "request out of range";
        case 0x33: return "security access denied";
        case 0x35: return "invalid key";
        case 0x7F: return "service not supported in session";
        default:   return "rejected";
    }
}

static uint8_t hexToBytes(const char *hex, uint8_t *out, uint8_t maxLen) {
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) || (n / 2) > maxLen) return 0;
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (uint8_t)(n / 2);
}

// Build the UDS payload for a command step, substituting {V} with the stored operator input (hex).
static uint8_t buildPayload(const char *tmpl, uint8_t *out, uint8_t maxLen) {
    const char *v = strstr(tmpl, "{V}");
    if (!v) return hexToBytes(tmpl, out, maxLen);
    char merged[96];
    size_t pre = (size_t)(v - tmpl);
    if (pre + strlen(s_input) + 1 >= sizeof(merged)) return 0;
    memcpy(merged, tmpl, pre);
    merged[pre] = 0;
    strncat(merged, s_input, sizeof(merged) - strlen(merged) - 1);
    strncat(merged, v + 3, sizeof(merged) - strlen(merged) - 1);   // tail after {V}
    return hexToBytes(merged, out, maxLen);
}

static bool chan(const GenInputs0200 &in, const char *name, float &v) {
    for (uint8_t i = 0; i < in.count; i++)
        if (strcmp(GEN_KEYS[in.key[i]], name) == 0) { v = in.value[i]; return true; }
    return false;
}

// Refresh s_telem from the live module's 0x22 0200 read; return front/rear wheel speed (-1 if unknown).
static bool snapshot(float &frontSpd, float &rearSpd) {
    frontSpd = rearSpd = -1.0f;
    s_telem[0] = 0;
    if (!s_def || s_def->liveModule == 0) return false;
    GenInputs0200 in;
    if (!UdsHandler::readGenInputs0200Synchronous(s_def->liveModule, in, 650) || !in.valid) {
        snprintf(s_telem, sizeof(s_telem), "no live data");
        return false;
    }
    float fc=0,fm=0,rc=0,rm=0,pump=0,fs=0,rs=0,bv=0;
    bool hfc=chan(in,"front_caliper_circuit_pressure",fc), hfm=chan(in,"front_master_cylinder_pressure",fm);
    bool hrc=chan(in,"rear_caliper_circuit_pressure",rc),  hrm=chan(in,"rear_master_cylinder_pressure",rm);
    bool hpm=chan(in,"pump_motor_status",pump);
    if (chan(in,"front_wheel_speed",fs)) frontSpd=fs;
    if (chan(in,"rear_wheel_speed",rs))  rearSpd=rs;
    chan(in,"battery_voltage",bv);
    int p=0;
    if (hfc) p+=snprintf(s_telem+p,sizeof(s_telem)-p,"F-cal %.1f ",(double)fc);
    if (hfm) p+=snprintf(s_telem+p,sizeof(s_telem)-p,"F-mc %.1f ",(double)fm);
    if (hrc) p+=snprintf(s_telem+p,sizeof(s_telem)-p,"R-cal %.1f ",(double)rc);
    if (hrm) p+=snprintf(s_telem+p,sizeof(s_telem)-p,"R-mc %.1f ",(double)rm);
    if (hpm) p+=snprintf(s_telem+p,sizeof(s_telem)-p,"pump %s ",pump>0.5f?"ON":"off");
    p+=snprintf(s_telem+p,sizeof(s_telem)-p,"spd F%.0f/R%.0f batt %.1fV",
                (double)(frontSpd<0?0:frontSpd),(double)(rearSpd<0?0:rearSpd),(double)bv);
    return true;
}

static void jsonEscape(const char *in, char *out, size_t outLen) {
    size_t q = 0;
    for (const char *c = in ? in : ""; *c && q < outLen - 3; c++) {
        if (*c == '\n')      { out[q++] = '\\'; out[q++] = 'n'; }
        else if (*c == '\r') { out[q++] = '\\'; out[q++] = 'r'; }
        else if (*c == '\t') { out[q++] = '\\'; out[q++] = 't'; }
        else {
            if (*c == '"' || *c == '\\') out[q++] = '\\';
            out[q++] = *c;
        }
    }
    out[q] = 0;
}

static void emit(char *out, size_t outLen) {
    const ProcStep *s = (s_def && s_pos >= 0 && s_pos < s_def->n) ? &s_def->steps[s_pos] : nullptr;
    bool input = (!s_done && !s_aborted && s && s->kind == P_INPUT);
    bool question = (!s_done && !s_aborted && s && s->kind == P_CONFIRM);
    const char *kind = s_aborted ? "abort" : (s_done ? "done" : (question ? "confirm" : (input ? "input" : "info")));
    const char *prompt = (s_done || s_aborted || !s) ? s_note : s->text;
    // Pressure gate for the current prompt: a P_INFO whose note is "press"/"release" asks the UI to confirm
    // the brake is applied (master-cylinder pressure high) or released (low) before enabling Continue.
    const char *gate = "";
    if (!s_done && !s_aborted && s && (s->kind == P_INFO || s->kind == P_CONFIRM) && s->note &&
        (strcmp(s->note, "press") == 0 || strcmp(s->note, "release") == 0 ||
         strcmp(s->note, "pumpwait") == 0)) gate = s->note;
    char pe[300], fld[48];
    jsonEscape(prompt, pe, sizeof(pe));
    jsonEscape((input && s->note) ? s->note : "", fld, sizeof(fld));
    snprintf(out, outLen,
        "{\"ok\":true,\"proc\":\"%s\",\"active\":%s,\"done\":%s,\"aborted\":%s,\"kind\":\"%s\","
        "\"question\":%s,\"input\":%s,\"field\":\"%s\",\"gate\":\"%s\",\"stage\":%d,\"total\":%d,"
        "\"prompt\":\"%s\",\"note\":\"%s\",\"telem\":\"%s\"}",
        s_def ? s_def->id : "", s_active?"true":"false", s_done?"true":"false", s_aborted?"true":"false",
        kind, question?"true":"false", input?"true":"false", fld, gate, s_pos, s_def?s_def->n:0,
        pe, s_note, s_telem);
}

static int findTag(uint8_t tag) {
    if (!s_def) return 0;
    for (int i = 0; i < s_def->n; i++) if (s_def->steps[i].tag == tag) return i;
    return s_def->n - 1;   // fall back to END
}

static void teardown() {
    if (!s_def) return;
    uint16_t m = s_def->liveModule ? s_def->liveModule : HD_ECU_ABS;
    uint8_t req[2] = {0x10, 0x01};
    UdsRawResult r;
    UdsHandler::rawTransaction(m, req, 2, r, 300);
    Serial.printf("[PROC:%s] SENT 10 01 -> default session (teardown).\n", s_def->id);
    s_active = false;
}

// Execute command steps forward from s_pos until a prompt/input or END. false = safety/fatal abort.
static bool present() {
    while (s_def && s_pos < s_def->n) {
        const ProcStep &s = s_def->steps[s_pos];
        uint8_t b[64]; uint8_t n;
        UdsRawResult r;
        float fs, rs;
        const char *id = s_def->id;

        switch (s.kind) {

        case P_INFO:
        case P_CONFIRM:
        case P_INPUT:
            snapshot(fs, rs);
            Serial.printf("[PROC:%s] %s \"%s\"\n", id,
                          s.kind == P_CONFIRM ? "ask:" : (s.kind == P_INPUT ? "input:" : "info:"), s.text);
            snprintf(s_note, sizeof(s_note), "%s", s.note ? s.note : "");
            return true;   // wait for the operator

        case P_SESSION:
            n = hexToBytes(s.text, b, sizeof(b));
            UdsHandler::rawTransaction(s.target, b, n, r, 450);
            Serial.printf("[PROC:%s] SENT %04X %s -> %s\n", id, s.target, s.text, (r.got_reply && r.positive) ? "session OK" : "no/neg");
            s_pos++;
            break;

        case P_SEND:
            n = hexToBytes(s.text, b, sizeof(b));
            UdsHandler::rawTransaction(s.target, b, n, r, 500);
            Serial.printf("[PROC:%s] SENT %04X %s (%s) -> %s\n", id, s.target, s.text, s.note ? s.note : "", (r.got_reply && r.positive) ? "OK" : "no/neg");
            s_pos++;
            break;

        case P_SEED: {
            n = hexToBytes(s.text, b, sizeof(b));
            bool ok = UdsHandler::rawTransaction(s.target ? s.target : HD_ECU_ABS, b, n, r, 450);
            if (ok && r.got_reply && r.positive && r.len >= 2) {
                char sh[24] = {0}; uint8_t sl = (uint8_t)(r.len - 2);
                for (uint8_t i = 0; i < sl && i < 8; i++) snprintf(sh + i * 2, sizeof(sh) - i * 2, "%02X", r.data[2 + i]);
                Serial.printf("[PROC:%s] 27 01 seed=0x%s (informational, no key sent)\n", id, sh);
            } else Serial.printf("[PROC:%s] 27 01 -> %s (ignored)\n", id, r.got_reply ? nrcText(r.nrc) : "no reply");
            s_pos++;
            break;
        }

        case P_UNLOCK: {
            uint8_t seedReq[2] = {0x27, 0x01};
            bool ok = UdsHandler::rawTransaction(s.target, seedReq, 2, r, 500);
            if (!ok || !r.got_reply || !r.positive || r.len < 2) {
                Serial.printf("[PROC:%s] 27 01 unlock: %s - continuing (writes may be denied)\n", id, r.got_reply ? nrcText(r.nrc) : "no reply");
                snprintf(s_note, sizeof(s_note), "security seed request failed"); s_pos++; break;
            }
            const uint8_t *seed = &r.data[2]; uint8_t sl = (uint8_t)(r.len - 2);
            // An all-zero seed means the module needs no unlock - do NOT send 27 02 (it would be rejected).
            bool seedZero = true;
            for (uint8_t i = 0; i < sl; i++) if (seed[i]) seedZero = false;
            if (seedZero) {
                Serial.printf("[PROC:%s] 27 01 seed=0x0000 -> no unlock required (key not sent)\n", id);
                snprintf(s_note, sizeof(s_note), "no unlock required");
                s_pos++; break;
            }
            uint8_t key[16], kl = 0; bool got = false;
            // The algorithm is set per PROCEDURE by its own mode marker, NOT by the target module:
            // e.g. FOB on 0x7E2 is mode 1 (BCM) but PIN on 0x7E2 is mode 2 (SPDO).
            // s.text selects it: "1"=BCM, "2"=SPDO, "3"=ABS, "B"=ECM (reads F1F4 flag from the target).
            char mode = (s.text && s.text[0]) ? s.text[0] : '1';
            switch (mode) {
                case '1': got = bcmComputeSecurityKey(seed, sl, key, &kl); break;
                case '2': got = spdoComputeSecurityKey(seed, sl, key, &kl); break;
                case '3': got = absComputeSecurityKey(seed, sl, key, &kl); break;
                case 'B': case 'b': {   // ECM needs the F1F4 key-flag byte
                    UdsRawResult f; uint8_t rq[3] = {0x22, 0xF1, 0xF4};
                    uint8_t flag = 0;
                    if (UdsHandler::rawTransaction(s.target, rq, 3, f, 500) && f.positive && f.len >= 5) flag = (uint8_t)(f.data[4] & 0x4A);
                    got = ecmComputeSecurityKey(flag, seed, sl, key, &kl); break;
                }
                default: Serial.printf("[PROC:%s] unknown unlock mode '%c'\n", id, mode); break;
            }
            char sh[20] = {0}; for (uint8_t i = 0; i < sl && i < 4; i++) snprintf(sh + i*2, sizeof(sh)-i*2, "%02X", seed[i]);
            if (got && kl) {
                uint8_t keyReq[18] = {0x27, 0x02}; memcpy(keyReq + 2, key, kl);
                UdsRawResult kr;
                UdsHandler::rawTransaction(s.target, keyReq, (uint8_t)(2 + kl), kr, 500);
                bool unlocked = kr.got_reply && kr.positive;
                Serial.printf("[PROC:%s] 27 01 seed=0x%s -> 27 02 -> %s%s\n", id, sh,
                              unlocked ? "UNLOCKED" : "rejected", unlocked ? "" : " (NRC follows)");
                snprintf(s_note, sizeof(s_note), unlocked ? "module unlocked" : "unlock rejected (verify algo/log)");
            } else {
                Serial.printf("[PROC:%s] 27 01 seed=0x%s but no key computed\n", id, sh);
                snprintf(s_note, sizeof(s_note), "no key computed for this module");
            }
            s_pos++;
            break;
        }

        case P_LIVEREAD:
            snapshot(fs, rs);
            Serial.printf("[PROC:%s] read %04X %s -> %s\n", id, s.target, s.text, s_telem);
            snprintf(s_note, sizeof(s_note), "live read");
            s_pos++;
            break;

        case P_ACTUATE:
            if (s_def->flags & PF_WHEEL_INTERLOCK) {
                snapshot(fs, rs);
                if ((fs > 0.5f) || (rs > 0.5f)) {
                    Serial.printf("[PROC:%s] ABORT: wheels moving (F%.0f/R%.0f) - no actuation.\n", id, (double)fs, (double)rs);
                    snprintf(s_note, sizeof(s_note), "SAFETY STOP: wheels turning (F%.0f/R%.0f km/h). Actuation blocked.", (double)fs, (double)rs);
                    s_aborted = true; teardown();
                    return false;
                }
            }
            n = buildPayload(s.text, b, sizeof(b));
            if (n) {
                // The ABS pump-motor run (2F FB 02 ..): time its reply so the popup shows WHEN it actually
                // arrives (no assumption). Give it a generous window; it returns as soon as the module replies.
                // Completion of the bleed cycle is tracked from the pump-motor status, not this reply.
                bool isPumpRun = (strncmp(s.text, "2FFB02", 6) == 0);
                uint32_t txStart = millis();
                UdsHandler::rawTransaction(s.target, b, n, r, isPumpRun ? 700 : 900);  // reply arrives in ~6 ms
                uint32_t txMs = (uint32_t)(millis() - txStart);
                const char *replyStr = (r.got_reply && r.positive) ? "OK" : (r.got_reply ? nrcText(r.nrc) : "no reply");
                Serial.printf("[PROC:%s] SENT %04X 2F %s -> %s (NRC 0x%02X, %u ms)\n", id, s.target, s.text, replyStr, r.nrc, (unsigned)txMs);
                if (isPumpRun) snprintf(s_pumpReply, sizeof(s_pumpReply), "%s after %u ms", replyStr, (unsigned)txMs);
                snprintf(s_note, sizeof(s_note), "actuated %04X %s", s.target, s.text);
            }
            snapshot(fs, rs);
            s_pos++;
            break;

        case P_WRITE: {
            n = buildPayload(s.text, b, sizeof(b));
            if (n) {
                UdsHandler::rawTransaction(s.target, b, n, r, 900);
                Serial.printf("[PROC:%s] SENT %04X 2E %s -> %s (NRC 0x%02X)\n", id, s.target, s.text,
                              (r.got_reply && r.positive) ? "OK" : (r.got_reply ? nrcText(r.nrc) : "no reply"), r.nrc);
                snprintf(s_note, sizeof(s_note), "wrote %04X (%s) -> %s", s.target, s.note ? s.note : "",
                         (r.got_reply && r.positive) ? "OK" : "rejected");
            } else {
                snprintf(s_note, sizeof(s_note), "write skipped (no/invalid value)");
            }
            s_pos++;
            break;
        }

        case P_BITWRITE: {
            // text = "RD:WRPFX:POS:VAL"  (RD=read cmd hex, WRPFX=write cmd+DID hex, POS=bit index, VAL=0/1)
            char rd[12] = "", wp[12] = ""; int pos = 0, val = 0;
            if (sscanf(s.text, "%11[0-9A-Fa-f]:%11[0-9A-Fa-f]:%d:%d", rd, wp, &pos, &val) == 4) {
                uint8_t rb[8]; uint8_t rn = hexToBytes(rd, rb, sizeof(rb));
                if (rn && UdsHandler::rawTransaction(s.target, rb, rn, r, 600) && r.positive && r.len > rn) {
                    uint8_t data[64]; uint8_t dn = 0;              // response echoes service + DID (rn bytes), data follows
                    for (uint8_t i = rn; i < r.len && dn < sizeof(data); i++) data[dn++] = r.data[i];
                    uint8_t bi = (uint8_t)(pos / 8), bm = (uint8_t)(1u << (pos % 8));
                    if (bi < dn) {
                        if (val) data[bi] |= bm; else data[bi] &= (uint8_t)~bm;
                        uint8_t wb[72]; uint8_t wn = hexToBytes(wp, wb, sizeof(wb));
                        memcpy(wb + wn, data, dn); wn = (uint8_t)(wn + dn);
                        UdsRawResult wr;
                        UdsHandler::rawTransaction(s.target, wb, wn, wr, 800);
                        Serial.printf("[PROC:%s] bit%d=%d: read %s -> wrote %s (%u B) -> %s\n", id, pos, val, rd, wp, dn,
                                      (wr.got_reply && wr.positive) ? "OK" : (wr.got_reply ? nrcText(wr.nrc) : "no reply"));
                        snprintf(s_note, sizeof(s_note), "%s bit %d set to %d (%s)", s.note ? s.note : "", pos, val,
                                 (wr.got_reply && wr.positive) ? "OK" : "rejected");
                    } else { snprintf(s_note, sizeof(s_note), "bit %d beyond config data (%uB)", pos, dn); }
                } else { Serial.printf("[PROC:%s] bit-write read %s failed\n", id, rd); snprintf(s_note, sizeof(s_note), "config read failed"); }
            }
            s_pos++;
            break;
        }

        case P_DWELL: {
            // Hold-and-monitor: keep reading the ABS live data (pressures/pump) for ~N ms so the pump-run
            // window actually watches pressure instead of sleeping. The last read is shown on the next popup.
            int ms = atoi(s.text);
            if (ms < 0) ms = 0;
            if (ms > 5000) ms = 5000;
            Serial.printf("[PROC:%s] hold + monitor pressure for %d ms\n", id, ms);
            uint32_t start = millis();
            do {
                snapshot(fs, rs);
                Serial.printf("[PROC:%s] monitor -> %s\n", id, s_telem);
            } while ((uint32_t)(millis() - start) < (uint32_t)ms);
            s_pos++;
            break;
        }

        case P_END:
            teardown();
            s_done = true;
            snprintf(s_note, sizeof(s_note), "%s complete. Module returned to default session.", s_def->name);
            return true;
        }
    }
    s_done = true;
    return true;
}

// ---- public API ------------------------------------------------------------
bool ProcRunner::isActive() { return s_active; }
const char *ProcRunner::activeId() { return s_def ? s_def->id : ""; }

bool ProcRunner::liveJson(char *out, size_t outLen) {
    if (!out || !outLen) return false;
    if (!s_active || !s_def) { snprintf(out, outLen, "{\"ok\":false,\"mc_kpa\":-1}"); return false; }
    uint16_t mod = s_def->liveModule ? s_def->liveModule : HD_ECU_ABS;
    bool rear = (strstr(s_def->id, "rear") != nullptr);
    // Use the DEDICATED ABS 0x0200 parser (the generic one needs its layout latched and returns no
    // master-cylinder pressure here). It decodes pressure directly from the ABS family map, in kPa.
    AbsInputs0200 a;
    if (UdsHandler::readAbsInputs0200Synchronous(mod, a, 400) && a.valid) {
        uint8_t idx = rear ? ABSV_REAR_MASTER_CYLINDER_PRESSURE : ABSV_FRONT_MASTER_CYLINDER_PRESSURE;
        float mc = (a.value_present & (uint16_t)(1U << idx)) ? a.values[idx] : -1.0f;
        int pump = (a.state_present & (1UL << ABSS_PUMP_MOTOR_STATUS)) && (a.state_on & (1UL << ABSS_PUMP_MOTOR_STATUS)) ? 1 : 0;
        snprintf(out, outLen, "{\"ok\":true,\"mc_kpa\":%.0f,\"pump\":%d,\"pumpreply\":\"%s\"}", (double)mc, pump, s_pumpReply);
        return true;
    }
    snprintf(out, outLen, "{\"ok\":true,\"mc_kpa\":-1,\"pump\":0,\"pumpreply\":\"%s\"}", s_pumpReply);
    return true;
}

const ProcDef *ProcRunner::find(const char *id) {
    if (!id) return nullptr;
    for (int i = 0; i < PROC_DEFS_N; i++)
        if (strcmp(PROC_DEFS[i].id, id) == 0) return &PROC_DEFS[i];
    return nullptr;
}

bool ProcRunner::start(const ProcDef *def, char *out, size_t outLen) {
    if (!def) { snprintf(out, outLen, "{\"ok\":false,\"note\":\"unknown procedure\"}"); return false; }
    s_def = def; s_active = true; s_done = false; s_aborted = false; s_pos = 0;
    s_telem[0] = 0; s_note[0] = 0; s_input[0] = 0; s_pumpReply[0] = 0;
    Serial.printf("[PROC:%s] ============ START: %s ============\n", def->id, def->name);
    uint16_t m = def->liveModule ? def->liveModule : HD_ECU_ABS;
    const uint8_t tp[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};   // prime (first frame after idle often lost)
    CanDriver::transmit(m, false, false, 8, tp);
    delay(60);
    present();
    emit(out, outLen);
    return true;
}

bool ProcRunner::step(int answer, const char *input, char *out, size_t outLen) {
    if (!s_def || !s_active || s_done || s_aborted) { emit(out, outLen); return false; }
    const ProcStep &cur = s_def->steps[s_pos];

    if (cur.kind == P_INPUT) {
        if (!input || !*input) { emit(out, outLen); return false; }    // wait for a value
        // "ascii:N" field: the operator typed text (e.g. a 5-digit PIN); store it as the hex of its ASCII
        // bytes so a following 2E...{V} write sends the characters verbatim. Otherwise the input IS hex.
        if (cur.note && strncmp(cur.note, "ascii", 5) == 0) {
            size_t q = 0;
            for (const char *p = input; *p && q + 2 < sizeof(s_input); p++) q += (size_t)snprintf(s_input + q, sizeof(s_input) - q, "%02X", (unsigned char)*p);
            s_input[q] = 0;
        } else {
            strncpy(s_input, input, sizeof(s_input) - 1); s_input[sizeof(s_input) - 1] = 0;
        }
        Serial.printf("[PROC:%s] operator input accepted (%u chars)\n", s_def->id, (unsigned)strlen(s_input));
        s_pos++;
    } else if (cur.kind == P_CONFIRM) {
        bool yes = (answer != 0);
        if (cur.tag == PT_GATE_A && !yes)       s_pos = findTag(PT_GATE_B);
        else if (cur.tag == PT_GATE_B && !yes)  s_pos = findTag(PT_FINALIZE);
        else if (cur.tag == PT_COMPLETE_Q && !yes) s_pos = s_def->n - 1;
        else                                    s_pos++;
    } else {
        if (answer == 0) { abort("operator stop", out, outLen); return false; }
        s_pos++;
    }

    if (!present()) { emit(out, outLen); return false; }
    emit(out, outLen);
    return true;
}

void ProcRunner::abort(const char *reason, char *out, size_t outLen) {
    Serial.printf("[PROC:%s] ABORT: %s\n", s_def ? s_def->id : "-", reason ? reason : "abort");
    if (s_active) teardown();
    s_aborted = true; s_active = false;
    snprintf(s_note, sizeof(s_note), "%s stopped (%s). Module returned to default session.",
             s_def ? s_def->name : "Procedure", reason ? reason : "abort");
    if (out && outLen) emit(out, outLen);
}
