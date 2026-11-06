// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// GENERATED reference table - RAD (0x7F1, Radio / Boom!Box) special functions.
// Each function entry gives the read command (DID), the byte position inside that DID's response (3 = first
// data byte) and the bit formula. `pos` below is that position re-based to the data block (position - 3).
// Engine: cfg_special.cpp (whole-block read-modify-write of the one byte).
//
// Two radio generations, chosen by the radio's Parts Number (DID 0xF1EA), part numbers paired with variants:
//   profile 0 "Boom!Box (classic)" - P/N 76000075C and anything unknown (DEFAULT)   [16 items]
//   profile 1 "Boom!Box GTS"       - P/N 76000779 .. 76000797 (10 part numbers)     [19 fields]
//
// DID map (both generations agree wherever both define a field):
//   0xF1E8  byte 0 = speaker configuration code (enum)          byte 1 = feature bits: CB 0x02, CB freq 0x01
//           (FM=1), Navigation 0x08, Intercom 0x04, Wireless headset 0x40, TPMS 0x80
//           GTS only: byte 3 bit 0x01 = Digital Audio Broadcast, byte 4 low nibble = CarPlay mic (1 wired, 4 wireless)
//   0x0102  region code (0..9)    0x0109 Bluetooth    0x010A rear speaker switch    0x010C SMS text messaging
//   0x0100 / 0x0101  GTS: Front / Rear audio output (Yes/No)  =  classic: AMP 1 / AMP 2 installed
//   0x0103 / 0x0104 / 0x0105  GTS: Audio input 1 / 2 / 3       0x010B classic: XM 0x02, HD Radio 0x04, DAB 0x08
//
// Deliberately NOT implemented:
//   - GTS "Apple Siri": its write formula depends on the CarPlay nibble in a way the display
//     formula does not mirror (asymmetric set/clear of bits 0x10/0x40) - too ambiguous to write safely.
//   - "Adjust Radio for Aftermarket Amp with 2 / 4+ speakers": both menu entries carry the identical byte
//     sequence (speaker code 0x00, AMP1=1, AMP2=1) and there is no reliable way to tell them apart.
//     The same result can be set through the individual fields above anyway.
//   - "Not Configured" (0xFF) as a *writable* speaker code on the classic radio - reading shows it, writing
//     0xFF on purpose is not offered.
#pragma once
#include <stdint.h>
#include "cfg_special.h"

