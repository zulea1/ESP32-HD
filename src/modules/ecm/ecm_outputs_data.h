// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// AUTO-GENERATED output-test table - ECM (0x7E0) output tests.
#pragma once
#include <stdint.h>
enum EcmOutMode : uint8_t { ECMO_PULSE = 1, ECMO_ONOFF = 2, ECMO_ADJUST = 3 };
struct EcmOutKind {
    const char* key;      // stable id used by the web UI
    const char* name;
    uint8_t     mode;     // EcmOutMode
    bool        secure;   // database marks the test as security-access protected
    bool        engine_on;// test requires the engine running (adjust tests); others require engine stopped
    uint8_t     delay_s;  // PULSE: seconds the output stays active before control is returned
    const char* start;    // hex request that starts / arms the test
    const char* on;       // ONOFF: ON request; ADJUST: template with '##' replaced by the step byte
    const char* off;      // ONOFF: OFF request
    const char* ret;      // PULSE: return-control request sent after delay_s
    const char* end1;     // end sequence (excluding session reset): first request
    const char* end2;     //   second request (return control)
    uint8_t     steps;    // ADJUST: number of steps
    int16_t     vals[5];  // ADJUST: displayed values
    const char* cmds[5];  // ADJUST: step bytes (hex)
};
static const EcmOutKind ECM_OUT_KINDS[] = {
    {"idle_air_control_valve","Idle air control valve",1,true,false,2,"2F0504034000","","","2F050400","","",0,{0,0,0,0,0},{"","","","",""}},
    {"servomotor_exhaust_flap","Servomotor Exhaust flap",2,true,false,0,"2F0505038000","2F05050380FF","2F0505038000","","2F0505038000","2F050500",0,{0,0,0,0,0},{"","","","",""}},
    {"air_cleaner_intake_valve","Air Cleaner Intake Valve",2,true,false,0,"2F0506038000","2F05060380FF","2F0506038000","","2F0506038000","2F050600",0,{0,0,0,0,0},{"","","","",""}},
    {"cooling_pump_cooling_relay","Cooling pump/Cooling Relay",2,true,false,0,"2F0501034000","2F0501034040","2F0501034000","","2F0501034000","2F050100",0,{0,0,0,0,0},{"","","","",""}},
    {"chassis_fan","Chassis Fan",2,true,false,0,"2F0509030200","2F0509030202","2F0509030200","","2F0509030200","2F050900",0,{0,0,0,0,0},{"","","","",""}},
    {"fan_a","Fan A",2,true,false,0,"2F0501032000","2F0501032020","2F0501032000","","2F0501032000","2F050100",0,{0,0,0,0,0},{"","","","",""}},
    {"fan_b","Fan B",2,true,false,0,"2F0501038000","2F0501038080","2F0501038000","","2F0501038000","2F050100",0,{0,0,0,0,0},{"","","","",""}},
    {"throttle_actuator_sweep","Throttle Actuator Sweep",1,true,false,8,"2F0507038000","","","2F050700","","",0,{0,0,0,0,0},{"","","","",""}},
    {"purge_solenoid_duty_cycle","Purge Solenoid Duty Cycle",3,false,true,0,"2F0502038000","2F05020380##","2F05020380##","","2F0502038000","2F050200",5,{0,25,50,75,100},{"00","40","80","C0","FF"}},
    {"idle_speed_control","Idle speed control",3,false,true,0,"2F0504030264","2F05040302##","2F05040302##","","2F0504030264","2F050400",5,{800,1100,1400,1700,2000},{"64","8A","AF","D5","FA"}},
    {"neutral_rev_limit_override","Neutral rev limit override",2,true,false,0,"3101400D00","3101400D01","3101400D00","","3101400D00","",0,{0,0,0,0,0},{"","","","",""}},
};
#define ECM_OUT_KIND_COUNT 11
struct EcmOutProfile { const char* name; uint8_t count; uint8_t kinds[12]; };
// Profiles = distinct ACT sets of the database (index = profile id)
static const EcmOutProfile ECM_OUT_PROFILES[] = {
    {"645F94", 10, {0,1,2,3,4,5,6,7,8,9}},
    {"646018", 5, {0,1,2,8,9}},
    {"646060", 5, {0,1,8,9,3}},
    {"6460A8", 5, {1,2,7,8,9}},
    {"6460F0", 3, {7,8,9}},
    {"646120", 7, {1,3,5,6,7,8,9}},
    {"646180", 7, {3,4,5,6,7,8,9}},
    {"64B6A0", 7, {4,5,6,7,8,9,10}},
};
#define ECM_OUT_PROFILE_COUNT 8
struct EcmOutCalRule { const char* prefix; int8_t profile; };  // profile -1 = no output tests defined in the database
static const EcmOutCalRule ECM_OUT_CAL_RULES[] = {
    {"31903-11", 1},
    {"31904-11", 1},
    {"34247-11", 3},
    {"34249-11", 3},
    {"41000006A", 2},
    {"41000006", 2},
    {"41000057", 5},
    {"41000061", 0},
    {"41000161", 2},
    {"41000307", 6},
    {"41000326", 2},
    {"41000434", 4},
    {"41000326A", 2},
    {"41000509", 6},
    {"41000677", 7},
    {"31700014", 7},
    {"41000669", 7},
    {"41000706", 7},
    {"41000713", 7},
    {"41000714", 7},
    {"41700014", 7},
    {"60700014", 7},
    {"77600014", 7},
    {"96600014", 7},
    {"41999999", 7},
    {"41001151", -1},
    {"41001274", -1},
    {"41001347", -1},
    {"41001348", -1},
    {"41001349", -1},
    {"41001352", -1},
    {"41001355", -1},
    {"41001357", -1}
};
#define ECM_OUT_CAL_RULE_COUNT 33
#define ECM_OUT_DEFAULT_PROFILE 0
