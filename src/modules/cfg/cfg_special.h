/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file cfg_special.h
 * @brief Generic "configuration option" special-function engine, shared by the SPDO (0x7F2) and RAD (0x7F1)
 *        special-function pages (spdo_special.h / rad_special.h are thin static wrappers around one
 *        instance each).
 *
 * This is the BCM special-function pattern (bcm_special.cpp) generalised in two ways the diagnostic
 * database turned out to need for these modules:
 *
 *   1. Byte position. A function's bits live at a given *position* inside a multi-byte DID, not always in
 *      the first data byte (the database's expression table stores, per function, the read command, the
 *      byte position counted from the response SID, and the bit formula - e.g. SPDO "Odometer unit" is
 *      DID 0x0158 position 8 = the 6th data byte). `pos` below is that position re-based to the first data
 *      byte (database position - 3).
 *   2. Whole-block write. Because of (1), changing an option reads the entire DID block, replaces only the
 *      named bits of the one byte at `pos`, and writes the entire block back (2E <DID> <all bytes>) -
 *      multi-frame when the block needs it. The read tells us the block's real length, so no length is
 *      hard-coded anywhere.
 *
 * As in bcm_special.cpp: security access (0x27) when the module asks for it, read-back verification after
 * every write with the previous byte put back if the check fails, and the original byte remembered per
 * function so "Restore original" can undo a change (only that function's own bits are restored).
 */
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

struct UdsRawResult;

struct CfgOpt { const char* text; uint8_t val; uint8_t val2; };    // val2: value for the linked field (if any)

struct CfgFn {
    const char* key;
    const char* name;
    const char* desc;
    uint16_t    did;        // DID holding the option
    uint8_t     pos;        // 0-based index of the byte inside the DID's data block
    uint8_t     mask;       // bits written by this function
    uint8_t     rmask;      // bits compared when identifying the current option (>= mask)
    uint16_t    did2;       // optional linked second field written in step with the choice (0 = none)
    uint8_t     pos2;
    uint8_t     mask2;
    const CfgOpt* opts;
    uint8_t     nopt;
    const char* warn;       // non-empty: needs confirm=1
};

struct CfgProfile { const char* name; const CfgFn* fns; uint8_t count; };
struct CfgRule    { const char* prefix; int8_t profile; };

// preUnlockBytes/preUnlockLength: the response data of `preUnlockDid` below, read right before the 27 01 seed
// request (empty/0 when the module has no preUnlockDid, or that read failed) - e.g. RAD compares its seed
// against this block instead of using a seed/key algorithm.
// outputMessage/outputMessageLength: optional - on a false return, computeKey may write a specific reason here
// (e.g. RAD's seed-vs-DID mismatch, with both values) and the engine reports that instead of its generic
// "no known key" message. Leaving outputMessage untouched (empty) keeps the generic message.
typedef bool (*CfgKeyFn)(const uint8_t *seedBytes, uint8_t seedLength, uint8_t *outputKeyBytes, uint8_t *outputKeyLength,
                          const uint8_t *preUnlockBytes, uint8_t preUnlockLength,
                          char *outputMessage, size_t outputMessageLength);

struct CfgModuleSpec {
    uint32_t        ecuId;            // CAN request id of the module
    const char*     tag;              // short module name for messages / log ("SPDO")
    const char*     jsonType;         // "spdo_spc"
    const char*     securityFile;     // where to add captured seed/key pairs ("spdo_security.h")
    const CfgProfile* profiles;
    uint8_t         profileCount;
    const CfgRule*  rules;            // identification-string prefix -> profile (longest prefix wins)
    uint16_t        ruleCount;
    int8_t          defaultProfile;   // used when nothing matches (-1 = none)
    uint16_t        idDid;            // DID read to identify the unit (0xF1EA parts number); 0 = readFamilyId()
    uint16_t        preUnlockDid;     // DID the database reads right before requesting the seed (0 = none)
    CfgKeyFn        computeKey;
    bool            unlockUpFront;    // true = this module's writes need Security Access, so unlock BEFORE the
                                      // first write (e.g. RADIO). false = unlock lazily, only if a write is
                                      // rejected with NRC 0x33 (the default for every other module).
};

#define CFG_MAX_FNS   32
#define CFG_MAX_BLOCK 56   // longest DID data block handled (2E + DID + block must stay <= 60 bytes)

class CfgSpecialEngine {
public:
    explicit CfgSpecialEngine(const CfgModuleSpec &spec);
    void   reset();
    bool   pageActive();
    bool   enter(char *statusMessage, size_t statusMessageLength);
    void   exitPage(const char *reason);
    void   closeSession();    // drop to the default session (10 01) if one is open, keep the page open
    bool   read(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    bool   run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength);
    bool   restore(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    bool   setProfile(int profileIndex);                 // -1 = automatic (by identification string)
    void   loop();
    size_t catalogJson(char *outputBuffer, size_t maxOutputLength);

    // Generic helpers for "custom card" functions that don't fit the bit-option model above (e.g. a 17-char
    // ASCII VIN, or a plain identity block to display). Same session/unlock/verify handling as read()/run().
    bool   readVin(uint16_t did, char *outputBuffer, size_t outputBufferLength);                           // 22 <did> -> JSON {ok,msg,vin}
    bool   writeVin(uint16_t did, const char *vin, char *outputBuffer, size_t outputBufferLength);         // unlock + 2E <did> <17 ASCII> + verify -> JSON {ok,msg,vin}
    // 22 <did> -> hex text (no unlock - plain read). A multi-byte block needs ISO-TP multi-frame, and a
    // dropped Consecutive Frame over the flaky bridge still looks like a valid, positive reply - just a short
    // or corrupted one - so a single reply (even a full-length one) is never trusted blindly here. Reads up
    // to 6 times and accepts the value only once at least 3 of those reads agree byte-for-byte (that is what a
    // clean, undropped read looks like); `minLength` (the expected full block length from the diagnostic
    // database) only decides whether the agreed-on value is reported as complete or as a short/true block.
    bool   readHexBlock(uint16_t did, uint8_t minLength, char *hexOutput, size_t hexOutputLength, char *statusMessage, size_t statusMessageLength);
    // unlock + 2E <did> <hex bytes decoded from hexInput> + read-back verify (a few tries, same multi-frame
    // tolerance as readHexBlock) -> statusMessage. hexInput is plain hex text (e.g. from readHexBlock's output
    // or a saved/loaded file), no "0x" prefix or separators.
    bool   writeHexBlock(uint16_t did, const char *hexInput, char *statusMessage, size_t statusMessageLength);

private:
    struct Block { uint8_t len; uint8_t bytes[CFG_MAX_BLOCK]; };

    const CfgModuleSpec &spec_;
    bool     pageActive_ = false, sessionActive_ = false, sessionUnlocked_ = false, profileManuallySet_ = false;
    int      activeProfile_ = -1;
    uint32_t lastTesterPresentMs_ = 0, lastOperationMs_ = 0;
    char     msg_[160] = {0};
    char     idString_[48] = {0};
    int16_t  originalByte_[CFG_MAX_FNS];      // original byte at pos (-1 = not read yet)
    int16_t  originalByte2_[CFG_MAX_FNS];     // original byte of the linked field

    void   setMsg(const char *formatText, ...) __attribute__((format(printf, 2, 3)));
    bool   tx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult);
    bool   ensureSession();
    bool   unlock();
    int    classify(const char *idString) const;
    const CfgProfile *profile() const;
    int    findFn(const char *testKey) const;
    bool   readBlock(const CfgFn &fn, uint16_t did, uint8_t pos, Block &block);
    bool   writeBlock(const CfgFn &fn, uint16_t did, const Block &block);
    bool   writeField(const CfgFn &fn, uint16_t did, uint8_t pos, uint8_t mask, uint8_t value, Block &block);
    int    optionOf(const CfgFn &fn, uint8_t currentByte) const;
    size_t reply(char *outputBuffer, size_t maxOutputLength, bool succeeded, const char *extraJson);
};
