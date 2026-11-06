// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// AUTO-GENERATED output-test table - BCM (0x7E2) output tests.
#pragma once
#include <stdint.h>
enum BcmOutMode : uint8_t { BCMO_PULSE = 1, BCMO_ONOFF = 2 };
struct BcmOutKind { const char* key; const char* name; uint8_t mode; bool engine_off; uint8_t delay_s; const char* start; const char* on; const char* off; const char* ret; const char* end1; const char* end2; const char* warn; };
static const BcmOutKind BCM_OUT_KINDS[] = {
    {"lamp_turn_signal_left","Lamp Turn Signal Left",2,false,0,"","2F32000808","2F32000008","","2F32000008","2F32000000",""},
    {"lamp_turn_signal_right","Lamp Turn Signal Right",2,false,0,"","2F32001010","2F32000010","","2F32000010","2F32000000",""},
    {"brake_light","Brake light",2,false,0,"","2F30070808","2F30070008","","2F30070008","2F30070000",""},
    {"fuel_pump","Fuel pump",2,true,0,"","2F30060404","2F30060400","","2F30060400","2F30060000","Don't do this activation when engine is still hot. In case some petrol leaks there is danger of fire!"},
    {"alarm","Alarm",1,true,2,"2F310207","","","2F310200","","","The Alarm will be activated now"},
    {"coolant_pump","Coolant Pump",2,true,0,"","2F30060808","2F30060008","","2F30060008","2F30060000",""},
};
#define BCM_OUT_KIND_COUNT 6
struct BcmOutProfile { const char* name; uint8_t count; uint8_t kinds[6]; };
static const BcmOutProfile BCM_OUT_PROFILES[] = {
    {"Lamps + Fuel pump & Alarm",5,{0,1,2,3,4}},
    {"Lamps + Fuel pump",4,{0,1,2,3}},
    {"Lamps + Coolant Pump",4,{0,1,2,5}},
};
#define BCM_OUT_PROFILE_COUNT 3
struct BcmOutRule { const char* prefix; uint8_t profile; };
static const BcmOutRule BCM_OUT_RULES[] = {
    {"69991-11",1},
    {"69991-11B",1},
    {"69991-11C",1},
    {"69992-12",1},
    {"69992-12A",1},
    {"69992-12B",1},
    {"69993-11",0},
    {"69993-11B",0},
    {"69993-11C",0},
    {"69994-12",0},
    {"69994-12A",0},
    {"69994-12B",0},
    {"69994-12C",0},
    {"69996-12",0},
    {"69996-12A",0},
    {"69996-12B",0},
    {"69997-11B",0},
    {"69997-11C",0},
    {"41000168",0},
    {"41000169",0},
    {"41000341",0},
    {"41000342",0},
    {"41000349",0},
    {"41000350",0},
    {"41000341B",0},
    {"41000342B",0},
    {"41000348",1},
    {"41000348B",1},
    {"41000349B",0},
    {"41000350B",0},
    {"41000351",1},
    {"41000351B",1},
    {"41000341E",0},
    {"41000342E",0},
    {"41000349E",0},
    {"41000350E",0},
    {"41000351E",1},
    {"41000663",2},
    {"41000662",2},
    {"41000351F",1},
    {"41000350F",1},
    {"41000349F",1},
    {"41000348F",1},
    {"41000342F",1},
    {"41000341F",1},
};
#define BCM_OUT_RULE_COUNT 45
#define BCM_OUT_DEFAULT_PROFILE -1   // parts numbers without an ACT list in the database have no output tests