static const CfgOpt RAD_OPT_SPEAKER_GTS[] = {
    {"4 Speakers FLHT original equipment system, Upper Fairing / Rear Pod [0x00]", 0x00, 0},
    {"2 Speakers FLHX original equipment system, Upper Fairing [0x01]", 0x01, 0},
    {"4 Speakers, FLTR original equipment system, Upper Fairing and Rear Pod [0x02]", 0x02, 0},
    {"2 Speakers, FLTR original equipment system, Upper Fairing [0x03]", 0x03, 0},
    {"4 Speakers, 1 Amp, Upper Fairing / Rear Pod [0x0E]", 0x0E, 0},
    {"4 Speakers, 1 Amp, Upper Fairing / Rear Pod [0x0F]", 0x0F, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Saddle Bag [0x10]", 0x10, 0},
    {"CVO ONLY -- 6 Speakers, 3 Amp, Upper Fairing / Lower Fairing / Saddle Bag [0x11]", 0x11, 0},
    {"2 Speakers, 1 Amp, Upper Fairing [0x80]", 0x80, 0},
    {"4 Speakers, 1 Amp, Upper Fairing / Lower Fairing [0x82]", 0x82, 0},
    {"4 Speakers, 1 Amp, Upper Fairing / Saddle Bag [0x83]", 0x83, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod [0x84]", 0x84, 0},
    {"6 Speakers, 2 Amps, Upper Fairing / Saddle Bag / Rear Pod [0x85]", 0x85, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Saddle Bag [0x86]", 0x86, 0},
    {"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod [0x87]", 0x87, 0},
    {"Stage II, 2 Speakers, 1 Amp, Upper Fairing [0x90]", 0x90, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Lower Fairing [0x91]", 0x91, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Rear Pod [0x92]", 0x92, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Rear Pod [0x93]", 0x93, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Saddle Bag / Rear Pod [0x94]", 0x94, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Saddle Bag [0x95]", 0x95, 0},
    {"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod [0x96]", 0x96, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod, For use with Twin Cooled fairings [0xB0]", 0xB0, 0},
    {"8 Speakers, 2 Amps , Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings [0xB1]", 0xB1, 0},
    {"6 Speakers, 3 Amps, Upper / Lower Fairing / Saddle Bag, For use with Twin Cooled fairings [0xB2]", 0xB2, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Rear Pod, For use with Twin Cooled fairings [0xB3]", 0xB3, 0},
    {"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle, Bag / Rear Pod, For use with Twin Cooled fairings [0xB4]", 0xB4, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod, For use with Twin Cooled fairings [0xB5]", 0xB5, 0},
    {"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings [0xB6]", 0xB6, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Rear Pod, For use with Twin Cooled fairings [0xB7]", 0xB7, 0},
    {"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings [0xB8]", 0xB8, 0},
    {"2 Speakers, 1 Amp, Upper Fairing [0xC0]", 0xC0, 0},
    {"4 Speakers, 1 Amp, Upper Fairing/Lower Fairing [0xC2]", 0xC2, 0},
    {"4 Speakers, 1 Amp, Upper Fairing / Saddle Bag [0xC3]", 0xC3, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod [0xC4]", 0xC4, 0},
    {"6 Speakers, 2 Amps, Upper Fairing / Saddle Bag / Rear Pod [0xC5]", 0xC5, 0},
    {"6 Speakers, 2 Amps, Upper / Lower Fairing / Saddle Bag [0xC6]", 0xC6, 0},
    {"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod [0xC7]", 0xC7, 0},
    {"Stage II, 2 Speakers, 1 Amp, Upper Fairing [0xD0]", 0xD0, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Lower Fairing [0xD1]", 0xD1, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Rear Pod [0xD2]", 0xD2, 0},
    {"4 Speakers, 2 Amps, Upper Fairing / Saddle Bag [0xD3]", 0xD3, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Rear Pod [0xD4]", 0xD4, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Saddle Bag / Rear Pod [0xD5]", 0xD5, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Saddle Bag [0xD6]", 0xD6, 0},
    {"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bags / Rear Pod [0xD7]", 0xD7, 0},
    {"6 Speakers, 2 Amps, Upper Fairing / Rear Pod / Body Mount [0xE0]", 0xE0, 0},
    {"6 Speakers, 3 Amps, Upper Fairing / Rear Pod / Body Mount [0xE1]", 0xE1, 0},
};
static const CfgOpt RAD_OPT_SPEAKER_CLASSIC[] = {
    {"4 Speakers - Upper Fairing / Rear Pods / No AMP [0x00]", 0x00, 0},
    {"2 Speaker original equipment system with Upper Fairing speakers installed and no Amplifier module. [0x01]", 0x01, 0},
    {"4 Speakers - Upper Fairing / Rear Pods / No AMP ( Tri Glide ) [0x08]", 0x08, 0},
    {"2 Speakers - Upper Fairing / No AMP ( Tri Glide ) [0x09]", 0x09, 0},
    {"4 Speakers - Upper Fairing / Rear Pods / 1 AMP [0x0E]", 0x0E, 0},
    {"6 Speakers - Upper Fairing / Mid-Combo / 2 AMPS [0x10]", 0x10, 0},
    {"2 Speakers - Upper Fairing / 1 AMP [0x80]", 0x80, 0},
    {"4 Speakers - Upper Fairing / Rear Pods / 1 AMP [0x81]", 0x81, 0},
    {"4 Speakers - Upper / Lower Fairing / 1 AMP [0x82]", 0x82, 0},
    {"4 Speakers - Upper Fairing / Saddle Bag / 1 AMP [0x83]", 0x83, 0},
    {"6 Speakers - Upper / Lower Fairing / Rear Pods / 2 AMPS [0x84]", 0x84, 0},
    {"6 Speakers - Upper Fairing / Saddle Bags / Rear Pods / 2 AMPS [0x85]", 0x85, 0},
    {"6 Speakers - Upper / Lower Fairing / Saddle Bags / 2 AMPS [0x86]", 0x86, 0},
    {"8 Speakers - Upper / Lower Fairing / Saddle Bags / Rear Pods / 2 AMPS [0x87]", 0x87, 0},
};
static const CfgOpt RAD_OPT_REGION[] = {
    {"Not Configured", 0x00, 0},
    {"North America", 0x01, 0},
    {"Japan", 0x02, 0},
    {"Australia", 0x03, 0},
    {"HDI/England/India", 0x04, 0},
    {"South America", 0x05, 0},
    {"Russia", 0x06, 0},
    {"New Zeeland", 0x07, 0},
    {"Mexico", 0x08, 0},
    {"Central America", 0x09, 0},
};
static const CfgOpt RAD_OPT_YES_NO[]       = { {"Yes", 0x01, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_EN_DIS[]       = { {"Enabled", 0x01, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_CB[]           = { {"Yes", 0x02, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_CB_FREQ[]      = { {"FM", 0x01, 0}, {"AM", 0x00, 0} };
static const CfgOpt RAD_OPT_NAV_GTS[]      = { {"Enabled", 0x08, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_NAV_CLASSIC[]  = { {"Internal", 0x08, 0}, {"External", 0x00, 0} };
static const CfgOpt RAD_OPT_INTERCOM_EN[]  = { {"Enabled", 0x04, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_INTERCOM_YN[]  = { {"Yes", 0x04, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_WHIM_EN[]      = { {"Enabled", 0x40, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_WHIM_YN[]      = { {"Yes", 0x40, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_TPMS[]         = { {"Yes", 0x80, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_AUDIO_IN1[]    = { {"External CB or other input device", 0x01, 0}, {"Internal CB", 0x00, 0} };
static const CfgOpt RAD_OPT_AUDIO_IN3[]    = { {"Yes", 0x01, 0}, {"Interrupt tone detect only", 0x02, 0}, {"Interrupt tone detect and add device to mode list", 0x03, 0}, {"No", 0x00, 0} };
static const CfgOpt RAD_OPT_CARPLAY_MIC[]  = { {"Wired", 0x01, 0}, {"Wireless", 0x04, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_XM[]           = { {"Enabled", 0x02, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_HDRADIO[]      = { {"Enabled", 0x04, 0}, {"Disabled", 0x00, 0} };
static const CfgOpt RAD_OPT_DAB_CLASSIC[]  = { {"Enabled", 0x08, 0}, {"Disabled", 0x00, 0} };

#define RAD_NOPT(a) ((uint8_t)(sizeof(a) / sizeof(a[0])))
#define RAD_SPEAKER_WARN "Speaker configuration tells the radio which speaker/amplifier set is fitted. A wrong value can mute outputs or drive speakers incorrectly - choose the entry that matches the installed hardware."

// ---- profile 1: Boom!Box GTS (database set 943AB0, 19-field "Radio configuration" screen) ----
static const CfgFn RAD_SPC_FNS_GTS[] = {
    // key                   name                               desc                                                                 did     pos mask  rmask did2 pos2 mask2 opts                  nopt                          warn
    {"speaker_config",       "Speaker Configuration",           "Fitted speaker / amplifier set.",                                  0xF1E8, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_SPEAKER_GTS,     RAD_NOPT(RAD_OPT_SPEAKER_GTS),     RAD_SPEAKER_WARN},
    {"region",               "Radio region code",               "Sales region the radio is configured for (tuner bands etc.).",     0x0102, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_REGION,          RAD_NOPT(RAD_OPT_REGION),          "Changing the region alters tuner band plans and regulatory settings. Only set the region the motorcycle is actually used in."},
    {"dab",                  "Digital Audio Broadcast",         "DAB tuner enable.",                                                  0xF1E8, 3,  0x01, 0x01, 0, 0, 0, RAD_OPT_EN_DIS,          2, ""},
    {"rear_speaker_switch",  "Rear Speaker Switch Installed",   "Whether a rear speaker on/off switch is fitted.",                   0x010A, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"navigation",           "Navigation",                      "Navigation function enable.",                                        0xF1E8, 1,  0x08, 0x08, 0, 0, 0, RAD_OPT_NAV_GTS,         2, ""},
    {"tpms",                 "TPMS Enable / Disable",           "Tire pressure monitoring display on the radio.",                    0xF1E8, 1,  0x80, 0x80, 0, 0, 0, RAD_OPT_TPMS,            2, ""},
    {"front_audio_output",   "Front Audio Output",              "Front audio output present.",                                        0x0100, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"rear_audio_output",    "Rear Audio Output",               "Rear audio output present.",                                         0x0101, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"audio_input_1",        "Audio Input 1",                   "What is connected to audio input 1.",                                0x0103, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_AUDIO_IN1,       2, ""},
    {"audio_input_2",        "Audio Input 2",                   "Audio input 2 present.",                                             0x0104, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"audio_input_3",        "Audio Input 3",                   "Audio input 3 mode.",                                                0x0105, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_AUDIO_IN3,       4, ""},
    {"cb",                   "CB",                              "CB radio fitted.",                                                   0xF1E8, 1,  0x02, 0x02, 0, 0, 0, RAD_OPT_CB,              2, ""},
    {"cb_frequency",         "CB Frequency",                    "CB modulation (FM / AM).",                                           0xF1E8, 1,  0x01, 0x01, 0, 0, 0, RAD_OPT_CB_FREQ,         2, ""},
    {"intercom",             "Intercom",                        "Rider / passenger intercom enable.",                                 0xF1E8, 1,  0x04, 0x04, 0, 0, 0, RAD_OPT_INTERCOM_EN,     2, ""},
    {"wireless_headset",     "Wireless Headset Interface Module","WHIM fitted / enabled.",                                            0xF1E8, 1,  0x40, 0x40, 0, 0, 0, RAD_OPT_WHIM_EN,         2, ""},
    {"bluetooth",            "Bluetooth",                       "Bluetooth enable.",                                                  0x0109, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_EN_DIS,          2, ""},
    {"carplay_mic",          "Apple CarPlay Mic Type",          "Microphone used for CarPlay.",                                     0xF1E8, 4,  0x05, 0x0F, 0, 0, 0, RAD_OPT_CARPLAY_MIC,     3, ""},
    {"sms",                  "SMS Text Messaging",              "SMS text messaging feature enable.",                                 0x010C, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_EN_DIS,          2, ""},
};

// ---- profile 0: Boom!Box classic (database set 943954, one menu item per setting) ----
static const CfgFn RAD_SPC_FNS_CLASSIC[] = {
    {"region",               "Radio region code",               "Sales region the radio is configured for (tuner bands etc.).",     0x0102, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_REGION,          RAD_NOPT(RAD_OPT_REGION),          "Changing the region alters tuner band plans and regulatory settings. Only set the region the motorcycle is actually used in."},
    {"speaker_config",       "Speaker Configuration",           "Fitted speaker / amplifier set.",                                  0xF1E8, 0,  0xFF, 0xFF, 0, 0, 0, RAD_OPT_SPEAKER_CLASSIC, RAD_NOPT(RAD_OPT_SPEAKER_CLASSIC), RAD_SPEAKER_WARN},
    {"cb",                   "CB",                              "CB radio fitted.",                                                   0xF1E8, 1,  0x02, 0x02, 0, 0, 0, RAD_OPT_CB,              2, ""},
    {"cb_frequency",         "CB Frequency",                    "CB modulation (FM / AM).",                                           0xF1E8, 1,  0x01, 0x01, 0, 0, 0, RAD_OPT_CB_FREQ,         2, ""},
    {"navigation",           "Navigation",                      "Navigation source: internal or external.",                          0xF1E8, 1,  0x08, 0x08, 0, 0, 0, RAD_OPT_NAV_CLASSIC,     2, ""},
    {"intercom",             "Intercom",                        "Rider / passenger intercom fitted.",                                 0xF1E8, 1,  0x04, 0x04, 0, 0, 0, RAD_OPT_INTERCOM_YN,     2, ""},
    {"bluetooth",            "Bluetooth",                       "Bluetooth module fitted.",                                           0x0109, 0,  0x01, 0x01, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"rear_speaker_switch",  "Rear Speaker Switch Installed",   "Whether a rear speaker on/off switch is fitted.",                   0x010A, 0,  0x01, 0x01, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"wireless_headset",     "Wireless Headset",                "WHIM fitted (database note: requires radio software 1.21.2.1 or newer).", 0xF1E8, 1, 0x40, 0x40, 0, 0, 0, RAD_OPT_WHIM_YN,    2, ""},
    {"tpms",                 "TPMS Enable / Disable",           "Tire pressure monitoring display on the radio.",                    0xF1E8, 1,  0x80, 0x80, 0, 0, 0, RAD_OPT_TPMS,            2, ""},
    {"sms",                  "SMS Text Messaging",              "SMS text messaging feature.",                                        0x010C, 0,  0x01, 0x01, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"amp1_installed",       "AMP 1 Installed",                 "Amplifier 1 fitted.",                                                0x0100, 0,  0x01, 0x01, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"amp2_installed",       "AMP 2 Installed",                 "Amplifier 2 fitted.",                                                0x0101, 0,  0x01, 0x01, 0, 0, 0, RAD_OPT_YES_NO,          2, ""},
    {"xm_sdars",             "XM / SDARS",                      "Satellite radio tuner enable.",                                      0x010B, 0,  0x02, 0x02, 0, 0, 0, RAD_OPT_XM,              2, ""},
    {"hd_radio",             "HD Radio (LCP)",                  "HD Radio tuner enable.",                                             0x010B, 0,  0x04, 0x04, 0, 0, 0, RAD_OPT_HDRADIO,         2, ""},
    {"dab",                  "DAB (LCP)",                       "DAB tuner enable.",                                                  0x010B, 0,  0x08, 0x08, 0, 0, 0, RAD_OPT_DAB_CLASSIC,     2, ""},
};

static const CfgProfile RAD_SPC_PROFILES[] = {
    {"Boom!Box (classic)", RAD_SPC_FNS_CLASSIC, (uint8_t)(sizeof(RAD_SPC_FNS_CLASSIC) / sizeof(RAD_SPC_FNS_CLASSIC[0]))},
    {"Boom!Box GTS",       RAD_SPC_FNS_GTS,     (uint8_t)(sizeof(RAD_SPC_FNS_GTS) / sizeof(RAD_SPC_FNS_GTS[0]))},
};
#define RAD_SPC_PROFILE_COUNT 2

// Parts Number (DID 0xF1EA) prefix -> profile, for the radio's CAN variants
static const CfgRule RAD_SPC_RULES[] = {
    {"76000075C", 0},
    {"76000779", 1}, {"76000780", 1}, {"76000781", 1}, {"76000783", 1}, {"76000784", 1},
    {"76000785", 1}, {"76000786", 1}, {"76000787", 1}, {"76000796", 1}, {"76000797", 1},
};
#define RAD_SPC_RULE_COUNT (sizeof(RAD_SPC_RULES) / sizeof(RAD_SPC_RULES[0]))
#define RAD_SPC_DEFAULT_PROFILE 0   // the database's DEFAULT entry maps to the classic set
