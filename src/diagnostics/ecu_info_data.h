// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
// AUTO-GENERATED INFO item table - Read ECU Information.
#pragma once
#include <stdint.h>
enum InfoKind : uint8_t { IK_ASCII = 0, IK_HEX = 1, IK_DATE = 2, IK_NUM = 3, IK_MASK = 4, IK_ENUM = 5 };
struct InfoEnumEntry { uint8_t value; const char* text; };
struct InfoEnum { const InfoEnumEntry* entries; uint8_t count; const char* defaultText; };
// bytePosition/byteLength: first byte (0-based, after the 62 DID header) and byte count (ASCII: byteLength=0 = up to the end)
struct InfoItem { const char* label; uint8_t kind; uint16_t did; uint8_t bytePosition; uint8_t byteLength; uint8_t mask; uint8_t enumId; float scale; float offset; uint8_t decimalPlaces; const char* on; const char* off; };
struct InfoSetDef { const char* name; const InfoItem* items; uint8_t count; };
struct InfoRule { const char* prefix; uint8_t setIndex; };
struct InfoModuleDef { uint16_t id; const char* name; const InfoSetDef* sets; uint8_t setCount; const InfoRule* rules; uint8_t ruleCount; uint8_t defaultSetIndex; };
static const InfoEnumEntry INFO_ENUM_0[] = {{0,"OEM"},{1,"Race"},{2,"Street"}};
static const InfoEnumEntry INFO_ENUM_1[] = {{0,"Not Configured"},{1,"North America"},{2,"Japan"},{3,"Australia"},{4,"HDI/England/India"},{5,"South America"},{6,"Russia"},{7,"New Zeeland"},{8,"Mexico"},{9,"Central America"}};
static const InfoEnumEntry INFO_ENUM_2[] = {{0,"4 Speakers - Upper Fairing / Rear Pods / No AMP"},{1,"2 Speaker original equipment system with Upper Fairing speakers installed and no Amplifier module."},{8,"4 Speakers - Upper Fairing / Rear Pods / No AMP ( Tri Glide )"},{9,"2 Speakers - Upper Fairing / No AMP  ( Tri Glide )"},{14,"4 Speakers - Upper Fairing / Rear Pods / 1 AMP"},{16,"6 Speakers - Upper Fairing / Mid-Combo / 2 AMPS"},{128,"2 Speakers - Upper Fairing / 1 AMP"},{129,"4 Speakers - Upper Fairing / Rear Pods / 1 AMP"},{130,"4 Speakers - Upper  / Lower Fairing  / 1 AMP"},{131,"4 Speakers - Upper Fairing / Saddle Bag / 1 AMP"},{132,"6 Speakers - Upper  / Lower Fairing / Rear Pods / 2 AMPS"},{133,"6 Speakers - Upper Fairing / Saddle Bags / Rear Pods / 2 AMPS"},{134,"6 Speakers - Upper / Lower Fairing / Saddle Bags / 2 AMPS"},{135,"8 Speakers - Upper / Lower Fairing / Saddle Bags / Rear Pods / 2 AMPS"}};
static const InfoEnumEntry INFO_ENUM_3[] = {{0,"No"},{1,"Yes"},{2,"Interrupt tone detect only"},{3,"Interrupt tone detect and add device to mode list"}};
static const InfoEnumEntry INFO_ENUM_4[] = {{0,"4 Speakers FLHT original equipment system, Upper Fairing / Rear Pod"},{1,"2 Speakers FLHX original equipment system, Upper Fairing"},{2,"4 Speakers, FLTR original equipment system, Upper Fairing and Rear Pod"},{3,"2 Speakers, FLTR original equipment system, Upper Fairing"},{14,"4 Speakers, 1 Amp, Upper Fairing / Rear Pod"},{15,"4 Speakers, 1 Amp, Upper Fairing / Rear Pod"},{16,"4 Speakers, 2 Amps, Upper Fairing / Saddle Bag"},{17,"CVO ONLY -- 6 Speakers, 3 Amp, Upper Fairing / Lower Fairing / Saddle Bag"},{128,"2 Speakers, 1 Amp, Upper Fairing"},{130,"4 Speakers, 1 Amp, Upper Fairing / Lower Fairing"},{131,"4 Speakers, 1 Amp, Upper Fairing / Saddle Bag"},{132,"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod"},{133,"6 Speakers, 2 Amps, Upper Fairing / Saddle Bag / Rear Pod"},{134,"6 Speakers, 2 Amps, Upper / Lower Fairing / Saddle Bag"},{135,"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod"},{144,"Stage II, 2 Speakers, 1 Amp, Upper Fairing"},{145,"4 Speakers, 2 Amps, Upper Fairing / Lower Fairing"},{146,"4 Speakers, 2 Amps, Upper Fairing / Rear Pod"},{147,"6 Speakers, 3 Amps,  Upper Fairing / Lower Fairing / Rear Pod"},{148,"6 Speakers, 3 Amps,  Upper Fairing / Saddle Bag / Rear Pod"},{149,"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Saddle Bag"},{150,"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod"},{176,"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod, For use with Twin Cooled fairings"},{177,"8 Speakers, 2 Amps , Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings"},{178,"6 Speakers, 3 Amps,  Upper / Lower Fairing / Saddle Bag, For use with Twin Cooled fairings"},{179,"6 Speakers, 3 Amps, Upper Fairing /  Lower Fairing / Rear Pod, For use with Twin Cooled fairings"},{180,"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle, Bag / Rear Pod, For use with Twin Cooled fairings"},{181,"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod, For use with Twin Cooled fairings"},{182,"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings"},{183,"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Rear Pod, For use with Twin Cooled fairings"},{184,"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod, For use with Twin Cooled fairings"},{192,"2 Speakers, 1 Amp, Upper Fairing"},{194,"4 Speakers, 1  Amp, Upper Fairing/Lower Fairing"},{195,"4 Speakers, 1 Amp, Upper Fairing / Saddle Bag"},{196,"6 Speakers, 2 Amps, Upper / Lower Fairing / Rear Pod"},{197,"6 Speakers, 2 Amps, Upper Fairing / Saddle Bag / Rear Pod"},{198,"6 Speakers, 2 Amps, Upper / Lower Fairing / Saddle Bag"},{199,"8 Speakers, 2 Amps, Upper Fairing / Lower Fairing / Saddle Bag / Rear Pod"},{208,"Stage II, 2 Speakers, 1 Amp,  Upper Fairing"},{209,"4 Speakers, 2 Amps,  Upper Fairing / Lower Fairing"},{210,"4 Speakers, 2 Amps,  Upper Fairing / Rear Pod"},{211,"4 Speakers, 2 Amps, Upper Fairing / Saddle Bag"},{212,"6 Speakers, 3 Amps,  Upper Fairing / Lower Fairing / Rear Pod"},{213,"6 Speakers, 3 Amps,  Upper Fairing / Saddle Bag / Rear Pod"},{214,"6 Speakers, 3 Amps, Upper Fairing / Lower Fairing / Saddle Bag"},{215,"8 Speakers, 4 Amps, Upper Fairing / Lower Fairing / Saddle Bags / Rear Pod"},{224,"6 Speakers, 2 Amps, Upper Fairing / Rear Pod / Body Mount"},{225,"6 Speakers, 3 Amps, Upper Fairing / Rear Pod / Body Mount"}};
static const InfoEnumEntry INFO_ENUM_5[] = {{0,"Disabled"},{1,"Enabled"}};
static const InfoEnumEntry INFO_ENUM_6[] = {{0,"Internal CB"},{1,"External CB or other input device"}};
static const InfoEnumEntry INFO_ENUM_7[] = {{0,"No"},{1,"YES"}};
static const InfoEnumEntry INFO_ENUM_8[] = {{0,"Disabled"},{1,"Wired"},{4,"Wireless"}};
static const InfoEnumEntry INFO_ENUM_9[] = {{0,"Disabled"},{16,"Wired"},{64,"Wireless"}};
static const InfoEnum INFO_ENUMS[] = {{INFO_ENUM_0,3,"---"},{INFO_ENUM_1,10,"Not Configured"},{INFO_ENUM_2,14,"Not Configured"},{INFO_ENUM_3,4,"No"},{INFO_ENUM_4,48,"2 Speakers FLHX original equipment system, Upper Fairing"},{INFO_ENUM_5,2,"Disabled"},{INFO_ENUM_6,2,"Internal CB"},{INFO_ENUM_7,2,"No"},{INFO_ENUM_8,3,"Disabled"},{INFO_ENUM_9,3,"Disabled"}};
static const InfoItem INFO_M7E0_0[] = {
    {"Parts Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration ID",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Vehicle identification number (VIN)",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Software version",3,0xF1F5,0,2,0x00,0,1.0f,0.0f,0,"",""},
    {"EFI Tuner",5,0xF1E7,0,1,0x00,0,1.0f,0.0f,0,"",""},
    {"Vehicle speed sensor configuration",3,0x0100,0,2,0x00,0,1.0f,0.0f,0,"",""},
    {"Idle speed - required(1/min)",3,0x0101,0,1,0x00,0,8.0f,1000.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7E0_1[] = {
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF193,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"EFI / Ignition Race Tuned",4,0xF1E7,0,1,0x01,0,1.0f,0.0f,0,"Race Tuned","Not Tuned"},
    {"Module Configuration",1,0xF1E8,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Software Configuration",1,0x1001,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"EITMS",4,0xF1E8,2,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Installation Date",2,0xF19D,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF199,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Calibrated",2,0xF19B,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Boot Software Version",1,0xF180,1,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Version",1,0xF181,1,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Version",1,0xF182,1,4,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7E0_2[] = {
    {"EFI / Ignition Race Tuned",4,0xF1E7,0,1,0x01,0,1.0f,0.0f,0,"Race Tuned","Not Tuned"},
    {"Module Configuration",1,0xF1E8,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"EITMS",4,0xF1E8,2,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF193,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Software Configuration",1,0x1001,0,8,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7E0_3[] = {
    {"EFI / Ignition Race Tuned",4,0xF1E7,0,1,0x01,0,1.0f,0.0f,0,"Race Tuned","Not Tuned"},
    {"Module Configuration",1,0xF1E8,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"EITMS",4,0xF1E8,2,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF193,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Boot Software Version",1,0xF180,1,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Version",1,0xF181,1,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Version",1,0xF182,1,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Software Configuration",1,0x1001,0,8,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7E0_SETS[] = {{"8F2A64",INFO_M7E0_0,9},{"8FBB6C",INFO_M7E0_1,15},{"A41B84",INFO_M7E0_2,9},{"A4335C",INFO_M7E0_3,12}};
static const InfoRule INFO_M7E0_RULES[] = {{"31903-11",0},{"31904-11",0},{"34247-11",0},{"34249-11",0},{"41000006A",0},{"41000006",0},{"41000057",0},{"41000061",0},{"41000161",0},{"41000307",0},{"41000326",0},{"41000434",0},{"41000326A",0},{"41000509",0},{"41000677",1},{"31700014",1},{"41000669",2},{"41000706",1},{"41000713",1},{"41000714",1},{"41700014",1},{"60700014",1},{"77600014",1},{"96600014",1},{"41999999",2},{"41001151",2},{"41001274",3},{"41001347",3},{"41001348",2},{"41001349",2},{"41001352",2},{"41001355",3},{"41001357",3}};
static const InfoItem INFO_M7E1_0[] = {
    {"VIN",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7E1_SETS[] = {{"8FC774",INFO_M7E1_0,7}};
static const InfoRule INFO_M7E1_RULES[] = {{"40662-11",0},{"48342-12",0},{"41100036",0},{"41100012",0},{"41100060",0},{"41100060A",0},{"41100062",0},{"41100089",0},{"41100106",0},{"41100097",0}};
static const InfoItem INFO_M7E2_0[] = {
    {"Parts Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Accelerate Axis",4,0x0200,27,1,0x10,0,1.0f,0.0f,0,"Z axis","Y axis"},
    {"Service Mode",4,0x0137,0,1,0x01,0,1.0f,0.0f,0,"yes","no"},
    {"Sidecar/TriGlide",4,0x0131,0,1,0x01,0,1.0f,0.0f,0,"yes","no"},
};
static const InfoItem INFO_M7E2_1[] = {
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Supplier Assembly P/N",0,0xF1F9,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Software Configuration",1,0x1001,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Turn Signal / Stop Lamp",4,0x1001,2,1,0x10,0,1.0f,0.0f,0,"Turn signal lamps","Dedicated brake lamp"},
    {"Fork Lock Switch Option",4,0x1001,0,1,0x80,0,1.0f,0.0f,0,"Present","Not Present"},
    {"Remote Immobilization (Brazil)",4,0x1001,4,1,0x20,0,1.0f,0.0f,0,"Installed","Not Installed"},
    {"Rider Assistance",4,0x1001,5,1,0x01,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Run Lighting in Accessory",4,0x1001,2,1,0x04,0,1.0f,0.0f,0,"ON (non CVO)","OFF (non CVO)"},
    {"Perimeter Lighting",4,0x1001,2,1,0x08,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"TPMS Status",4,0x1001,0,1,0x20,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"TCU",4,0x1001,6,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Hardware Part Number",0,0xF191,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Radio",4,0x1001,6,1,0x01,0,1.0f,0.0f,0,"Installed","Not Installed"},
};
static const InfoSetDef INFO_M7E2_SETS[] = {{"8F3084",INFO_M7E2_0,5},{"90D6B4",INFO_M7E2_1,13}};
static const InfoRule INFO_M7E2_RULES[] = {{"69991-11",0},{"69991-11B",0},{"69991-11C",0},{"69992-12",0},{"69992-12A",0},{"69992-12B",0},{"69993-11",0},{"69993-11B",0},{"69993-11C",0},{"69994-12",0},{"69994-12A",0},{"69994-12B",0},{"69994-12C",0},{"69996-12",0},{"69996-12A",0},{"69996-12B",0},{"69997-11B",0},{"69997-11C",0},{"41000168",0},{"41000169",0},{"41000341",0},{"41000342",0},{"41000349",0},{"41000350",0},{"41000341B",0},{"41000342B",0},{"41000348",0},{"41000348B",0},{"41000349B",0},{"41000350B",0},{"41000351",0},{"41000351B",0},{"41000341E",0},{"41000342E",0},{"41000349E",0},{"41000350E",0},{"41000351E",0},{"41000663",0},{"41000662",0},{"41000351F",0},{"41000350F",0},{"41000349F",0},{"41000348F",0},{"41000342F",0},{"41000341F",0},{"41000674",1},{"41000675",1},{"41000678",1},{"41000679",1},{"41001169",1},{"41001170",1},{"41001171",1},{"41001172",1},{"41001169A",1},{"41001170A",1},{"41001171A",1},{"41001172A",1}};
static const InfoItem INFO_M7D0_0[] = {
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"TSSM Security State",4,0x0200,32,1,0x01,0,1.0f,0.0f,0,"Disarmed","Armed"},
    {"Service Mode",4,0x1007,0,1,0x01,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Transport Mode",4,0x0200,3,1,0x02,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"FOB Low Battery Warning Feature",4,0x0200,3,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
};
static const InfoSetDef INFO_M7D0_SETS[] = {{"91379C",INFO_M7D0_0,6}};
static const InfoRule INFO_M7D0_RULES[] = {{"",0}};
static const InfoItem INFO_M7F2_0[] = {
    {"Parts Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration ID",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Vehicle identification number (VIN)",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7F2_1[] = {
    {"Hardware Part Number",0,0xF193,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Serial number",0,0xF18C,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Odometer Accumulated(km)",3,0x0206,0,4,0x00,0,0.001f,0.0f,1,"",""},
    {"Odometer(km)",3,0x0206,4,4,0x00,0,0.001f,0.0f,1,"",""},
    {"Software Configuration",1,0x1001,0,8,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7F2_2[] = {
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Serial number",0,0xF18C,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF193,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7F2_SETS[] = {{"8F32B4",INFO_M7F2_0,5},{"A61D94",INFO_M7F2_1,6},{"A629D4",INFO_M7F2_2,4}};
static const InfoRule INFO_M7F2_RULES[] = {{"67263-11",0},{"67264-11",0},{"70701-11",0},{"70702-11",0},{"70708-11",0},{"74774-11",0},{"74775-11",0},{"74776-11",0},{"74777-11",0},{"70900100",0},{"70900100A",0},{"70900121",0},{"70900124",0},{"70900275",0},{"70900275A",0},{"70900293",0},{"70900294",0},{"70900295",0},{"70900369",0},{"70900370",0},{"70900371",0},{"70900523",0},{"70900524",0},{"70900644",0},{"70900645",0},{"70900646",0},{"70900801",0},{"70900118D",0},{"70900119C",0},{"70900120C",0},{"70900121D",0},{"70900122C",0},{"70900123C",0},{"70900124D",0},{"70900125C",0},{"70900126C",0},{"70900260C",0},{"70900261B",0},{"70900379C",0},{"70900421C",0},{"70900616B",0},{"70900773B",0},{"70900777B",0},{"70900778B",0},{"70900896",0},{"70900897",0},{"70900987",0},{"70900988",0},{"70900989",0},{"70901100",0},{"70901105",1},{"70901201",1},{"70901202",0},{"70901203",0},{"70901204",0},{"70900971",2},{"70900972",2},{"70900973",2},{"70901208",2},{"70901209",2},{"70901210",2},{"70901218",2},{"70901219",2},{"70901220",2},{"SERENGETI",0},{"STREET",0}};
static const InfoItem INFO_M7F1_0[] = {
    {"VIN",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Module Configuration",1,0xF1E8,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"MEC",3,0xF1F0,0,1,0x00,0,1.0f,0.0f,0,"",""},
    {"Supplier Software P/N",0,0xF1F1,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Input 1 Name",0,0x0106,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Input 2 Name",0,0x0107,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Input 3 Name",0,0x0108,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Radio Region Code",5,0x0102,0,1,0x00,1,1.0f,0.0f,0,"",""},
    {"Speaker Configuration",5,0xF1E8,0,1,0x00,2,1.0f,0.0f,0,"",""},
    {"CB",4,0xF1E8,1,1,0x02,0,1.0f,0.0f,0,"Yes","No"},
    {"CB Frequency",4,0xF1E8,1,1,0x01,0,1.0f,0.0f,0,"FM","AM"},
    {"Navigation",4,0xF1E8,1,1,0x08,0,1.0f,0.0f,0,"Internal","External"},
    {"Bluetooth",4,0x0109,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Intercom",4,0xF1E8,1,1,0x04,0,1.0f,0.0f,0,"Yes","No"},
    {"SMS Text Messaging",4,0x010C,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"AMP 1 Installed",4,0x0100,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"AMP 2 Installed",4,0x0101,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Audio Input 1",4,0x0103,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Audio Input 2",4,0x0104,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Audio Input 3",5,0x0105,0,1,0x00,3,1.0f,0.0f,0,"",""},
    {"Rear Speaker Switch Installed",4,0x010A,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"CB Option",4,0x010B,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"XM/SDARS",4,0x010B,0,1,0x02,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"HD Radio  (LCP)",4,0x010B,0,1,0x04,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"DAB (LCP)",4,0x010B,0,1,0x08,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Amp1 (LCP)",4,0x010B,0,1,0x10,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Amp2 (LCP)",4,0x010B,0,1,0x20,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"TPMS Installed",4,0xF1E8,1,1,0x80,0,1.0f,0.0f,0,"Yes","No"},
    {"Wireless Headset (Req. min. Radio sw v1.21.2.1)",4,0xF1E8,1,1,0x40,0,1.0f,0.0f,0,"Yes","No"},
};
static const InfoItem INFO_M7F1_1[] = {
    {"VIN",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Module Configuration",1,0xF1E8,0,8,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"MEC",3,0xF1F0,0,1,0x00,0,1.0f,0.0f,0,"",""},
    {"Supplier Software P/N",0,0xF1F1,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Speaker Configuration",5,0xF1E8,0,1,0x00,4,1.0f,0.0f,0,"",""},
    {"Radio Region Code",5,0x0102,0,1,0x00,1,1.0f,0.0f,0,"",""},
    {"Digital Audio Broadcast",5,0xF1E8,3,1,0x00,5,1.0f,0.0f,0,"",""},
    {"Rear Speaker Switch Installed",4,0x010A,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Navigation",4,0xF1E8,1,1,0x08,0,1.0f,0.0f,0,"Internal","External"},
    {"TPMS Installed",4,0xF1E8,1,1,0x80,0,1.0f,0.0f,0,"Yes","No"},
    {"AMP 1 Installed",4,0x0100,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"AMP 2 Installed",4,0x0101,0,1,0x01,0,1.0f,0.0f,0,"Yes","No"},
    {"Audio Input 1",5,0x0103,0,1,0x00,6,1.0f,0.0f,0,"",""},
    {"Audio Input 2",5,0x0104,0,1,0x00,7,1.0f,0.0f,0,"",""},
    {"Audio Input 3",5,0x0105,0,1,0x00,3,1.0f,0.0f,0,"",""},
    {"CB",4,0xF1E8,1,1,0x02,0,1.0f,0.0f,0,"Yes","No"},
    {"CB Frequency",4,0xF1E8,1,1,0x01,0,1.0f,0.0f,0,"FM","AM"},
    {"Intercom",4,0xF1E8,1,1,0x04,0,1.0f,0.0f,0,"Yes","No"},
    {"Wireless Headset Interface Module",4,0xF1E8,1,1,0x40,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Bluetooth",4,0x0109,0,1,0x01,0,1.0f,0.0f,0,"Enabled","Disabled"},
    {"Apple CarPlay Mic Type",5,0xF1E8,4,1,0x00,8,1.0f,0.0f,0,"",""},
    {"Apple Siri",5,0xF1E8,4,1,0x00,9,1.0f,0.0f,0,"",""},
    {"SMS Text Messaging",4,0x010C,0,1,0x01,0,1.0f,0.0f,0,"Enabled","Disabled"},
};
static const InfoSetDef INFO_M7F1_SETS[] = {{"91A79C",INFO_M7F1_0,33},{"91AF0C",INFO_M7F1_1,27}};
static const InfoRule INFO_M7F1_RULES[] = {{"76000075C",0},{"76000779",1},{"76000780",1},{"76000781",1},{"76000783",1},{"76000784",1},{"76000785",1},{"76000786",1},{"76000787",1},{"76000796",1},{"76000797",1}};
static const InfoItem INFO_M7E3_0[] = {
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Assembly Part Number",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7E3_SETS[] = {{"91B60C",INFO_M7E3_0,4}};
static const InfoRule INFO_M7E3_RULES[] = {{"",0}};
static const InfoItem INFO_M7E5_0[] = {
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Assembly Part Number",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7E5_SETS[] = {{"91B874",INFO_M7E5_0,4}};
static const InfoRule INFO_M7E5_RULES[] = {{"",0}};
static const InfoItem INFO_M7E7_0[] = {
    {"VIN",0,0xF190,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoItem INFO_M7E7_1[] = {
    {"VIN",0,0xF1EF,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Hardware Part Number",0,0xF1EA,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Calibration Part Number",0,0xF1ED,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Date ECU Reprogrammed",2,0xF1E9,0,4,0x00,0,1.0f,0.0f,0,"",""},
    {"Homologation ID",0,0xF1EE,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Application Software PN",0,0xF1EB,0,0,0x00,0,1.0f,0.0f,0,"",""},
    {"Assembly Part Number",0,0xF1EC,0,0,0x00,0,1.0f,0.0f,0,"",""},
};
static const InfoSetDef INFO_M7E7_SETS[] = {{"93AD68",INFO_M7E7_0,7},{"A646B4",INFO_M7E7_1,7}};
static const InfoRule INFO_M7E7_RULES[] = {{"41100110",0},{"41100112",1},{"41100123",1},{"41100130",0}};
static const InfoModuleDef INFO_MODULES[] = {
    {0x7E0,"ECM",INFO_M7E0_SETS,4,INFO_M7E0_RULES,33,0},
    {0x7E1,"ABS",INFO_M7E1_SETS,1,INFO_M7E1_RULES,10,0},
    {0x7E2,"BCM",INFO_M7E2_SETS,2,INFO_M7E2_RULES,57,1},
    {0x7D0,"SCM",INFO_M7D0_SETS,1,INFO_M7D0_RULES,0,0},
    {0x7F2,"SPEEDO",INFO_M7F2_SETS,3,INFO_M7F2_RULES,66,0},
    {0x7F1,"RADIO",INFO_M7F1_SETS,2,INFO_M7F1_RULES,11,0},
    {0x7E3,"LHC",INFO_M7E3_SETS,1,INFO_M7E3_RULES,0,0},
    {0x7E5,"RHC",INFO_M7E5_SETS,1,INFO_M7E5_RULES,0,0},
    {0x7E7,"EHCU",INFO_M7E7_SETS,2,INFO_M7E7_RULES,4,1},
};
#define INFO_MODULE_COUNT 9
