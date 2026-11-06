// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// AUTO-GENERATED special-functions table - BCM 0x7E2.
// Only the "configuration option" functions are included. PIN read/change, key FOB assignment, auto-arming and
// TPMS sensor functions are deliberately not implemented (anti-theft / multi-step routines).
#pragma once
#include <stdint.h>
struct BcmSpcOpt { const char* text; uint8_t val; };
struct BcmSpcFn { const char* key; const char* name; const char* desc; uint16_t did; uint8_t mask; uint8_t rmask; uint8_t nopt; BcmSpcOpt opts[5]; const char* warn; };
static const BcmSpcFn BCM_SPC_FNS[] = {
    // Reordered per operator request: Auto-arm, Fork lock switch, Keyless ignition come first so in the BCM
    // special-functions view they render immediately below the Key FOB assignment (2 FOBs) card.
    {"auto_arm","Auto-arm (automatic anti-theft)","Automatically arms the anti-theft when the bike is turned off. Read it first and check the current state against your diagnostic tool; the exact bit in DID 0138 may differ by model.",0x0138,0x02,0x02,2,{{"Disable",0x00},{"Enable",0x02}},""},
    {"fork_lock_switch","Fork lock switch","Fork Lock Switch Option",0x0188,0x01,0x01,2,{{"Not Present",0x00},{"Present",0x01}},""},
    {"keyless_ignition","Keyless ignition","Keyless Ignition option allow to start bike without ignition switch.",0x0138,0x01,0x01,2,{{"Enable",0x00},{"Disable",0x01}},""},
    {"service_mode","Service mode","Enable/Disable Service Mode",0x0137,0x01,0x01,2,{{"Disable",0x00},{"Enable",0x01}},""},
    {"sidecar","Sidecar","Enable/Disable Sidecar.",0x0131,0x01,0x01,2,{{"Not Installed",0x00},{"Installed",0x01}},""},
    {"abs_installed","ABS installed","Select Installed if ABS is installed other way selet not Installed.",0x013B,0x02,0x02,2,{{"Not Installed",0x00},{"Installed",0x02}},""},
    {"radio_installed","Radio installed","Select Installed if Radio is installed other way selet not Installed.",0x0181,0x01,0x01,2,{{"Not Installed",0x00},{"Installed",0x01}},""},
    {"turn_signal_stop_lamp","Turn signal / stop lamp","Lighting Turn Signal /stop Lamp",0x0135,0x01,0x01,2,{{"Dedicated brake lamp",0x00},{"Turn signal lamps",0x01}},""},
    {"run_lighting_accessory","Run lighting in accessory","Run Lighting in Accessory",0x0133,0xFF,0xFF,4,{{"OFF(non CVO)",0x00},{"ON(non CVO)",0x01},{"OFF(CVO)",0x02},{"ON(CVO)",0x03}},""},
    {"headlamp","Headlamp","Headlamp",0x0182,0x01,0x01,2,{{"Standard",0x00},{"Dual Halogen",0x01}},""},
    {"fog_light_operation","Auxiliary / fog light operation","Auxiliary/Fog Light Operation",0x0180,0x37,0x07,5,{{"Legacy",0x10},{"PA Dash Switch",0x11},{"Factory Dash Switch",0x12},{"P/A Hand Control Switch",0x13},{"Factory Hand Control Switch",0x14}},""},
    {"fog_light_control","Auxiliary / fog light control","Auxiliary/Fog Light Control",0x0180,0xF0,0x30,3,{{"always on",0x00},{"Off with Lo Beams",0x10},{"Off with Hi Beams",0x20}},""},
    {"power_lock_switch","Power lock switch (saddle bags / trunk)","Power lock switch for Saddle bags and trunk Enabled  Disabled the Power Lock Switch",0x0185,0x01,0x01,2,{{"Disable",0x00},{"Enable",0x01}},""},
    {"front_turn_signal","Front turn signal","Front Turn Signal",0x0139,0x10,0x10,2,{{"Incandescent",0x00},{"LED",0x10}},""},
    {"rear_turn_signal","Rear turn signal","Rear Turn Signal",0x0139,0x20,0x20,2,{{"Incandescent",0x00},{"LED",0x20}},""},
    {"ebas","EBAS (trike models)","All Models that are not equipped with a EBAS FLHTCUTG Trike model equipped with a EBAS FLRT Trike model equipped with a EBAS",0x0131,0x13,0x13,4,{{"no EBAS",0x00},{"FLHTCUTG",0x01},{"FLRT",0x03},{"Trike 2019+",0x13}},""},
    {"stealth_mode","Stealth mode (police only)","stealth Mode (Police Only)",0x013D,0x02,0x02,2,{{"Disable",0x00},{"Enable",0x02}},""},
    {"rider_assistance","Rider assistance (ABS / TC)","Rider assistance ABS (Antilock Braking System) TC (Traction Control)",0x013B,0x66,0x66,3,{{"None",0x00},{"ABS Enabled",0x02},{"ABS & TC Enabled",0x66}},""},
    {"telematic_control_unit","Telematic control unit","Telematic control unit",0x0181,0x04,0x04,2,{{"Not Installed",0x00},{"Installed",0x04}},""},
    {"remote_immobilization","Remote immobilization","Remote Immobilization",0x013A,0x01,0x01,2,{{"Not Installed",0x00},{"Installed",0x01}},"DO NOT SELECT INSTALLED if motorcycles is not equipped with Remote Imobilization or it will disable the motorcycle.This feature is only available for select countries.All configurations must adhere to country and local regulations."},
};
#define BCM_SPC_FN_COUNT 20
