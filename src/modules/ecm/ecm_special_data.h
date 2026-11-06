// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// AUTO-GENERATED special-functions table - ECM (0x7E0) special functions.
#pragma once
#include <stdint.h>
enum EcmSpcType : uint8_t { ECMS_IDLE = 1, ECMS_CFGBIT = 2, ECMS_RESET = 3, ECMS_PAIR = 4, ECMS_SPEEDO = 5, ECMS_UNSUPPORTED = 9 };
struct EcmSpcKind { const char* key; const char* name; const char* desc; const char* note; uint8_t type; uint8_t pos; uint8_t mask; uint16_t vmin; uint16_t vmax; uint8_t step; const char* cmd; uint16_t delay_ms; };
static const EcmSpcKind ECM_SPC_KINDS[] = {
    {"idle_speed","Idle speed","Adjust engine idle speed in the range 800 to 1200 rpm. The lowest recommended engine idle speed by producer are 950 rpm to keep the pressure of the oil.","",1,0,0x00,592,2000,8,"",0},
    {"exhaust_actuator","Exhaust Actuator","Enable/Disable Exhaust Actuator. The setting is possible only if everything is right mounted and functional.","",2,3,0x01,0,0,0,"",0},
    {"air_cleaner_intake_valve","Air Cleaner Intake Valve","Enable/Disable Air Cleaner Intake Valve. The setting is possible only if everything is right mounted and functional.","",2,3,0x02,0,0,0,"",0},
    {"automatic_compression_release_acr","Automatic compression release (ACR)","Enable/Disable automatic compression release (ACR) valves during starting. The setting is possible only if everything is right mounted and functional.","",2,8,0x03,0,0,0,"",0},
    {"side_stand_switch","Side stand switch","Enable/Disable Side stand switch The setting is possible only if everything is right mounted and functional.","",2,5,0x01,0,0,0,"",0},
    {"reset_afv_0301","Reset air-fuel values (AFV)","By this function will be reset Air Fuel Values.","",3,0,0x00,0,0,0,"2F05080301",10000},
    {"pairing","Security module pairing (password learn)","After replacement engine control module (EFI) or security module. (TSSM/HFSM/BCM) perform its pairing (Password learn procedure).","",4,0,0x00,0,0,0,"",600},
    {"speedo_calibration","Speedometer calibration","Type speed from Speedometer and real speed from GPS You need this function if you changed Primary ratio Secondary ratio GearBox Wheel size, etc..","",5,0,0x00,500,5000,0,"",0},
    {"exhaust_actuator_3_2","Exhaust Actuator","Enable/Disable Exhaust Actuator. The setting is possible only if everything is right mounted and functional.","",2,3,0x02,0,0,0,"",0},
    {"automatic_compression_release_acr_4_4","Automatic compression release (ACR)","Enable/Disable automatic compression release (ACR) valves during starting. The setting is possible only if everything is right mounted and functional.","",2,4,0x04,0,0,0,"",0},
    {"cruise_control_speedo","Cruise Control","Enable/Disable Cruise Control The setting is possible only if everything is right mounted and functional.","Also needs a write to the speedometer module (0x7F2) with its own security access - not supported yet",9,0,0x00,0,0,0,"",0},
    {"eitms_idle_temperature_management","EITMS (idle temperature management)","EITMS assists in lowering engine temperature at idle RPM. Fuel is restricted to rear cylinder at high temperature operating conditions.","",2,5,0x04,0,0,0,"",0},
    {"chassis_fan_kit","Chassis fan kit","Chassis fan kit","Touring models Year 2017-Later Set New value state to Enable after the Chassis Fan kit was mounted",2,4,0x08,0,0,0,"",0},
    {"oil_cooler_fan","Oil cooler fan","Oil Cooler Fan","Touring models Year 2017-Later Set New value to Enable when the vehicle is equipped with Oil Cooler Fan Set Disable when the oil cooler is passive and doesn't have a fan",2,4,0x20,0,0,0,"",0},
    {"cruise_control","Cruise Control","Enable/Disable Cruise Control The setting is possible only if everything is right mounted and functional.","",2,7,0x01,0,0,0,"",0},
    {"reset_afv_8000","Reset air-fuel values (AFV)","By this function will be reset Air Fuel Values.","",3,0,0x00,0,0,0,"2F0508038000",10000},
    {"chassis_fan_kit_speedo","Chassis fan kit","Chassis fan kit","Also needs a write to the speedometer module (0x7F2) with its own security access - not supported yet",9,0,0x00,0,0,0,"",0},
};
#define ECM_SPC_KIND_COUNT 17
struct EcmSpcProfile { const char* name; uint8_t count; uint8_t kinds[12]; };
static const EcmSpcProfile ECM_SPC_PROFILES[] = {
    {"942794", 8, {0,1,2,3,4,5,6,7}},
    {"942824", 8, {0,1,2,3,4,5,6,7}},
    {"9428B4", 9, {0,8,9,10,11,4,5,6,7}},
    {"942954", 9, {0,8,9,10,11,4,5,6,7}},
    {"9429F4", 11, {0,8,2,9,10,11,4,5,6,7,12}},
    {"942AB4", 8, {0,1,3,11,5,4,6,7}},
    {"942B44", 9, {0,8,2,9,11,4,5,6,7}},
    {"942BE4", 9, {0,8,9,10,11,4,5,6,7}},
    {"942C84", 8, {0,8,9,11,4,5,6,7}},
    {"942D14", 8, {0,8,9,11,4,5,6,7}},
    {"942DA4", 10, {8,9,10,11,13,4,5,6,12,7}},
    {"942E54", 11, {0,8,2,9,14,11,4,5,6,7,4}},
    {"942EA4", 11, {0,8,2,9,14,11,4,5,6,7,4}},
    {"942EF4", 10, {8,9,10,11,13,4,5,6,12,7}},
    {"942FA4", 8, {8,9,10,11,4,5,6,7}},
    {"94CBB4", 9, {9,10,11,4,15,6,7,13,16}},
};
#define ECM_SPC_PROFILE_COUNT 16
struct EcmSpcCalRule { const char* prefix; int8_t profile; };
static const EcmSpcCalRule ECM_SPC_CAL_RULES[] = {
    {"31903-11", 0},
    {"31904-11", 1},
    {"34247-11", 2},
    {"34249-11", 3},
    {"41000006A", 6},
    {"41000006", 5},
    {"41000057", 7},
    {"41000061", 8},
    {"41000161", 9},
    {"41000307", 10},
    {"41000326", 11},
    {"41000434", 14},
    {"41000326A", 12},
    {"41000509", 13},
    {"41000677", 15},
    {"31700014", 15},
    {"41000669", 15},
    {"41000706", 15},
    {"41000713", 15},
    {"41000714", 15},
    {"41700014", 15},
    {"60700014", 15},
    {"77600014", 15},
    {"96600014", 15},
    {"41999999", 15},
    {"41001151", 15},
    {"41001274", 15},
    {"41001347", 15},
    {"41001348", 15},
    {"41001349", 15},
    {"41001352", 15},
    {"41001355", 15},
    {"41001357", 15}
};
#define ECM_SPC_CAL_RULE_COUNT 33
#define ECM_SPC_DEFAULT_PROFILE 4
