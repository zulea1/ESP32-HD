// AUTO-GENERATED output-test table - ABS (0x7E1) output tests.
#pragma once
#include <stdint.h>
enum AbsOutMode : uint8_t { ABSO_PULSE = 1, ABSO_ONOFF = 2 };
struct AbsOutKind { const char* key; const char* name; uint8_t mode; bool engine_off; uint8_t delay_s; const char* start; const char* on; const char* off; const char* ret; const char* end1; const char* end2; const char* warn; };
static const AbsOutKind ABS_OUT_KINDS[] = {
    {"front_apply_solenoid","Front apply solenoid",2,true,0,"","2F311F01020000","2F311F01000000","","2F311F01000000","2F311F00000000",""},
    {"front_release_solenoid","Front release solenoid",2,true,0,"","2F311F02020000","2F311F02000000","","2F311F02000000","2F311F00000000",""},
    {"rear_apply_solenoid","Rear apply solenoid",2,true,0,"","2F311F10010000","2F311F10000000","","2F311F10000000","2F311F00000000",""},
    {"rear_release_solenoid","Rear release solenoid",2,true,0,"","2F311F20010000","2F311F20000000","","2F311F20000000","2F311F00000000",""},
    {"front_actuator","Front actuator",2,true,0,"","2F311F00020000","2F311F00000000","","2F311F00000000","2F311F00000000",""},
    {"rear_actuator","Rear actuator",2,true,0,"","2F311F00010000","2F311F00000000","","2F311F00000000","2F311F00000000",""},
    {"front_apply_solenoid_6464c4","Front apply solenoid",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_6464c4","Front release solenoid",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_6464c4","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_6464c4","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"pump_motor_voltage","Pump Motor Voltage",1,true,10,"2FFB06080832","","","2FFB06000832","","",""},
    {"front_apply_solenoid_646560","Front apply solenoid",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_646560","Front release solenoid",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_646560","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_646560","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"front_release_solenoid_left","Front Release solenoid Left",1,true,5,"2FFB07080814","","","2FFB07008014","","",""},
    {"front_apply_solenoid_left","Front apply solenoid Left",1,true,5,"2FFB07040414","","","2FFB07000414","","",""},
    {"front_apply_solenoid_right","Front apply solenoid Right",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_right","Front Release solenoid Right",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_6465c0","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_6465c0","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"front_apply_solenoid_646620","Front apply solenoid",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_646620","Front release solenoid",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_646620","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_646620","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"front_apply_solenoid_646668","Front apply solenoid",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_646668","Front release solenoid",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_646668","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_646668","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"rear_apply_solenoid_6466b0","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_6466b0","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"rear_apply_solenoid_right","Rear apply solenoid Right",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_apply_solenoid_left","Rear apply solenoid Left",1,true,5,"2FFB08101014","","","2FFB08001014","","",""},
    {"rear_release_solenoid_right","Rear Release solenoid Right",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
    {"rear_release_solenoid_left","Rear Release solenoid Left",1,true,5,"2FFB08202014","","","2FFB08002014","","",""},
    {"front_apply_solenoid_6467e8","Front apply solenoid",1,true,5,"2FFB06404014","","","2FFB06004014","","",""},
    {"front_release_solenoid_6467e8","Front release solenoid",1,true,5,"2FFB06808014","","","2FFB06008014","","",""},
    {"rear_apply_solenoid_6467e8","Rear apply solenoid",1,true,5,"2FFB06101014","","","2FFB06001014","","",""},
    {"rear_release_solenoid_6467e8","Rear release solenoid",1,true,5,"2FFB06202014","","","2FFB06002014","","",""},
};
#define ABS_OUT_KIND_COUNT 39
struct AbsOutProfile { const char* name; uint8_t count; uint8_t kinds[9]; };
static const AbsOutProfile ABS_OUT_PROFILES[] = {
    {"ABS solenoids (6) - toggle mode",6,{0,1,2,3,4,5}},
    {"ABS solenoids (5) - pump",5,{6,7,8,9,10}},
    {"ABS solenoids (6) - toggle mode",6,{0,1,2,3,4,5}},
    {"ABS solenoids (7) - pump, split L/R",7,{11,12,13,14,10,15,16}},
    {"ABS solenoids (7) - pump, split L/R",7,{17,16,18,15,19,20,10}},
    {"ABS solenoids (5) - pump",5,{21,22,23,24,10}},
    {"ABS solenoids (5) - pump",5,{25,26,27,28,10}},
    {"ABS solenoids (7) - pump, split L/R",7,{17,16,18,15,29,30,10}},
    {"ABS solenoids (9) - pump, split L/R",9,{17,16,18,15,31,32,33,34,10}},
    {"ABS solenoids (7) - pump, split L/R",7,{31,33,10,17,16,18,15}},
    {"ABS solenoids (5) - pump",5,{35,36,37,38,10}},
};
#define ABS_OUT_PROFILE_COUNT 11
struct AbsOutRule { const char* prefix; int8_t profile; };
static const AbsOutRule ABS_OUT_RULES[] = {
    {"40662-11", 0},
    {"48342-12", 1},
    {"41100036", 2},
    {"41100012", 4},
    {"41100060", 5},
    {"41100060A", 6},
    {"41100062", 7},
    {"41100089", 8},
    {"41100106", 9},
    {"41100097", 10}
};
#define ABS_OUT_RULE_COUNT 10
#define ABS_OUT_DEFAULT_PROFILE 3
