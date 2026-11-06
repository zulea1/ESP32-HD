/**
 * ============================================================================
 * File      : src/core/uds_handler.cpp
 * Project   : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright : (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License   : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version   : see FIRMWARE_VERSION_STR / FIRMWARE_BUILD_NUM_STR in src/config.h
 * ============================================================================
 * UDS & ISO-TP PROTOCOL ENGINE ARCHITECTURE:
 * 1. Atomic Cross-Core Synchronization (`udsMux` Spinlock):
 *    - Core 1 (`WebServer`) initiates UDS requests while Core 0 (`canDiagWorkerTask`)
 *      processes incoming CAN frames at 500 kbps.
 *    - `active_uds` is armed inside a `portENTER_CRITICAL(&udsMux)` block BEFORE
 *      calling `CanDriver::transmit()`, guaranteeing that sub-millisecond ECU
 *      responses are never missed.
 *
 * 2. ISO 15765-2 (ISO-TP) Multi-Frame Transport:
 *    - Single Frame (PCI 0x0N): Payload up to 7 bytes carried in a single frame.
 *    - First Frame  (PCI 0x1N + LenLSB): Announces a multi-byte payload (such as
 *      17-character VIN 0xF1EF, 40-byte ECM Live Inputs 0x0200, or multi-DTC list).
 *      Core 0 immediately arms the reassembler and transmits a Flow Control
 *      ContinueToSend (`30 00 00 ...`) frame to `rx_id - 8`.
 *    - Consecutive Frames (PCI 0x21..0x2F, wrapping to 0x20): Reassembled in
 *      strict sequence order. Any sequence mismatch aborts immediately so the
 *      caller can retry without waiting for timeout.
 *
 * 3. Background TesterPresent (0x3E 00 / 0x3E 80) Suppression:
 *    - When a module is selected, Core 0 transmits `02 3E 00 00 00 00 00 00`
 *      every 2000 ms to maintain the active diagnostic session.
 *    - While the user remains on the ABS Outputs page view (0x7E1), Core 0
 *      transmits `02 3E 80 00 00 00 00 00` every 2000 ms to maintain the
 *      Extended Diagnostic Session (`02 10 03`) without flooding CAN with 0x7E replies.
 *    - During any synchronous DID read or multi-DID sequence (`diag_sequence_active`),
 *      background TesterPresent frames are paused so they never collide with
 *      `0x22`, `0x2F`, `0x19`, or `0x14` requests on the CAN bus.
 * ============================================================================
 */

#include "uds_handler.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "can_driver.h"
#include "config.h"
#include "hd_dtc_db.h"

// CAN/UDS traffic trace: printed to the serial console ONLY when the operator enables it in Settings
// ("CAN traffic log"). Default off, so [UDS-TX]/[UDS-RX]/[ISOTP]/[UDS-SCAN] never flood the console or
// disturb timing during normal use.
#define CAN_TRACE(...)  do { if (settings.canlog_enabled) Serial.printf(__VA_ARGS__); } while (0)

static UdsPendingTransaction active_uds = {
    false, false, false, false, 0x7E0, 0x7E8, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 1, {0}, {0}, {0}
};

static EcmInputs0200 latest_ecm_inputs = {};
static AbsInputs0200 latest_abs_inputs = {};
static BcmInputs0200 latest_bcm_inputs = {};

// Static work buffers for Core 0 ISO-TP reassembly and Core 1 voltage safeguard
// (Prevents allocating ~1,712-byte EcmInputs0200 on FreeRTOS task stacks)
static EcmInputs0200 s_isotp_ecm_work = {};
static AbsInputs0200 s_isotp_abs_work = {};
static BcmInputs0200 s_isotp_bcm_work = {};
static EcmInputs0200 s_batt_ecm_work  = {};
static char          s_isotp_ascii_buf[248]   = {0};
static char          s_isotp_decoded_buf[320] = {0};

static portMUX_TYPE      udsMux                 = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// UDS Bus Lock (recursive mutex): serializes every synchronous request/response
// transaction because they all share the single `active_uds` slot. Needed since the
// live DID 0x0200 poller runs in its own task, concurrently with HTTP handlers.
// Recursive: composite calls (e.g. readEcmInputs0200 -> readDidSynchronous) re-enter safely.
// ============================================================================
static SemaphoreHandle_t getUdsBusMutex() {
    static SemaphoreHandle_t m = nullptr;
    if (m == nullptr) {
        SemaphoreHandle_t created = xSemaphoreCreateRecursiveMutex();
        portENTER_CRITICAL(&udsMux);
        if (m == nullptr) { m = created; created = nullptr; }
        portEXIT_CRITICAL(&udsMux);
        if (created != nullptr) vSemaphoreDelete(created);
    }
    return m;
}

struct UdsBusLock {
    bool ok;
    explicit UdsBusLock(TickType_t ticks = portMAX_DELAY) : ok(xSemaphoreTakeRecursive(getUdsBusMutex(), ticks) == pdTRUE) {}
    ~UdsBusLock() { if (ok) xSemaphoreGiveRecursive(getUdsBusMutex()); }
    UdsBusLock(const UdsBusLock &) = delete;
    UdsBusLock &operator=(const UdsBusLock &) = delete;
};
static volatile bool     tester_present_enabled = false;
static volatile uint32_t tester_present_target  = 0;
static volatile uint32_t last_tester_present_ms = 0;
static volatile bool     abs_outputs_page_active = false;
static volatile uint32_t last_abs_tp_80_ms      = 0;
static volatile bool     diag_sequence_active   = false;
static volatile bool     auto_fc_enabled        = true; // Flow Control ContinueToSend (0x30 CTS)

static volatile bool scan_in_progress = false;
static uint32_t scan_detected_ids[16] = {0};
static uint8_t  scan_count = 0;

// ISO-TP transmit flow control (FC) captured by inspectFrame() while rawTransaction() sends a multi-frame request
static volatile bool    s_txfc_wait = false;
static volatile bool    s_txfc_got  = false;
static volatile uint8_t s_txfc_fs = 0, s_txfc_bs = 0, s_txfc_st = 0;

// ============================================================================
// Runtime ECU Profile Latching (Determined via DID 0xF193 / 0xF1ED Calibration ID)
// ============================================================================
static EcuFamily current_ecu_family = ECU_FAMILY_TC_ETC; // Default to 77-byte CAN ETC/M8 map
static bool      ecu_family_latched = false;
static AbsFamily abs_family_current = ABS_FAMILY_DEFAULT;   // ABS (0x7E1) DID 0x0200 layout
static bool      abs_family_latched = false;
static BcmFamily bcm_family_current = BCM_FAMILY_DEFAULT;   // BCM (0x7E2) DID 0x0200 layout
static bool      bcm_family_latched = false;
static bool      bcm_family_by_rule = false;                // true: calibration ID matched a database rule (not the DEFAULT fallback)
static uint8_t   gen_layout_current = 0;                    // generic modules (config.h 5D)
static uint32_t  gen_layout_module  = 0;
static bool      gen_layout_latched = false;

void UdsHandler::identifyEcuFamily(const char* calibrationIdString) {
    if (calibrationIdString == nullptr || calibrationIdString[0] == '\0') return;

    // Prefix-based classification (see ECU_PREFIX_RULES / classifyCalibrationId() in config.h):
    //   M8       : 67..., 64..., 32107...
    //   TC_ETC   : 410..., 325..., 324..., 32109...   (also the DEFAULT when nothing matches)
    //   XL       : 329... or an ID containing "XL"
    //   TC_CABLE : legacy numeric prefix below the 300 series
    const char* rule = nullptr;
    current_ecu_family = classifyCalibrationId(calibrationIdString, &rule);
    ecu_family_latched = true;

    Serial.printf("[UDS-IDENT] ECU Profile determined: Family %d (rule '%s') for ID: %s\n",
                  (int)current_ecu_family, rule ? rule : "?", calibrationIdString);
}

void UdsHandler::resetEcuFamilyLatch() {
    // Called on module select/exit so a different ECU (or bike) is re-identified.
    ecu_family_latched = false;
    current_ecu_family = ECU_FAMILY_DEFAULT;
    abs_family_latched = false;
    abs_family_current = ABS_FAMILY_DEFAULT;
    bcm_family_latched = false;
    bcm_family_by_rule = false;
    bcm_family_current = BCM_FAMILY_DEFAULT;
    gen_layout_latched = false;
    gen_layout_module  = 0;
    gen_layout_current = 0;
}

// Reads the identification string used to pick a DID 0x0200 layout. The family is identified from the
// Parts Number (DID 0xF1EA) for BCM / SCM / LHC / RHC (these modules have no Calibration ID entry and answer NRC 0x31 to
// 0xF1ED); the other modules use the Calibration ID (0xF1ED, fallback 0xF193).
static bool readFamilyIdString(uint32_t targetModuleId, char *out, size_t out_len, uint32_t timeoutMs) {
    static const uint16_t kPartsFirst[] = {0xF1EA, 0xF1EE, HD_DID_CALIBRATION_ID, HD_DID_CALIBRATION_F193};
    static const uint16_t kCalFirst[]   = {HD_DID_CALIBRATION_ID, HD_DID_CALIBRATION_F193};
    const bool parts = (targetModuleId == HD_ECU_BCM || targetModuleId == HD_ECU_SCM ||
                        targetModuleId == HD_ECU_LHC || targetModuleId == HD_ECU_RHC);
    const uint16_t *list = parts ? kPartsFirst : kCalFirst;
    const size_t n = parts ? (sizeof(kPartsFirst) / sizeof(kPartsFirst[0])) : (sizeof(kCalFirst) / sizeof(kCalFirst[0]));
    for (size_t i = 0; i < n; i++) {
        out[0] = '\0';
        if (UdsHandler::readDidSynchronous(targetModuleId, list[i], out, out_len, timeoutMs) && out[0] != '\0') return true;
    }
    return false;
}

bool UdsHandler::readFamilyId(uint32_t targetModuleId, char *out, size_t out_len, uint32_t timeoutMs) {
    return readFamilyIdString(targetModuleId, out, out_len, timeoutMs);
}

void UdsHandler::identifyAbsFamily(const char* calibrationIdString) {
    if (calibrationIdString == nullptr || calibrationIdString[0] == '\0') return;
    const char* rule = nullptr;
    abs_family_current = classifyAbsCalibrationId(calibrationIdString, &rule);
    abs_family_latched = true;
    Serial.printf("[UDS-IDENT] ABS Profile determined: Family %s (rule '%s') for ID: %s\n",
                  ABS_FAMILY_MAPS[abs_family_current].name, rule ? rule : "?", calibrationIdString);
}

void UdsHandler::identifyBcmFamily(const char* calibrationIdString) {
    if (calibrationIdString == nullptr || calibrationIdString[0] == '\0') return;
    const char* rule = nullptr;
    bcm_family_current = classifyBcmCalibrationId(calibrationIdString, &rule);
    bcm_family_latched = true;
    bcm_family_by_rule = (rule != nullptr && strcmp(rule, "default") != 0);
    Serial.printf("[UDS-IDENT] BCM Profile determined: Family %s (rule '%s') for ID: %s\n",
                  BCM_FAMILY_MAPS[bcm_family_current].name, rule ? rule : "?", calibrationIdString);
}

void UdsHandler::identifyGenLayout(uint32_t moduleId, const char* calibrationIdString) {
    const GenModuleDef* m = findGenModule(moduleId);
    if (!m || calibrationIdString == nullptr || calibrationIdString[0] == '\0') return;
    const char* rule = nullptr;
    gen_layout_current = classifyGenCalibrationId(m, calibrationIdString, &rule);
    gen_layout_module  = moduleId;
    gen_layout_latched = true;
    Serial.printf("[UDS-IDENT] %s Profile determined: Layout %s (rule '%s') for ID: %s\n",
                  m->name, m->layouts[gen_layout_current].name, rule ? rule : "default", calibrationIdString);
}

BcmFamily UdsHandler::getCurrentBcmFamily() {
    return bcm_family_current;
}

AbsFamily UdsHandler::getCurrentAbsFamily() {
    return abs_family_current;
}

EcuFamily UdsHandler::getCurrentEcuFamily() {
    return current_ecu_family;
}

const DynamicParamMap* UdsHandler::getActiveParamMap() {
    switch (current_ecu_family) {
        case ECU_FAMILY_M8:       return Map_M8_0200;
        case ECU_FAMILY_TC_ETC:   return Map_TC_ETC_0200;
        case ECU_FAMILY_TC_CABLE: return Map_TC_CABLE_0200;
        case ECU_FAMILY_XL:       return Map_XL_0200;
        default:                  return Map_TC_ETC_0200;
    }
}

static const char* getHDDidName(uint16_t did) {
    switch (did) {
        case HD_DID_ECM_LIVE_INPUTS:     return "ECM Live Inputs";
        case HD_DID_VIN:                 return "VIN";
        case HD_DID_PARTS_NUMBER:        return "Parts Number";
        case HD_DID_CALIBRATION_ID:      return "Calibration ID";
        case HD_DID_CALIBRATION_F193:    return "Calibration ID (0xF193)";
        case HD_DID_HOMOLOGATION_ID:     return "Homologation ID";
        case HD_DID_ACT_COOLING_FAN:     return "Cooling Fan Relay";
        case HD_DID_ACT_FUEL_PUMP:       return "Fuel Pump Relay";
        case HD_DID_ACT_FRONT_INJECTOR:  return "Front Injector Actuator";
        case HD_DID_ACT_REAR_INJECTOR:   return "Rear Injector Actuator";
        case HD_DID_ACT_FRONT_COIL:      return "Front Ignition Coil";
        case HD_DID_ACT_REAR_COIL:       return "Rear Ignition Coil";
        case HD_DID_ACT_CHECK_ENGINE_MIL: return "Check Engine Light / MIL";
        case HD_DID_ACT_ABS_PUMP_MOTOR:  return "ABS Hydraulic Pump Motor";
        default:                             return "DID";
    }
}

bool UdsHandler::decodeLocalUdsFrame(uint32_t canIdentifier, uint8_t dlc, const uint8_t *data, char *outputSummaryBuffer, size_t maxOutputLength) {
    if (outputSummaryBuffer != nullptr && maxOutputLength > 0) {
        outputSummaryBuffer[0] = '\0';
    }

    bool is_diag = (canIdentifier >= HD_MIN_DIAG_ID && canIdentifier <= 0x7FF) || ((canIdentifier & 0xFFFF00) == 0x18DA00);
    if (!is_diag || data == nullptr || dlc < 1) {
        return false;
    }

    uint8_t pci_type = (data[0] >> 4) & 0x0F;

    if (pci_type == 0x00 && dlc >= 2) {
        uint8_t sf_len = data[0] & 0x0F;
        uint8_t sid    = data[1];

        if (sid == 0x7F && dlc >= 4) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS NegResp (RejSID 0x%02X, NRC 0x%02X)", data[2], data[3]);
            return true;
        }

        if (sid >= 0x50 && sid <= 0x7E) {
            uint8_t orig = sid - 0x40;
            if (orig == 0x22 && dlc >= 4) {
                uint16_t did = ((uint16_t)data[2] << 8) | data[3];
                snprintf(outputSummaryBuffer, maxOutputLength, "UDS PosResp 0x62 %s (0x%04X)", getHDDidName(did), did);
            } else if (orig == 0x19 && dlc >= 4) {
                snprintf(outputSummaryBuffer, maxOutputLength, "UDS PosResp 0x59 ReadDTC (Sub 0x%02X, Mask 0x%02X)", data[2], data[3]);
            } else if (orig == 0x14) {
                snprintf(outputSummaryBuffer, maxOutputLength, "UDS PosResp 0x54 ClearDTC OK");
            } else if (orig == 0x3E) {
                snprintf(outputSummaryBuffer, maxOutputLength, "UDS PosResp 0x7E TesterPresent OK");
            } else {
                snprintf(outputSummaryBuffer, maxOutputLength, "UDS PosResp 0x%02X (SID 0x%02X)", sid, orig);
            }
            return true;
        }

        if (sid == 0x22 && dlc >= 4) {
            uint16_t did = ((uint16_t)data[2] << 8) | data[3];
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x22 %s (0x%04X)", getHDDidName(did), did);
        } else if (sid == 0x19 && dlc >= 4) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x19 ReadDTC (Sub 0x%02X, Mask 0x%02X)", data[2], data[3]);
        } else if (sid == 0x3E) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x3E TesterPresent (%02X %02X %02X %02X)", data[0], data[1], data[2], data[3]);
        } else if (sid == 0x10 && dlc >= 3) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x10 SessionControl (Sub 0x%02X)", data[2]);
        } else if (sid == 0x14) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x14 ClearDTC");
        } else if (sid == 0x11 && dlc >= 3) {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS Req 0x11 ECUReset (Sub 0x%02X)", data[2]);
        } else {
            snprintf(outputSummaryBuffer, maxOutputLength, "UDS SF (Len %u, SID 0x%02X)", sf_len, sid);
        }
        return true;
    } else if (pci_type == 0x01 && dlc >= 3) {
        uint16_t total_len = ((uint16_t)(data[0] & 0x0F) << 8) | data[1];
        snprintf(outputSummaryBuffer, maxOutputLength, "ISO-TP First Frame (%u bytes, SID 0x%02X)", total_len, data[2]);
        return true;
    } else if (pci_type == 0x02) {
        snprintf(outputSummaryBuffer, maxOutputLength, "ISO-TP Consecutive Frame (Seq #%u)", data[0] & 0x0F);
        return true;
    } else if (pci_type == 0x03 && dlc >= 3) {
        snprintf(outputSummaryBuffer, maxOutputLength, "ISO-TP Flow Control (FS=%u, BS=%u, STmin=%u)", data[0] & 0x0F, data[1], data[2]);
        return true;
    }

    snprintf(outputSummaryBuffer, maxOutputLength, "UDS Diagnostic Frame (ID 0x%03X, DLC %u)", canIdentifier, dlc);
    return true;
}

void UdsHandler::armForManualRequest(uint32_t requestId, uint8_t sid) {
    uint32_t now = millis();
    portENTER_CRITICAL(&udsMux);
    active_uds.completed      = false;
    active_uds.is_negative    = false;
    active_uds.isotp_multi    = false;
    active_uds.req_id         = requestId;
    active_uds.expected_rx_id = (requestId == 0x7DF) ? 0x7E8 : (requestId + 8);
    active_uds.rx_id          = 0;
    active_uds.sid            = sid;
    active_uds.target_did     = 0;
    active_uds.start_ms       = now;
    active_uds.elapsed_ms     = 0;
    active_uds.rx_dlc         = 0;
    memset(active_uds.rx_data, 0, sizeof(active_uds.rx_data));
    active_uds.expected_len   = 0;
    active_uds.collected_len  = 0;
    active_uds.cf_count       = 0;
    active_uds.expected_sn    = 1;
    memset(active_uds.full_payload, 0, sizeof(active_uds.full_payload));
    active_uds.ascii_payload[0] = '\0';
    active_uds.decoded[0]     = '\0';
    active_uds.pending        = true;
    last_tester_present_ms    = now;
    portEXIT_CRITICAL(&udsMux);
}

bool UdsHandler::sendDiagnosticRequest(uint32_t targetModuleId, uint8_t sid, uint8_t subFunction, uint8_t didLowByte, const char *rawHexString) {
    uint8_t payload[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    if (rawHexString != nullptr && strlen(rawHexString) > 0) {
        char buf[64];
        strncpy(buf, rawHexString, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, " ,");
        int idx = 0;
        while (tok != nullptr && idx < 8) {
            payload[idx++] = (uint8_t)strtoul(tok, nullptr, 16);
            tok = strtok(nullptr, " ,");
        }
        sid = payload[1];
    } else if (sid == 0x22) {
        payload[0] = 0x03;
        payload[1] = 0x22;
        payload[2] = subFunction;
        payload[3] = didLowByte;
    } else if (sid == 0x19) {
        // ReadDTCInformation: 03 19 02 09 00 00 00 00
        payload[0] = 0x03;
        payload[1] = 0x19;
        payload[2] = 0x02;
        payload[3] = 0x09;
    } else if (sid == 0x3E) {
        // TesterPresent: 03 3E 00 01 00 00 00 00
        payload[0] = 0x03;
        payload[1] = 0x3E;
        payload[2] = 0x00;
        payload[3] = 0x01;
    } else if (sid == 0x14) {
        payload[0] = 0x04;
        payload[1] = 0x14;
        payload[2] = 0xFF;
        payload[3] = 0xFF;
        payload[4] = 0xFF;
    } else {
        payload[0] = 0x02;
        payload[1] = sid;
        payload[2] = subFunction;
    }

    uint16_t req_did = (sid == 0x22) ? (((uint16_t)payload[2] << 8) | payload[3]) : 0;
    uint32_t now = millis();

    // Arm the physical ECU response verifier atomically BEFORE transmitting onto the CAN bus
    // so Core 0 is ready even if the 500kbps ECU replies in <300 microseconds.
    portENTER_CRITICAL(&udsMux);
    active_uds.completed      = false;
    active_uds.is_negative    = false;
    active_uds.isotp_multi    = false;
    active_uds.req_id         = targetModuleId;
    active_uds.expected_rx_id = (targetModuleId == 0x7DF) ? 0x7E8 : (targetModuleId + 8);
    active_uds.rx_id          = 0;
    active_uds.sid            = sid;
    active_uds.target_did     = req_did;
    active_uds.start_ms       = now;
    active_uds.elapsed_ms     = 0;
    active_uds.rx_dlc         = 0;
    memset(active_uds.rx_data, 0, sizeof(active_uds.rx_data));
    active_uds.expected_len   = 0;
    active_uds.collected_len  = 0;
    active_uds.cf_count       = 0;
    active_uds.expected_sn    = 1;
    memset(active_uds.full_payload, 0, sizeof(active_uds.full_payload));
    active_uds.ascii_payload[0] = '\0';
    active_uds.decoded[0]     = '\0';
    active_uds.pending        = true;
    last_tester_present_ms    = now;
    portEXIT_CRITICAL(&udsMux);

    CAN_TRACE("[UDS-TX] ID=0x%03X (ExpectRX=0x%03X) | SID=0x%02X | Frame=[%02X %02X %02X %02X %02X %02X %02X %02X]\n",
                  targetModuleId, (targetModuleId == 0x7DF) ? 0x7E8 : (targetModuleId + 8), sid,
                  payload[0], payload[1], payload[2], payload[3],
                  payload[4], payload[5], payload[6], payload[7]);

    if (CanDriver::transmit(targetModuleId, false, false, 8, payload)) {
        last_tester_present_ms = millis(); // Defer background TesterPresent keepalive while active query runs
        if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
            metrics.uds_queries_count++;
            xSemaphoreGive(metricsMutex);
        }
        return true;
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending = false;
    portEXIT_CRITICAL(&udsMux);
    CAN_TRACE("[UDS-TX] ERROR: CAN transmit failed for ID=0x%03X, SID=0x%02X\n", targetModuleId, sid);
    return false;
}

void UdsHandler::inspectFrame(const twai_message_t &msg) {
    if (msg.rtr || msg.data_length_code < 1) {
        return;
    }

    // Decode any local CAN bus UDS diagnostic frame (0x7E0..0x7FF)
    char uds_summary[96] = {0};
    bool is_uds = decodeLocalUdsFrame(msg.identifier, msg.data_length_code, msg.data, uds_summary, sizeof(uds_summary));

    const uint8_t pci_type = (msg.data[0] >> 4) & 0x0F;

    // During active broadcast scan (0x7DF TesterPresent), capture ALL present HD ECU responses
    // Present modules reply from (moduleId + 8) in 0x7E8..0x7FF (both 0x7E positive and 0x7F 3E negative)
    if (scan_in_progress && !msg.extd && IS_HD_RX_ID(msg.identifier) && msg.data_length_code >= 2) {
        uint8_t sid = msg.data[1];
        bool is_pos_tp = (sid == 0x7E) || (msg.data[0] == 0x7E);
        bool is_neg_tp = (sid == 0x7F && msg.data_length_code >= 3 && msg.data[2] == 0x3E);
        bool is_any_uds = is_pos_tp || is_neg_tp || (sid >= 0x50 && sid <= 0x7F);
        if (is_any_uds) {
            uint32_t mod_id = msg.identifier - 8;
            // Exclude 0x7E8..0x7EF as request IDs (they are reply IDs for 0x7E0..0x7E7)
            bool isValidReqId = (mod_id == HD_ECU_SCM) || (mod_id >= HD_ECU_ECM && mod_id <= HD_ECU_BCM) ||
                                 (mod_id == HD_ECU_LHC) || (mod_id == HD_ECU_RHC) || (mod_id == HD_ECU_EHCU) ||
                                 (mod_id == HD_ECU_RADIO) || (mod_id == HD_ECU_SPEEDO);   // only known module IDs
            if (isValidReqId) {
                bool already = false;
                for (uint8_t i = 0; i < scan_count; i++) {
                    if (scan_detected_ids[i] == mod_id) {
                        already = true;
                        break;
                    }
                }
                if (!already && scan_count < 16) {
                    scan_detected_ids[scan_count++] = mod_id;
                    CAN_TRACE("[UDS-SCAN] Discovered ECU Module at Request ID 0x%03X (Reply ID 0x%03X, SID 0x%02X)\n",
                                  mod_id, msg.identifier, sid);
                }
            }
        }
    }

    // Snapshot current active_uds state atomically
    bool     is_pending        = false;
    bool     is_isotp_multi    = false;
    uint32_t requestId            = 0;
    uint32_t expected_rx_id    = 0;
    uint32_t multi_rx_id       = 0;
    uint8_t  expected_sid      = 0;
    uint16_t expected_did      = 0;

    portENTER_CRITICAL(&udsMux);
    is_pending     = active_uds.pending;
    is_isotp_multi = active_uds.isotp_multi;
    requestId         = active_uds.req_id;
    expected_rx_id = active_uds.expected_rx_id;
    multi_rx_id    = active_uds.rx_id;
    expected_sid   = active_uds.sid;
    expected_did   = active_uds.target_did;
    portEXIT_CRITICAL(&udsMux);

    // 1. Handle ISO-TP Consecutive Frames (0x20..0x2F) when multi-frame reception is active
    if (is_pending && is_isotp_multi && pci_type == 0x02 && msg.identifier == multi_rx_id) {
        uint8_t sn = msg.data[0] & 0x0F;
        uint32_t now = millis();

        portENTER_CRITICAL(&udsMux);
        if (sn != active_uds.expected_sn) {
            // Out of sequence frame received: abort multi-frame transaction and notify caller so it can retry.
            // 'completed' is published only AFTER the message text is written (no torn reads by the waiter).
            uint8_t exp_sn = active_uds.expected_sn;
            active_uds.pending     = false;
            active_uds.isotp_multi = false;
            portEXIT_CRITICAL(&udsMux);

            char errmsg[sizeof(active_uds.decoded)];
            snprintf(errmsg, sizeof(errmsg),
                     "ISO-TP Sequence Error (Expected SN %u, received %u) - Aborted", exp_sn, sn);
            portENTER_CRITICAL(&udsMux);
            memcpy(active_uds.decoded, errmsg, sizeof(active_uds.decoded));
            active_uds.is_negative = true;
            active_uds.completed   = true;
            portEXIT_CRITICAL(&udsMux);

            CAN_TRACE("[ISOTP] ERROR: Sequence mismatch on 0x%03X (expected SN=%u, got SN=%u). Aborting transfer.\n",
                          msg.identifier, exp_sn, sn);
            return;
        }

        // Expected sequence number begins at 1 after First Frame, increments from 0x1 to 0xF, and rolls over to 0x0
        active_uds.expected_sn = (active_uds.expected_sn + 1) & 0x0F;

        // Safe incremental indexing: append Consecutive Frame payload bytes right where the last frame left off
        uint8_t avail = (msg.data_length_code > 8) ? 7 : (msg.data_length_code > 1 ? (msg.data_length_code - 1) : 0);
        for (uint8_t i = 0; i < avail; i++) {
            if (active_uds.collected_len < active_uds.expected_len &&
                active_uds.collected_len < sizeof(active_uds.full_payload)) {
                active_uds.full_payload[active_uds.collected_len] = msg.data[1 + i];
                active_uds.collected_len++;
            }
        }

        active_uds.cf_count++;
        active_uds.start_ms    = now; // Refresh timeout while CFs are streaming
        last_tester_present_ms = now;
        bool all_collected     = (active_uds.collected_len >= active_uds.expected_len);
        uint16_t exp_len       = active_uds.collected_len;
        uint8_t  cf_cnt        = active_uds.cf_count;
        uint8_t  local_payload[128];
        if (all_collected) {
            active_uds.isotp_multi = false;
            memcpy(local_payload, active_uds.full_payload, sizeof(local_payload));
        }
        portEXIT_CRITICAL(&udsMux);

        if (all_collected) {
            memset(s_isotp_ascii_buf, 0, sizeof(s_isotp_ascii_buf));
            memset(s_isotp_decoded_buf, 0, sizeof(s_isotp_decoded_buf));
            uint8_t data_offset = (local_payload[0] == 0x62 && exp_len >= 3) ? 3 : 1;
            size_t  data_len    = (exp_len > data_offset) ? (exp_len - data_offset) : 0;

            if (local_payload[0] == 0x62 && exp_len >= 3) {
                uint16_t did = ((uint16_t)local_payload[1] << 8) | local_payload[2];
                if (did == HD_DID_ECM_LIVE_INPUTS && exp_len > 3) {
                    // Format the complete binary DID 0x0200 payload (e.g. all 77 bytes of 80B ISO-TP)
                    // directly as uppercase HEX without truncation or ASCII heuristic stripping
                    size_t hex_idx = 0;
                    const uint8_t *raw_data = local_payload + 3;
                    for (size_t i = 0; i < data_len && (hex_idx + 4) < sizeof(s_isotp_ascii_buf); i++) {
                        int w = snprintf(
                            s_isotp_ascii_buf + hex_idx,
                            sizeof(s_isotp_ascii_buf) - hex_idx,
                            (i == 0) ? "%02X" : " %02X",
                            raw_data[i]
                        );
                        if (w <= 0) break;
                        hex_idx += (size_t)w;
                    }

                    if (msg.identifier == (HD_ECU_ABS + 8)) {
                        if (parseAbsInputs0200(local_payload + 3, exp_len - 3, s_isotp_abs_work)) {
                            portENTER_CRITICAL(&udsMux);
                            latest_abs_inputs = s_isotp_abs_work;
                            portEXIT_CRITICAL(&udsMux);
                        }
                    } else if (msg.identifier == (HD_ECU_BCM + 8)) {
                        if (parseBcmInputs0200(local_payload + 3, exp_len - 3, s_isotp_bcm_work)) {
                            portENTER_CRITICAL(&udsMux);
                            latest_bcm_inputs = s_isotp_bcm_work;
                            portEXIT_CRITICAL(&udsMux);
                        }
                    } else if (msg.identifier == (HD_ECU_ECM + 8)) {
                        if (parseEcmInputs0200(local_payload + 3, exp_len - 3, s_isotp_ecm_work)) {
                            portENTER_CRITICAL(&udsMux);
                            latest_ecm_inputs = s_isotp_ecm_work;
                            portEXIT_CRITICAL(&udsMux);
                        }
                    }
                } else {
                    cleanAsciiString(
                        local_payload + data_offset,
                        data_len,
                        s_isotp_ascii_buf,
                        sizeof(s_isotp_ascii_buf)
                    );
                }
                snprintf(
                    s_isotp_decoded_buf,
                    sizeof(s_isotp_decoded_buf),
                    "ISO-TP PosResp 0x62 %s (0x%04X, %uB, %u CFs): %s",
                    getHDDidName(did),
                    did,
                    exp_len,
                    cf_cnt,
                    s_isotp_ascii_buf
                );
            } else {
                cleanAsciiString(
                    local_payload + data_offset,
                    data_len,
                    s_isotp_ascii_buf,
                    sizeof(s_isotp_ascii_buf)
                );
                snprintf(
                    s_isotp_decoded_buf,
                    sizeof(s_isotp_decoded_buf),
                    "ISO-TP Multi-Frame Complete (%uB, %u CFs): %s",
                    exp_len,
                    cf_cnt,
                    s_isotp_ascii_buf
                );
            }

            portENTER_CRITICAL(&udsMux);
            active_uds.elapsed_ms = now - active_uds.start_ms;
            strncpy(active_uds.ascii_payload, s_isotp_ascii_buf, sizeof(active_uds.ascii_payload) - 1);
            active_uds.ascii_payload[sizeof(active_uds.ascii_payload) - 1] = '\0';
            strncpy(active_uds.decoded, s_isotp_decoded_buf, sizeof(active_uds.decoded) - 1);
            active_uds.decoded[sizeof(active_uds.decoded) - 1] = '\0';
            active_uds.is_negative = false;
            active_uds.isotp_multi = false;
            active_uds.pending     = false;
            active_uds.completed   = true;
            portEXIT_CRITICAL(&udsMux);

            CAN_TRACE("[ISOTP] Reassembly Complete from 0x%03X: %s\n", msg.identifier, s_isotp_decoded_buf);

            if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
                metrics.uds_replies_count++;
                xSemaphoreGive(metricsMutex);
            }
        }
        return;
    }

    // 2. Check if incoming frame is an ISO-TP First Frame (0x10..0x1F) from a HD ECU response ID (0x7E8..0x7FF)
    bool is_std_ecu_rx = (!msg.extd && IS_HD_RX_ID(msg.identifier));
    bool id_matches = false;
    if (is_pending) {
        if (requestId == 0x7DF) {
            id_matches = is_std_ecu_rx;
        } else {
            id_matches = (!msg.extd && msg.identifier == expected_rx_id);
        }
    }

    if (pci_type == 0x01 && msg.data_length_code >= 3 && (id_matches || (auto_fc_enabled && is_std_ecu_rx))) {
        uint16_t total_len = ((uint16_t)(msg.data[0] & 0x0F) << 8) | msg.data[1];
        uint32_t fc_tx_id  = 0x7E0;
        if (is_pending && requestId != 0 && requestId != 0x7DF && msg.identifier == expected_rx_id) {
            fc_tx_id = requestId;
        } else if (IS_HD_RX_ID(msg.identifier)) {
            fc_tx_id = msg.identifier - 8;
        } else if (requestId != 0 && requestId != 0x7DF) {
            fc_tx_id = requestId;
        }

        // If an active 0x22 ReadDID request is waiting for a specific DID, verify the First Frame matches that DID
        bool ff_matches_active_query = id_matches;
        if (is_pending && id_matches && expected_sid == 0x22 && expected_did != 0) {
            if (msg.data[2] != 0x62) {
                ff_matches_active_query = false;
            } else if (msg.data_length_code >= 5) {
                uint16_t ff_did = ((uint16_t)msg.data[3] << 8) | msg.data[4];
                if (ff_did != expected_did) {
                    ff_matches_active_query = false;
                }
            }
        }

        bool    ff_armed  = false;   // logged AFTER Flow Control is on the bus (keeps CF latency minimal)
        uint8_t ff_copied = 0;
        if (ff_matches_active_query || (!is_pending && auto_fc_enabled)) {
            uint32_t now = millis();
            // Arm Consecutive Frame reassembler BEFORE sending Flow Control so Core 0 is ready
            // before the ECU puts the first Consecutive Frame (0x21) onto the 500kbps bus.
            portENTER_CRITICAL(&udsMux);
            active_uds.pending        = true;
            active_uds.completed      = false;
            active_uds.is_negative    = false;
            active_uds.isotp_multi    = true;
            active_uds.rx_id          = msg.identifier;
            active_uds.expected_rx_id = msg.identifier;
            active_uds.rx_dlc         = (msg.data_length_code > 8) ? 8 : msg.data_length_code;
            memcpy(active_uds.rx_data, msg.data, active_uds.rx_dlc);
            active_uds.expected_len   = (total_len > sizeof(active_uds.full_payload)) ? sizeof(active_uds.full_payload) : total_len;
            active_uds.cf_count       = 0;
            active_uds.expected_sn    = 1; // Expected sequence number begins at 1 after a First Frame
            memset(active_uds.full_payload, 0, sizeof(active_uds.full_payload));
            active_uds.ascii_payload[0] = '\0';

            // Safely initialize collected_len = 0 before copying First Frame payload bytes (msg.data[2 + i])
            active_uds.collected_len  = 0;
            uint8_t ff_avail = (active_uds.rx_dlc > 2) ? (active_uds.rx_dlc - 2) : 0;
            for (uint8_t i = 0; i < ff_avail; i++) {
                if (active_uds.collected_len < active_uds.expected_len &&
                    active_uds.collected_len < sizeof(active_uds.full_payload)) {
                    active_uds.full_payload[active_uds.collected_len] = msg.data[2 + i];
                    active_uds.collected_len++;
                }
            }

            active_uds.start_ms    = now;
            last_tester_present_ms = now;
            if (is_uds) {
                strncpy(active_uds.decoded, uds_summary, sizeof(active_uds.decoded) - 1);
                active_uds.decoded[sizeof(active_uds.decoded) - 1] = '\0';
            }
            ff_copied = active_uds.collected_len;
            portEXIT_CRITICAL(&udsMux);
            ff_armed = true;
        }

        // Immediately transmit ISO-TP Flow Control ContinueToSend (30 00 00 00 00 00 00 00)
        // FS=0 (ContinueToSend), BS=0 (Unbounded block size), STmin=0ms
        if (is_pending || auto_fc_enabled) {
            const uint8_t fc_frame[8] = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            CanDriver::transmit(fc_tx_id, msg.extd, false, 8, fc_frame);
        }
        if (ff_armed) {
            CAN_TRACE("[ISOTP] First Frame RX from 0x%03X (TotalLen=%u B, CopiedFF=%u B, SID=0x%02X) -> Flow Control (0x30 CTS) sent to 0x%03X\n",
                          msg.identifier, total_len, ff_copied, msg.data[2], fc_tx_id);
        }
        return;
    }

    // ISO-TP Flow Control frame from the ECU while a multi-frame request is being transmitted
    if (s_txfc_wait && is_pending && pci_type == 0x03 && !msg.extd && msg.identifier == expected_rx_id && msg.data_length_code >= 3) {
        s_txfc_fs  = msg.data[0] & 0x0F;
        s_txfc_bs  = msg.data[1];
        s_txfc_st  = msg.data[2];
        s_txfc_got = true;
        return;
    }

    // Do not allow stray Single Frames (e.g. TesterPresent replies) to interrupt an active multi-frame transfer
    if (!is_pending || is_isotp_multi || msg.extd || !id_matches || msg.data_length_code < 2) {
        return;
    }

    // 3. Handle Single Frame (pci_type == 0x00) responses
    if (pci_type == 0x00) {
        const uint8_t resp_sid = msg.data[1];

        // If Negative Response (0x7F), strictly verify that the rejected SID (msg.data[2]) matches our active request SID!
        // This prevents a negative response to background TesterPresent (0x3E) from aborting an active ReadDID (0x22) query.
        if (resp_sid == 0x7F) {
            if (msg.data_length_code < 4 || msg.data[2] != expected_sid) {
                return;
            }
            // Extend P2* timeout on NRC 0x78 (RequestCorrectlyReceived-ResponsePending)
            if (msg.data[3] == 0x78) {
                uint32_t now = millis();
                portENTER_CRITICAL(&udsMux);
                active_uds.start_ms    = now;
                last_tester_present_ms = now;
                portEXIT_CRITICAL(&udsMux);
                CAN_TRACE("[UDS-RX] NRC 0x78 (ResponsePending) from 0x%03X for SID 0x%02X -> Extending P2* timer.\n",
                              msg.identifier, expected_sid);
                return;
            }
        }

        if (resp_sid == (uint8_t)(expected_sid + 0x40) || resp_sid == 0x7F) {
            // For Positive Response to ReadDID (0x62), strictly verify that the 16-bit DID matches target_did
            if (resp_sid == 0x62) {
                if (msg.data_length_code < 4) {
                    return;
                }
                uint16_t rx_did = ((uint16_t)msg.data[2] << 8) | msg.data[3];
                if (expected_did != 0 && rx_did != expected_did) {
                    return;
                }
            }

            uint8_t rx_dlc = (msg.data_length_code > 8) ? 8 : msg.data_length_code;
            uint8_t sf_len = msg.data[0] & 0x0F;
            uint8_t copy_len = 0;
            uint8_t local_payload[128] = {0};
            if (sf_len > 0 && sf_len <= 7) {
                copy_len = (sf_len < rx_dlc) ? sf_len : (rx_dlc - 1);
                memcpy(local_payload, msg.data + 1, copy_len);
            }

            char ascii_buf[32] = {0};
            char decoded_buf[160] = {0};
            if (resp_sid == 0x62 && rx_dlc >= 4) {
                uint16_t did = ((uint16_t)msg.data[2] << 8) | msg.data[3];
                if (copy_len > 3) {
                    cleanAsciiString(local_payload + 3, copy_len - 3, ascii_buf, sizeof(ascii_buf));
                } else {
                    cleanAsciiString(msg.data + 4, rx_dlc - 4, ascii_buf, sizeof(ascii_buf));
                }
                snprintf(decoded_buf, sizeof(decoded_buf), "PosResp 0x62 %s (0x%04X): %s", getHDDidName(did), did, ascii_buf);
            } else if (is_uds) {
                strncpy(decoded_buf, uds_summary, sizeof(decoded_buf) - 1);
                decoded_buf[sizeof(decoded_buf) - 1] = '\0';
            }

            uint32_t now = millis();
            portENTER_CRITICAL(&udsMux);
            active_uds.rx_id         = msg.identifier;
            active_uds.rx_dlc        = rx_dlc;
            memcpy(active_uds.rx_data, msg.data, rx_dlc);
            active_uds.is_negative   = (resp_sid == 0x7F);
            active_uds.elapsed_ms    = now - active_uds.start_ms;
            active_uds.expected_len  = copy_len;
            active_uds.collected_len = copy_len;
            memset(active_uds.full_payload, 0, sizeof(active_uds.full_payload));
            if (copy_len > 0) {
                memcpy(active_uds.full_payload, local_payload, copy_len);
            }
            strncpy(active_uds.ascii_payload, ascii_buf, sizeof(active_uds.ascii_payload) - 1);
            active_uds.ascii_payload[sizeof(active_uds.ascii_payload) - 1] = '\0';
            strncpy(active_uds.decoded, decoded_buf, sizeof(active_uds.decoded) - 1);
            active_uds.decoded[sizeof(active_uds.decoded) - 1] = '\0';
            active_uds.isotp_multi   = false;
            active_uds.pending       = false;
            active_uds.completed     = true;
            last_tester_present_ms   = now;
            portEXIT_CRITICAL(&udsMux);

            CAN_TRACE("[UDS-RX] Single Frame from 0x%03X (%u ms): %s\n",
                          msg.identifier, active_uds.elapsed_ms, decoded_buf);

            if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
                metrics.uds_replies_count++;
                xSemaphoreGive(metricsMutex);
            }
        }
    }
}

bool UdsHandler::isLocalUdsResponse(uint32_t canIdentifier) {
    bool match = false;
    portENTER_CRITICAL(&udsMux);
    if (active_uds.pending) {
        if (active_uds.isotp_multi && canIdentifier == active_uds.rx_id) {
            match = true;
        } else if (active_uds.req_id == 0x7DF) {
            match = IS_HD_RX_ID(canIdentifier);
        } else {
            match = (canIdentifier == active_uds.expected_rx_id);
        }
    }
    portEXIT_CRITICAL(&udsMux);
    return match;
}

void UdsHandler::setTesterPresent(bool enable, uint32_t targetModuleId) {
    if (!enable || targetModuleId < HD_MIN_DIAG_ID || targetModuleId > HD_MAX_DIAG_ID) {
        if (tester_present_enabled) {
            Serial.println("[UDS-TP] Stopping background TesterPresent (0x3E 00) keepalive.");
        }
        tester_present_enabled = false;
        tester_present_target  = 0;
        last_tester_present_ms = 0;
    } else {
        bool target_changed = (targetModuleId != tester_present_target);
        tester_present_enabled = true;
        tester_present_target  = targetModuleId;
        if (target_changed || last_tester_present_ms == 0) {
            Serial.printf("[UDS-TP] Starting 2000ms TesterPresent (0x3E 00) keepalive for Module 0x%03X.\n", targetModuleId);
            // Transmit the initial single Tester Present keepalive frame upon entering module
            last_tester_present_ms = millis();
            const uint8_t tp_frame[8] = {0x02, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            CanDriver::transmit(tester_present_target, false, false, 8, tp_frame);
        }
    }
    if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
        metrics.tester_present_active = tester_present_enabled;
        metrics.tester_present_target = tester_present_target;
        xSemaphoreGive(metricsMutex);
    }
}

bool UdsHandler::isTesterPresentEnabled() {
    return tester_present_enabled;
}

uint32_t UdsHandler::getTesterPresentTarget() {
    return tester_present_target;
}

void UdsHandler::setDiagnosticSequenceActive(bool active) {
    diag_sequence_active = active;
    if (active) {
        last_tester_present_ms = millis();
    }
}

bool UdsHandler::isDiagnosticSequenceActive() {
    return diag_sequence_active;
}

void UdsHandler::setAutoFlowControl(bool enable) {
    auto_fc_enabled = enable;
}

bool UdsHandler::isAutoFlowControlEnabled() {
    return auto_fc_enabled;
}

void UdsHandler::loop() {
    uint32_t now = millis();

    // 1. Check P2 / ISO-TP N_Cr Timeout (1500 ms) for pending local UDS request
    bool is_busy = false;
    portENTER_CRITICAL(&udsMux);
    if (active_uds.pending && (now - active_uds.start_ms >= 1500)) {
        active_uds.elapsed_ms  = now - active_uds.start_ms;
        active_uds.is_negative = true;
        active_uds.completed   = true;
        active_uds.pending     = false;
        active_uds.isotp_multi = false;
    }
    is_busy = active_uds.pending || active_uds.isotp_multi || diag_sequence_active || scan_in_progress;
    portEXIT_CRITICAL(&udsMux);

    // 2. ABS Outputs Page View Periodic TesterPresent (02 3E 80 00 00 00 00 00 every 2000ms targeting 0x7E1):
    //    Maintains the ABS Extended Diagnostic Session (02 10 03) while the user remains on the ABS Outputs page.
    if (!is_busy && abs_outputs_page_active) {
        if (now - last_abs_tp_80_ms >= 2000) {
            last_abs_tp_80_ms      = now;
            last_tester_present_ms = now;
            const uint8_t abs_tp_frame[8] = {0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
            CanDriver::transmit(HD_ECU_ABS, false, false, 8, abs_tp_frame);
            if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
                metrics.uds_queries_count++;
                metrics.tester_present_active = true;
                metrics.tester_present_target = HD_ECU_ABS;
                xSemaphoreGive(metricsMutex);
            }
        }
        return;
    }

    // 3. Standard Background Tester Present (0x3E 00) keepalive:
    //    Only transmit background keepalive when no active UDS query or multi-DID sequence is in progress
    if (!is_busy &&
        tester_present_enabled && tester_present_target >= HD_MIN_DIAG_ID && tester_present_target <= HD_MAX_DIAG_ID) {
        if (now - last_tester_present_ms >= 2000) {
            last_tester_present_ms = now;
            const uint8_t tp_frame[8] = {0x02, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            CanDriver::transmit(tester_present_target, false, false, 8, tp_frame);
            if (xSemaphoreTake(metricsMutex, 0) == pdTRUE) {
                metrics.uds_queries_count++;
                metrics.tester_present_active = true;
                metrics.tester_present_target = tester_present_target;
                xSemaphoreGive(metricsMutex);
            }
        }
    }
}

UdsPendingTransaction UdsHandler::getLatestResult() {
    UdsPendingTransaction snap;
    portENTER_CRITICAL(&udsMux);
    snap = active_uds;
    portEXIT_CRITICAL(&udsMux);
    return snap;
}

void UdsHandler::cleanAsciiString(const uint8_t *rawBytes, size_t len, char *outputString, size_t maxOutputStringLength) {
    if (outputString == nullptr || maxOutputStringLength == 0) return;
    outputString[0] = '\0';
    if (rawBytes == nullptr || len == 0) return;

    // Trim trailing non-printable bytes (0x00, 0xFF, < 32, > 126, or '.')
    size_t end = len;
    while (end > 0) {
        uint8_t b = rawBytes[end - 1];
        if (b == 0x00 || b == 0xFF || b < 32 || b > 126 || b == '.') {
            end--;
        } else {
            break;
        }
    }

    // Trim leading nulls / 0xFF (do not skip valid characters or whitespace)
    size_t start = 0;
    while (start < end && (rawBytes[start] == 0x00 || rawBytes[start] == 0xFF)) {
        start++;
    }

    // Check count of printable ASCII characters
    size_t ascii_count = 0;
    for (size_t i = start; i < end; i++) {
        uint8_t c = rawBytes[i];
        if (c >= 32 && c <= 126) {
            ascii_count++;
        }
    }

    // If valid ASCII: at least 2 chars and at least 60% printable
    if (end > start && ascii_count >= 2 && ((ascii_count * 10) / (end - start)) >= 6) {
        size_t out_idx = 0;
        for (size_t i = start; i < end && out_idx < maxOutputStringLength - 1; i++) {
            uint8_t c = rawBytes[i];
            outputString[out_idx++] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        // Cut any trailing dots '....'
        while (out_idx > 0 && outputString[out_idx - 1] == '.') {
            out_idx--;
        }
        outputString[out_idx] = '\0';
        if (out_idx > 0) return;
    }

    // Fallback: If cannot convert to ASCII, display read raw HEX values!
    size_t hex_idx = 0;
    for (size_t i = 0; i < len && hex_idx + 4 < maxOutputStringLength; i++) {
        int written = snprintf(outputString + hex_idx, maxOutputStringLength - hex_idx, (i == 0) ? "%02X" : " %02X", rawBytes[i]);
        if (written <= 0) break;
        hex_idx += written;
    }
}

void UdsHandler::formatDtcCode(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t status, HDDtcRecord &rec) {
    static const char PFX_MAP[4] = {'P', 'C', 'B', 'U'};
    char pfx = PFX_MAP[(b0 >> 6) & 0x03];
    uint8_t d1 = (b0 >> 4) & 0x03;
    uint8_t d2 = b0 & 0x0F;
    uint8_t d3 = (b1 >> 4) & 0x0F;
    uint8_t d4 = b1 & 0x0F;
    uint8_t ftb = b2;

    rec.status_byte = status;
    snprintf(rec.raw_hex, sizeof(rec.raw_hex), "%02X%02X%02X", b0, b1, b2);

    // Format code: e.g. B2206-00, U0141-01, U0141-02, U0156-03, U0184-01
    snprintf(rec.code, sizeof(rec.code), "%c%X%X%X%X-%02X", pfx, d1, d2, d3, d4, ftb);

    if (status & 0x08) {
        snprintf(rec.status_desc, sizeof(rec.status_desc), "Confirmed Fault (0x%02X)", status);
    } else if (status & 0x04) {
        snprintf(rec.status_desc, sizeof(rec.status_desc), "Pending Fault (0x%02X)", status);
    } else if (status & 0x01) {
        snprintf(rec.status_desc, sizeof(rec.status_desc), "Test Failed (0x%02X)", status);
    } else {
        snprintf(rec.status_desc, sizeof(rec.status_desc), "(0x%02X)", status);
    }
    rec.status_desc[sizeof(rec.status_desc) - 1] = '\0';

    char base_code[8];
    snprintf(base_code, sizeof(base_code), "%c%X%X%X%X", pfx, d1, d2, d3, d4);

    const char *dtc_desc = lookupHDDtcDescription(base_code);
    strncpy(rec.description, dtc_desc, sizeof(rec.description) - 1);
    rec.description[sizeof(rec.description) - 1] = '\0';
}

bool UdsHandler::readDidSynchronous(uint32_t targetModuleId, uint16_t did, char *outputAsciiBuffer, size_t maxOutputLength, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    if (outputAsciiBuffer == nullptr || maxOutputLength == 0) return false;
    outputAsciiBuffer[0] = '\0';

    uint8_t hi = (did >> 8) & 0xFF;
    uint8_t lo = did & 0xFF;

    // Suppress background TesterPresent during synchronous DID reads
    bool prev_seq_active = diag_sequence_active;
    diag_sequence_active = true;

    // If a TesterPresent frame was transmitted within the last 60ms, allow its reply to settle first
    uint32_t since_tp = millis() - last_tester_present_ms;
    if (last_tester_present_ms != 0 && since_tp < 60) {
        delay(60 - since_tp);
    }
    last_tester_present_ms = millis();

    uint32_t per_try_timeout = (timeoutMs >= 450) ? timeoutMs : 450;
    const int MAX_QUERY_ATTEMPTS = 2;

    for (int query_attempt = 0; query_attempt < MAX_QUERY_ATTEMPTS; query_attempt++) {
        if (query_attempt > 0) {
            Serial.printf("[UDS-DID] Retrying ReadDataByIdentifier (0x22) for %s (0x%04X) on ECU 0x%03X (Attempt %d/%d)...\n",
                          getHDDidName(did), did, targetModuleId, query_attempt + 1, MAX_QUERY_ATTEMPTS);
            delay(60);
        } else {
            Serial.printf("[UDS-DID] Requesting %s (DID 0x%04X) from ECU 0x%03X...\n",
                          getHDDidName(did), did, targetModuleId);
        }

        // Ensure the 0x22 DID request frame is transmitted onto the CAN bus
        bool sent = false;
        for (int tx_try = 0; tx_try < 3 && !sent; tx_try++) {
            sent = sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DID, hi, lo);
            if (!sent) {
                delay(20);
            }
        }
        if (!sent) {
            Serial.printf("[UDS-DID] ERROR: Failed to transmit ReadDID 0x%04X to 0x%03X (attempt %d)\n", did, targetModuleId, query_attempt + 1);
            continue;
        }

        uint32_t wait_start = millis();
        while (millis() - wait_start < per_try_timeout) {
            bool     is_done       = false;
            bool     is_neg        = false;
            bool     in_multi      = false;
            uint32_t last_frame_ms = 0;
            uint16_t col_len       = 0;
            uint8_t  rx_dlc        = 0;
            uint8_t  local_payload[128];
            uint8_t  local_rx_data[8];

            portENTER_CRITICAL(&udsMux);
            is_done       = active_uds.completed;
            is_neg        = active_uds.is_negative;
            in_multi      = active_uds.isotp_multi;
            last_frame_ms = active_uds.start_ms;
            if (is_done && !is_neg) {
                col_len = active_uds.collected_len;
                rx_dlc  = active_uds.rx_dlc;
                memcpy(local_payload, active_uds.full_payload, sizeof(local_payload));
                memcpy(local_rx_data, active_uds.rx_data, sizeof(local_rx_data));
            }
            portEXIT_CRITICAL(&udsMux);

            // Extend wait window while ISO-TP Consecutive Frames are actively streaming
            if (in_multi && (millis() - last_frame_ms < 250)) {
                wait_start = millis();
            }

            if (is_done) {
                bool success = false;
                if (!is_neg) {
                    // Verify 0x62 (Positive Response) + 2 bytes of DID (MSB, LSB)
                    const uint8_t offset = 3;
                    if (col_len > offset && local_payload[0] == 0x62) {
                        uint16_t resp_did = ((uint16_t)local_payload[1] << 8) | local_payload[2];
                        if (resp_did == did) {
                            size_t data_len = col_len - offset;
                            cleanAsciiString(local_payload + offset, data_len, outputAsciiBuffer, maxOutputLength);
                            success = (outputAsciiBuffer[0] != '\0');
                        }
                    } else if (rx_dlc >= 4 && local_rx_data[1] == 0x62) {
                        uint16_t resp_did = ((uint16_t)local_rx_data[2] << 8) | local_rx_data[3];
                        if (resp_did == did) {
                            uint8_t sf_len = local_rx_data[0] & 0x0F;
                            uint8_t avail = (sf_len >= 3 && sf_len <= 7) ? (sf_len - 3) : (rx_dlc > 4 ? rx_dlc - 4 : 0);
                            if (avail > 0) {
                                cleanAsciiString(local_rx_data + 4, avail, outputAsciiBuffer, maxOutputLength);
                                success = (outputAsciiBuffer[0] != '\0');
                            }
                        }
                    }
                }

                portENTER_CRITICAL(&udsMux);
                active_uds.pending     = false;
                active_uds.completed   = false;
                active_uds.isotp_multi = false;
                last_tester_present_ms = millis();
                portEXIT_CRITICAL(&udsMux);

                if (success) {
                    if (did == HD_DID_CALIBRATION_F193 || did == HD_DID_CALIBRATION_ID) {
                        // Family tables are per module: ECM (0x7E0) and ABS (0x7E1) use different calibration families.
                        if (targetModuleId == HD_ECU_ECM) identifyEcuFamily(outputAsciiBuffer);
                        else if (targetModuleId == HD_ECU_ABS) identifyAbsFamily(outputAsciiBuffer);
                        else if (targetModuleId == HD_ECU_BCM) identifyBcmFamily(outputAsciiBuffer);
                        else if (findGenModule(targetModuleId)) identifyGenLayout(targetModuleId, outputAsciiBuffer);
                    }
                    if (did == HD_DID_PARTS_NUMBER) {
                        // BCM / SCM / LHC / RHC have no Calibration ID: the database selects their layout by Parts Number.
                        if (targetModuleId == HD_ECU_BCM) identifyBcmFamily(outputAsciiBuffer);
                        else if (targetModuleId == HD_ECU_SCM || targetModuleId == HD_ECU_LHC || targetModuleId == HD_ECU_RHC) identifyGenLayout(targetModuleId, outputAsciiBuffer);
                    }
                    Serial.printf("[UDS-DID] SUCCESS: %s (0x%04X) = '%s'\n", getHDDidName(did), did, outputAsciiBuffer);
                    delay(15); // Brief bus settling guard after completing DID reception
                    diag_sequence_active = prev_seq_active;
                    return true;
                }
                // If negative or aborted, break inner wait loop and retry
                break;
            }
            delay(5);
        }
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);

    Serial.printf("[UDS-DID] TIMEOUT: No valid response for %s (0x%04X) from ECU 0x%03X.\n",
                  getHDDidName(did), did, targetModuleId);
    diag_sequence_active = prev_seq_active;
    return false;
}

/**
 * ============================================================================
 * DID 0x0200: Abstract Multi-Profile ECM Live Data Parser (41 Parameters)
 * ============================================================================
 * Implements the strict Two-Step Separated Extraction Execution Strategy:
 * - Step 0: Boundary Checks (`payloadLength >= 65`, optional `0x62 0x02 0x00` strip,
 *           `len >= 42`) and ECU Family Profile Selection (`active_map`).
 * - Step 1: Dedicated Pure Analog Extraction Loop (`0` .. `IDX_IAC_STEPS`).
 * - Step 2: Isolated Bitwise Flags & Discrete Extraction Loop
 *           (`IDX_FLAG_RUN` .. `TOTAL_PARAM_COUNT - 1`).
 */
bool UdsHandler::parseEcmInputs0200(const uint8_t *rawPayloadBytes, size_t payloadLength, EcmInputs0200 &out) {
    memset(&out, 0, sizeof(EcmInputs0200));

    // Step 0: Boundary Checks & Profile Selection
    if (rawPayloadBytes == nullptr || payloadLength < 65) {
        return false;
    }

    const uint8_t *b = rawPayloadBytes;
    size_t len = payloadLength;

    // Safely strip the 3-byte UDS Positive Response header (0x62 0x02 0x00) if present
    if (len >= 68 && b[0] == 0x62 && b[1] == 0x02 && b[2] == 0x00) {
        b   += 3;
        len -= 3;
    }

    if (len < 42) {
        return false;
    }

    // Dynamically assign active_map based on current_ecu_family
    const DynamicParamMap *active_map = nullptr;
    switch (current_ecu_family) {
        case ECU_FAMILY_M8:
            active_map = Map_M8_0200;
            break;
        case ECU_FAMILY_TC_ETC:
            active_map = Map_TC_ETC_0200;
            break;
        case ECU_FAMILY_TC_CABLE:
            active_map = Map_TC_CABLE_0200;
            break;
        case ECU_FAMILY_XL:
            active_map = Map_XL_0200;
            break;
        default:
            active_map = Map_M8_0200;
            break;
    }

    out.valid        = true;
    out.raw_len      = (uint16_t)len;
    out.timestamp_ms = millis();
    out.ecu_family   = (current_ecu_family == ECU_FAMILY_UNKNOWN) ? ECU_FAMILY_M8 : current_ecu_family;

    // ========================================================================
    // Step 1 Loop: Pure Analog Extractions (0 .. IDX_IAC_STEPS)
    // ========================================================================
    for (int i = 0; i <= (int)IDX_IAC_STEPS; i++) {
        const DynamicParamMap &p = active_map[i];

        if (p.byte_offset == 0 && p.data_length == 0) {
            out.param_is_na[i]     = true;
            out.param_values[i]    = 0.0f;
            out.param_is_string[i] = true;
            strncpy(out.param_strings[i], "N/A", sizeof(out.param_strings[i]) - 1);

            if (strcmp(p.json_key, "twistgrip_position") == 0)          out.twistgrip_position = 0.0f;
            else if (strcmp(p.json_key, "twistgrip_sensor_1") == 0)     out.twistgrip_sensor_1 = 0.0f;
            else if (strcmp(p.json_key, "twistgrip_sensor_2") == 0)     out.twistgrip_sensor_2 = 0.0f;
        } else if ((size_t)p.byte_offset + (size_t)p.data_length > len) {
            out.param_is_na[i]     = true;
            out.param_values[i]    = 0.0f;
            out.param_is_string[i] = true;
            strncpy(out.param_strings[i], "N/A", sizeof(out.param_strings[i]) - 1);
        } else {
            uint16_t raw = 0;
            if (p.data_length == 2) {
                raw = (uint16_t)(((uint16_t)b[p.byte_offset] << 8) | (uint16_t)b[p.byte_offset + 1]);
            } else {
                raw = (uint16_t)b[p.byte_offset];
            }

            float scaled_val = ((float)raw * p.scaling) + p.offset;
            out.param_is_na[i]     = false;
            out.param_values[i]    = scaled_val;
            out.param_is_string[i] = false;
            snprintf(out.param_strings[i], sizeof(out.param_strings[i]), "%.2f", (double)scaled_val);

            if (strcmp(p.json_key, "battery_voltage") == 0)                     out.battery_voltage = scaled_val;
            else if (strcmp(p.json_key, "map_kpa") == 0)                        { out.map_kpa = scaled_val; out.map = scaled_val; }
            else if (strcmp(p.json_key, "map_volts") == 0)                      out.map_volts = scaled_val;
            else if (strcmp(p.json_key, "engine_temperature") == 0)             { out.engine_temperature = scaled_val; out.cylinder_head_temp = scaled_val; }
            else if (strcmp(p.json_key, "engine_temperature_sensor") == 0)      { out.engine_temperature_sensor = scaled_val; out.cylinder_head_temp_voltage = scaled_val; }
            else if (strcmp(p.json_key, "intake_air_temperature") == 0)         { out.intake_air_temperature = scaled_val; out.intake_air_temp = scaled_val; }
            else if (strcmp(p.json_key, "intake_air_temperature_sensor") == 0)  { out.intake_air_temperature_sensor = scaled_val; out.iat_voltage = scaled_val; }
            else if (strcmp(p.json_key, "throttle_position") == 0)              out.throttle_position = scaled_val;
            else if (strcmp(p.json_key, "throttle_position_sensor_1") == 0)     { out.throttle_position_sensor_1 = scaled_val; out.tps_voltage = scaled_val; }
            else if (strcmp(p.json_key, "throttle_position_sensor_2") == 0)     out.throttle_position_sensor_2 = scaled_val;
            else if (strcmp(p.json_key, "twistgrip_position") == 0)             out.twistgrip_position = scaled_val;
            else if (strcmp(p.json_key, "twistgrip_sensor_1") == 0)             out.twistgrip_sensor_1 = scaled_val;
            else if (strcmp(p.json_key, "twistgrip_sensor_2") == 0)             out.twistgrip_sensor_2 = scaled_val;
            else if (strcmp(p.json_key, "idle_set_speed") == 0)                 { out.idle_set_speed = scaled_val; out.idle_speed_required = scaled_val; }
            else if (strcmp(p.json_key, "engine_speed") == 0)                   out.engine_speed = scaled_val;
            else if (strcmp(p.json_key, "vehicle_speed") == 0)                  out.vehicle_speed = scaled_val;
            else if (strcmp(p.json_key, "front_closed_loop_integrator") == 0)   { out.front_closed_loop_integrator = scaled_val; out.afr_front_integrator = scaled_val; }
            else if (strcmp(p.json_key, "rear_closed_loop_integrator") == 0)    { out.rear_closed_loop_integrator = scaled_val; out.afr_rear_integrator = scaled_val; }
            else if (strcmp(p.json_key, "front_adaptive_fuel_value") == 0)      out.front_adaptive_fuel_value = scaled_val;
            else if (strcmp(p.json_key, "rear_adaptive_fuel_value") == 0)       out.rear_adaptive_fuel_value = scaled_val;
            else if (strcmp(p.json_key, "front_oxygen_sensor") == 0)            { out.front_oxygen_sensor = scaled_val; out.o2_sensor_front = scaled_val; }
            else if (strcmp(p.json_key, "rear_oxygen_sensor") == 0)             { out.rear_oxygen_sensor = scaled_val; out.o2_sensor_rear = scaled_val; }
            else if (strcmp(p.json_key, "injector_time_front") == 0)            { out.injector_time_front = scaled_val; out.injector_pw_front = scaled_val; }
            else if (strcmp(p.json_key, "injector_time_rear") == 0)             { out.injector_time_rear = scaled_val; out.injector_pw_rear = scaled_val; }
            else if (strcmp(p.json_key, "spark_advance_front") == 0)            out.spark_advance_front = scaled_val;
            else if (strcmp(p.json_key, "spark_advance_rear") == 0)             out.spark_advance_rear = scaled_val;
            else if (strcmp(p.json_key, "front_spark_knock_retard") == 0)       { out.front_spark_knock_retard = scaled_val; out.knock_front_retard = scaled_val; }
            else if (strcmp(p.json_key, "rear_spark_knock_retard") == 0)        { out.rear_spark_knock_retard = scaled_val; out.knock_rear_retard = scaled_val; }
            else if (strcmp(p.json_key, "idle_air_control_steps") == 0)         { out.idle_air_control_steps = scaled_val; out.idle_air_control = scaled_val; }
        }
    }

    // ========================================================================
    // Step 2 Loop: Bitwise Flags & Discrete Extractions (IDX_FLAG_RUN .. TOTAL_PARAM_COUNT - 1)
    // ========================================================================
    for (int i = (int)IDX_FLAG_RUN; i < (int)TOTAL_PARAM_COUNT; i++) {
        const DynamicParamMap &p = active_map[i];

        if (p.byte_offset == 0 && p.data_length == 0) {
            out.param_is_na[i]     = true;
            out.param_values[i]    = 0.0f;
            out.param_is_string[i] = true;
            strncpy(out.param_strings[i], "N/A", sizeof(out.param_strings[i]) - 1);

            if (strcmp(p.json_key, "cruise_brake_input") == 0)          strncpy(out.cruise_brake_input, "N/A", sizeof(out.cruise_brake_input) - 1);
            else if (strcmp(p.json_key, "cruise_set") == 0)             strncpy(out.cruise_set, "N/A", sizeof(out.cruise_set) - 1);
            else if (strcmp(p.json_key, "cruise_res") == 0)             strncpy(out.cruise_res, "N/A", sizeof(out.cruise_res) - 1);
            else if (strcmp(p.json_key, "cruise_enable_switch") == 0)   strncpy(out.cruise_enable_switch, "N/A", sizeof(out.cruise_enable_switch) - 1);
            else if (strcmp(p.json_key, "cruise_mode") == 0)            strncpy(out.cruise_mode, "N/A", sizeof(out.cruise_mode) - 1);
            else if (strcmp(p.json_key, "cruise_rolloff_input") == 0)   strncpy(out.cruise_rolloff_input, "N/A", sizeof(out.cruise_rolloff_input) - 1);
        } else if ((size_t)p.byte_offset + (size_t)p.data_length > len) {
            out.param_is_na[i]     = true;
            out.param_values[i]    = 0.0f;
            out.param_is_string[i] = true;
            strncpy(out.param_strings[i], "N/A", sizeof(out.param_strings[i]) - 1);
        } else if (strcmp(p.json_key, "gear_position") == 0) {
            uint8_t gear_raw       = b[p.byte_offset];
            out.param_is_na[i]     = false;
            out.param_values[i]    = (float)gear_raw;
            out.param_is_string[i] = true;
            if (gear_raw == 0) {
                snprintf(out.gear_position, sizeof(out.gear_position), "Neutral");
            } else {
                snprintf(out.gear_position, sizeof(out.gear_position), "Gear %u", (unsigned int)gear_raw);
            }
            strncpy(out.param_strings[i], out.gear_position, sizeof(out.param_strings[i]) - 1);
        } else {
            bool bit_active       = (b[p.byte_offset] & p.bit_mask) == p.bit_mask;
            const char *state_str = bit_active ? "On" : "Off";

            if (strcmp(p.json_key, "run_mode_flag") == 0) {
                state_str = bit_active ? "On" : "Off";
                strncpy(out.run_mode_flag, state_str, sizeof(out.run_mode_flag) - 1);
                strncpy(out.engine_run_stop_switch, state_str, sizeof(out.engine_run_stop_switch) - 1);
            } else if (strcmp(p.json_key, "tssm_security_state") == 0) {
                state_str = bit_active ? "Armed" : "Disarmed";
                strncpy(out.tssm_security_state, state_str, sizeof(out.tssm_security_state) - 1);
                strncpy(out.vehicle_theft_deterrent, state_str, sizeof(out.vehicle_theft_deterrent) - 1);
            } else if (strcmp(p.json_key, "bank_angle_sensor_mode") == 0) {
                state_str = bit_active ? "Tipped" : "Run Mode";
                strncpy(out.bank_angle_sensor_mode, state_str, sizeof(out.bank_angle_sensor_mode) - 1);
                strncpy(out.vehicle_tipped, state_str, sizeof(out.vehicle_tipped) - 1);
            } else if (strcmp(p.json_key, "neutral_switch_input") == 0) {
                state_str = bit_active ? "In Neutral" : "In Gear";
                strncpy(out.neutral_switch_input, state_str, sizeof(out.neutral_switch_input) - 1);
                strncpy(out.neutral_switch, state_str, sizeof(out.neutral_switch) - 1);
            } else if (strcmp(p.json_key, "clutch_lever_switch_state") == 0) {
                state_str = bit_active ? "Pressed" : "Released";
                strncpy(out.clutch_lever_switch_state, state_str, sizeof(out.clutch_lever_switch_state) - 1);
                strncpy(out.clutch_switch, state_str, sizeof(out.clutch_switch) - 1);
            } else if (strncmp(p.json_key, "cruise_", 7) == 0) {
                state_str = bit_active ? "on" : "off";
                if (strcmp(p.json_key, "cruise_brake_input") == 0)         strncpy(out.cruise_brake_input, state_str, sizeof(out.cruise_brake_input) - 1);
                else if (strcmp(p.json_key, "cruise_set") == 0)            strncpy(out.cruise_set, state_str, sizeof(out.cruise_set) - 1);
                else if (strcmp(p.json_key, "cruise_res") == 0)            strncpy(out.cruise_res, state_str, sizeof(out.cruise_res) - 1);
                else if (strcmp(p.json_key, "cruise_enable_switch") == 0)  strncpy(out.cruise_enable_switch, state_str, sizeof(out.cruise_enable_switch) - 1);
                else if (strcmp(p.json_key, "cruise_mode") == 0)           strncpy(out.cruise_mode, state_str, sizeof(out.cruise_mode) - 1);
                else if (strcmp(p.json_key, "cruise_rolloff_input") == 0)  strncpy(out.cruise_rolloff_input, state_str, sizeof(out.cruise_rolloff_input) - 1);
            }

            out.param_is_na[i]     = false;
            out.param_values[i]    = bit_active ? 1.0f : 0.0f;
            out.param_is_string[i] = true;
            strncpy(out.param_strings[i], state_str, sizeof(out.param_strings[i]) - 1);
        }
    }

    return true;
}

EcmInputs0200 UdsHandler::getLatestEcmInputs0200() {
    EcmInputs0200 snap;
    portENTER_CRITICAL(&udsMux);
    snap = latest_ecm_inputs;
    portEXIT_CRITICAL(&udsMux);
    return snap;
}

bool UdsHandler::readEcmInputs0200Synchronous(uint32_t targetModuleId, EcmInputs0200 &out, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&out, 0, sizeof(EcmInputs0200));
    if (targetModuleId == 0) targetModuleId = HD_ECU_ECM;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // If the user enters "Read Module Inputs" directly without reading ECU Info first,
    // probe Calibration ID (0xF1ED / 0xF193) once to latch the ECU family profile.
    // The latch is set only by a SUCCESSFUL identification; failed probes are retried at most every 3 s.
    static uint32_t last_family_probe_ms = 0;
    if (!ecu_family_latched && targetModuleId == HD_ECU_ECM &&
        (last_family_probe_ms == 0 || (millis() - last_family_probe_ms) > 3000)) {
        last_family_probe_ms = millis();
        char calib_probe[48] = {0};
        if (readDidSynchronous(targetModuleId, HD_DID_CALIBRATION_ID, calib_probe, sizeof(calib_probe), 350) ||
            readDidSynchronous(targetModuleId, HD_DID_CALIBRATION_F193, calib_probe, sizeof(calib_probe), 350)) {
            identifyEcuFamily(calib_probe);
        }
        delay(25);
    }

    uint32_t since_tp = millis() - last_tester_present_ms;
    if (last_tester_present_ms != 0 && since_tp < 50) {
        delay(50 - since_tp);
    }
    last_tester_present_ms = millis();

    const uint8_t hi = (HD_DID_ECM_LIVE_INPUTS >> 8) & 0xFF;
    const uint8_t lo = HD_DID_ECM_LIVE_INPUTS & 0xFF;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            delay(40);
        }

        if (!sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DID, hi, lo)) {
            continue;
        }

        uint32_t wait_start = millis();
        while (millis() - wait_start < timeoutMs) {
            bool     is_done       = false;
            bool     is_neg        = false;
            bool     in_multi      = false;
            uint32_t last_frame_ms = 0;
            uint16_t col_len       = 0;
            uint8_t  local_payload[128];

            portENTER_CRITICAL(&udsMux);
            is_done       = active_uds.completed;
            is_neg        = active_uds.is_negative;
            in_multi      = active_uds.isotp_multi;
            last_frame_ms = active_uds.start_ms;
            if (is_done && !is_neg) {
                col_len = active_uds.collected_len;
                memcpy(local_payload, active_uds.full_payload, sizeof(local_payload));
            }
            portEXIT_CRITICAL(&udsMux);

            if (in_multi && (millis() - last_frame_ms < 250)) {
                wait_start = millis();
            }

            if (is_done) {
                bool parsed_ok = false;
                if (!is_neg && col_len > 3 && local_payload[0] == 0x62 &&
                    local_payload[1] == hi && local_payload[2] == lo) {
                    parsed_ok = parseEcmInputs0200(local_payload + 3, col_len - 3, out);
                    if (parsed_ok) {
                        portENTER_CRITICAL(&udsMux);
                        latest_ecm_inputs = out;
                        portEXIT_CRITICAL(&udsMux);
                    }
                }

                portENTER_CRITICAL(&udsMux);
                active_uds.pending     = false;
                active_uds.completed   = false;
                active_uds.isotp_multi = false;
                last_tester_present_ms = millis();
                portEXIT_CRITICAL(&udsMux);

                if (parsed_ok) {
                    Serial.printf("[UDS-0200] Parsed ECM DID 0x0200 (%u bytes): Batt=%.1fV, RPM=%.0f, CHT=%.0fC, TPS=%.1f%%, RunSw=%s\n",
                                  out.raw_len, out.battery_voltage, out.engine_speed,
                                  out.cylinder_head_temp, out.throttle_position, out.engine_run_stop_switch);
                    diag_sequence_active = prev_seq;
                    return true;
                }
                break;
            }
            delay(4);
        }
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    return false;
}

/**
 * ============================================================================
 * DID 0x0200: ABS (0x7E1) Read Module Inputs - Live Sensor Payload Parser
 * ============================================================================
 * Extracts all ABS live sensor, switch, relay, and hydraulic valve parameters
 * from the reassembled UDS payload (Bytes 0..11) with strict bounds checking.
 * Safely strips the 3-byte UDS Positive Response header (0x62 0x02 0x00) only
 * if truly present (len >= 15) so Byte 0 always aligns with Front Wheel Speed.
 */
bool UdsHandler::parseAbsInputs0200(const uint8_t *rawPayloadBytes, size_t payloadLength, AbsInputs0200 &out) {
    memset(&out, 0, sizeof(AbsInputs0200));
    if (rawPayloadBytes == nullptr || payloadLength < 1) {
        return false;
    }

    const uint8_t *b = rawPayloadBytes;
    size_t len = payloadLength;

    // Callers pass the payload AFTER the 62 02 00 header (no header sniffing: a data byte could mimic it).
    // Table-driven decode (config.h 5B): the family map was chosen from the ABS Calibration ID.
    const AbsFamilyMap &fm = ABS_FAMILY_MAPS[abs_family_current];
    out.family       = (uint8_t)abs_family_current;
    out.raw_len      = (uint16_t)len;
    out.timestamp_ms = millis();

    for (uint8_t i = 0; i < fm.value_count; i++) {
        const AbsValueDef &d = fm.values[i];
        if ((size_t)d.offset + d.len > len) continue;            // beyond received payload -> not decoded
        uint32_t raw = b[d.offset];
        if (d.len == 2) raw = (raw << 8) | b[d.offset + 1];       // 16-bit big-endian
        out.values[d.idx]  = (float)raw * d.scale + d.add;
        out.value_present |= (uint16_t)(1U << d.idx);
    }
    for (uint8_t i = 0; i < fm.state_count; i++) {
        const AbsStateDef &d = fm.states[i];
        if ((size_t)d.offset >= len) continue;
        out.state_present |= (1UL << d.idx);
        if ((b[d.offset] & d.mask) == d.mask) out.state_on |= (1UL << d.idx);
    }

    // Convenience copies for interlocks / header telemetry
    out.front_wheel_speed       = out.values[ABSV_FRONT_WHEEL_SPEED];
    out.rear_wheel_speed        = out.values[ABSV_REAR_WHEEL_SPEED];
    out.rear_right_wheel_speed  = out.values[ABSV_REAR_RIGHT_WHEEL_SPEED];
    out.vehicle_speed           = out.values[ABSV_VEHICLE_SPEED];
    out.abs_ecu_module_voltage  = out.values[ABSV_ABS_ECU_MODULE_VOLTAGE];

    out.valid = (out.value_present != 0 || out.state_present != 0);
    return out.valid;
}

AbsInputs0200 UdsHandler::getLatestAbsInputs0200() {
    AbsInputs0200 snap;
    portENTER_CRITICAL(&udsMux);
    snap = latest_abs_inputs;
    portEXIT_CRITICAL(&udsMux);
    return snap;
}

bool UdsHandler::readAbsInputs0200Synchronous(uint32_t targetModuleId, AbsInputs0200 &out, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&out, 0, sizeof(AbsInputs0200));
    if (targetModuleId == 0) targetModuleId = HD_ECU_ABS;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // Determine the ABS DID 0x0200 layout from the ABS Calibration ID (once per module selection;
    // a failed probe is retried at most every 3 s and decoding falls back to the DEFAULT family meanwhile).
    static uint32_t last_abs_probe_ms = 0;
    if (!abs_family_latched && targetModuleId == HD_ECU_ABS &&
        (last_abs_probe_ms == 0 || (millis() - last_abs_probe_ms) > 3000)) {
        last_abs_probe_ms = millis();
        char abs_cal[48] = {0};
        if (readDidSynchronous(targetModuleId, HD_DID_CALIBRATION_ID, abs_cal, sizeof(abs_cal), 350) ||
            readDidSynchronous(targetModuleId, HD_DID_CALIBRATION_F193, abs_cal, sizeof(abs_cal), 350)) {
            identifyAbsFamily(abs_cal);
        }
        delay(25);
    }

    uint32_t since_tp = millis() - last_tester_present_ms;
    if (last_tester_present_ms != 0 && since_tp < 50) {
        delay(50 - since_tp);
    }
    last_tester_present_ms = millis();

    const uint8_t hi = (HD_DID_ABS_LIVE_INPUTS >> 8) & 0xFF;
    const uint8_t lo = HD_DID_ABS_LIVE_INPUTS & 0xFF;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            delay(40);
        }

        if (!sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DID, hi, lo)) {
            continue;
        }

        uint32_t wait_start = millis();
        while (millis() - wait_start < timeoutMs) {
            bool     is_done       = false;
            bool     is_neg        = false;
            bool     in_multi      = false;
            uint32_t last_frame_ms = 0;
            uint16_t col_len       = 0;
            uint8_t  local_payload[128];

            portENTER_CRITICAL(&udsMux);
            is_done       = active_uds.completed;
            is_neg        = active_uds.is_negative;
            in_multi      = active_uds.isotp_multi;
            last_frame_ms = active_uds.start_ms;
            if (is_done && !is_neg) {
                col_len = active_uds.collected_len;
                memcpy(local_payload, active_uds.full_payload, sizeof(local_payload));
            }
            portEXIT_CRITICAL(&udsMux);

            if (in_multi && (millis() - last_frame_ms < 250)) {
                wait_start = millis();
            }

            if (is_done) {
                bool parsed_ok = false;
                if (!is_neg && col_len > 3 && local_payload[0] == 0x62 &&
                    local_payload[1] == hi && local_payload[2] == lo) {
                    parsed_ok = parseAbsInputs0200(local_payload + 3, col_len - 3, out);
                    if (parsed_ok) {
                        portENTER_CRITICAL(&udsMux);
                        latest_abs_inputs = out;
                        portEXIT_CRITICAL(&udsMux);
                    }
                }

                portENTER_CRITICAL(&udsMux);
                active_uds.pending     = false;
                active_uds.completed   = false;
                active_uds.isotp_multi = false;
                last_tester_present_ms = millis();
                portEXIT_CRITICAL(&udsMux);

                if (parsed_ok) {
                    Serial.printf("[UDS-ABS-0200] Parsed ABS DID 0x0200 (%u bytes, family %s): FWS=%.2f km/h, RWS=%.2f km/h, Volt=%.2fV\n",
                                  out.raw_len, ABS_FAMILY_MAPS[out.family].name,
                                  out.front_wheel_speed, out.rear_wheel_speed, out.abs_ecu_module_voltage);
                    diag_sequence_active = prev_seq;
                    return true;
                }
                break;
            }
            delay(4);
        }
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    return false;
}

bool UdsHandler::parseBcmInputs0200(const uint8_t *data, size_t len, BcmInputs0200 &out) {
    memset(&out, 0, sizeof(BcmInputs0200));
    if (data == nullptr || len < 1) {
        return false;
    }

    // Callers pass the payload AFTER the 62 02 00 header (no header sniffing: a data byte could mimic it).
    uint16_t copy_len = (len > sizeof(out.raw_bytes)) ? (uint16_t)sizeof(out.raw_bytes) : (uint16_t)len;
    memcpy(out.raw_bytes, data, copy_len);
    out.raw_len      = (uint16_t)len;
    out.update_ms    = millis();
    out.timestamp_ms = out.update_ms;
    // Table-driven decode (config.h 5C), family chosen from the BCM Calibration ID. When the calibration ID is not
    // listed in the database (or could not be read) the payload length selects the layout: family E (2-byte voltage,
    // tip-over at byte 47) needs >= 48 bytes, every other family uses the 28..31-byte layout (B = most calibrations).
    BcmFamily fam = bcm_family_current;
    if (!bcm_family_by_rule) {
        fam = (len >= 48) ? BCM_FAMILY_E : BCM_FAMILY_B;
    }
    out.family = (uint8_t)fam;
    const BcmFamilyMap &fm = BCM_FAMILY_MAPS[fam];
    if ((size_t)fm.voltage.offset + fm.voltage.len <= len) {
        uint32_t raw = data[fm.voltage.offset];
        if (fm.voltage.len == 2) raw = (raw << 8) | data[fm.voltage.offset + 1];
        out.system_voltage  = (float)raw * fm.voltage.scale;
        out.voltage_present = true;
    }
    for (uint8_t i = 0; i < fm.state_count; i++) {
        const BcmStateDef &d = fm.states[i];
        if ((size_t)d.offset >= len) continue;               // beyond received payload -> not decoded
        out.state_present |= (1ULL << d.idx);
        if ((data[d.offset] & d.mask) == d.mask) out.state_on |= (1ULL << d.idx);
    }

    out.valid = out.voltage_present || out.state_present != 0;
    return out.valid;
}

BcmInputs0200 UdsHandler::getLatestBcmInputs0200() {
    BcmInputs0200 snap;
    portENTER_CRITICAL(&udsMux);
    snap = latest_bcm_inputs;
    portEXIT_CRITICAL(&udsMux);
    return snap;
}

bool UdsHandler::readBcmInputs0200Synchronous(uint32_t targetModuleId, BcmInputs0200 &out, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&out, 0, sizeof(BcmInputs0200));
    if (targetModuleId == 0) targetModuleId = HD_ECU_BCM;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // Determine the BCM DID 0x0200 layout from the BCM Calibration ID (once per module selection;
    // a failed probe is retried at most every 3 s and decoding uses the DEFAULT family meanwhile).
    static uint32_t last_bcm_probe_ms = 0;
    if (!bcm_family_latched && targetModuleId == HD_ECU_BCM &&
        (last_bcm_probe_ms == 0 || (millis() - last_bcm_probe_ms) > 3000)) {
        last_bcm_probe_ms = millis();
        char bcm_cal[48] = {0};
        if (readFamilyIdString(targetModuleId, bcm_cal, sizeof(bcm_cal), 350)) {
            identifyBcmFamily(bcm_cal);
        }
        delay(25);
    }

    const uint8_t did_hi = (uint8_t)((HD_DID_BCM_LIVE_INPUTS >> 8) & 0xFF);
    const uint8_t did_lo = (uint8_t)(HD_DID_BCM_LIVE_INPUTS & 0xFF);

    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            delay(25);
        }

        const uint32_t request_ms = millis();
        if (!sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DID, did_hi, did_lo, "03 22 02 00 00 00 00 00")) {
            continue;
        }

        uint32_t wait_start = millis();
        while (millis() - wait_start < timeoutMs) {
            bool done          = false;
            bool is_neg        = false;
            uint16_t rx_did    = 0;
            uint16_t exp_len   = 0;
            uint8_t  rx_dlc    = 0;
            uint8_t  sf_data[8] = {0};

            portENTER_CRITICAL(&udsMux);
            if (active_uds.completed) {
                done    = true;
                is_neg  = active_uds.is_negative;
                exp_len = active_uds.expected_len;
                rx_dlc  = active_uds.rx_dlc;
                memcpy(sf_data, active_uds.rx_data, 8);
                if (exp_len >= 3 && active_uds.full_payload[0] == 0x62) {
                    rx_did = ((uint16_t)active_uds.full_payload[1] << 8) | active_uds.full_payload[2];
                } else if (rx_dlc >= 4 && sf_data[1] == 0x62) {
                    rx_did = ((uint16_t)sf_data[2] << 8) | sf_data[3];
                }
            }
            portEXIT_CRITICAL(&udsMux);

            if (done) {
                if (is_neg) {
                    break;
                }

                bool parsed_ok = false;
                if (latest_bcm_inputs.valid && (int32_t)(latest_bcm_inputs.update_ms - request_ms) >= 0) {   // parsed AFTER this request (never stale)
                    out = getLatestBcmInputs0200();
                    parsed_ok = true;
                } else if (rx_did == HD_DID_BCM_LIVE_INPUTS && exp_len > 3) {
                    uint8_t local_buf[64] = {0};
                    uint16_t data_len = exp_len - 3;
                    if (data_len > sizeof(local_buf)) data_len = sizeof(local_buf);
                    portENTER_CRITICAL(&udsMux);
                    memcpy(local_buf, active_uds.full_payload + 3, data_len);
                    portEXIT_CRITICAL(&udsMux);

                    if (parseBcmInputs0200(local_buf, data_len, out)) {
                        portENTER_CRITICAL(&udsMux);
                        latest_bcm_inputs = out;
                        portEXIT_CRITICAL(&udsMux);
                        parsed_ok = true;
                    }
                } else if (rx_did == HD_DID_BCM_LIVE_INPUTS && rx_dlc >= 5 && sf_data[1] == 0x62) {
                    uint8_t sf_len = (sf_data[0] & 0x0F);
                    if (sf_len > 3 && sf_len <= 7) {
                        if (parseBcmInputs0200(&sf_data[4], sf_len - 3, out)) {
                            portENTER_CRITICAL(&udsMux);
                            latest_bcm_inputs = out;
                            portEXIT_CRITICAL(&udsMux);
                            parsed_ok = true;
                        }
                    }
                }

                portENTER_CRITICAL(&udsMux);
                active_uds.pending     = false;
                active_uds.completed   = false;
                active_uds.isotp_multi = false;
                last_tester_present_ms = millis();
                portEXIT_CRITICAL(&udsMux);

                if (parsed_ok) {
                    diag_sequence_active = prev_seq;
                    return true;
                }
                break;
            }
            delay(4);
        }
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    return false;
}

bool UdsHandler::searchModulesSynchronous(uint32_t *outputModuleIds, size_t maxModuleCount, uint8_t *outputCount, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    if (outputModuleIds == nullptr || maxModuleCount == 0 || outputCount == nullptr) return false;
    *outputCount = 0;
    scan_count = 0;
    memset(scan_detected_ids, 0, sizeof(scan_detected_ids));
    scan_in_progress = true;

    CAN_TRACE("[UDS-SCAN] Broadcasting TesterPresent (02 3E 00) to 0x7DF -> Listening %u ms for HD ECUs...\n", timeoutMs);

    // Send single Tester Present command to ID 0x7DF (Broadcast):
    // 02 3E 00 00 00 00 00 00
    const uint8_t tp_frame[8] = {0x02, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (!CanDriver::transmit(0x7DF, false, false, 8, tp_frame)) {
        scan_in_progress = false;
        if (settings.canlog_enabled) Serial.println("[UDS-SCAN] ERROR: Failed to transmit 0x7DF broadcast frame on CAN bus.");
        return false;
    }

    // Wait the full timeoutMs (now 4000 ms) for all present ECU modules on the CAN bus to reply (some late).
    // Core 0's canDiagWorkerTask safely extracts all queued frames and appends them to scan_detected_ids
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        delay(20);
    }

    scan_in_progress = false;
    uint8_t copied = 0;
    for (uint8_t i = 0; i < scan_count && copied < maxModuleCount; i++) {
        outputModuleIds[copied++] = scan_detected_ids[i];
    }
    // Sort modules strictly in order of their ID address
    for (uint8_t i = 0; i < copied; i++) {
        for (uint8_t j = i + 1; j < copied; j++) {
            if (outputModuleIds[j] < outputModuleIds[i]) {
                uint32_t tmp = outputModuleIds[i];
                outputModuleIds[i] = outputModuleIds[j];
                outputModuleIds[j] = tmp;
            }
        }
    }
    *outputCount = copied;
    CAN_TRACE("[UDS-SCAN] Broadcast scan finished: %u ECU module(s) responded.\n", copied);
    return (*outputCount > 0);
}

bool UdsHandler::readDtcSynchronous(uint32_t targetModuleId, HDDtcRecord *outputDtcRecords, size_t maxDtcCount, uint8_t *outputCount, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    if (outputDtcRecords == nullptr || maxDtcCount == 0 || outputCount == nullptr) return false;
    *outputCount = 0;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    Serial.printf("[UDS-DTC] Reading DTCs (Service 0x19 02 09) from ECU 0x%03X...\n", targetModuleId);

    // Reliable read over the WiFi CAN bridge, same pattern as "Read ECU information" (readDid): a short
    // per-try wait (500 ms) with a brief settle between tries, retried several times on NO answer. A lost
    // request/frame is retried instead of reported as a fault; only a real negative response stops early.
    const uint32_t PER_TRY_MS = 500;
    uint8_t tries = (uint8_t)(timeoutMs / 560);
    if (tries < 5) tries = 5;
    if (tries > 8) tries = 8;

    for (uint8_t attempt = 0; attempt < tries; attempt++) {
        if (attempt) delay(60);                                   // let the bridge settle between tries
        if (!sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DTC, 0x02, 0x09)) continue;   // tx lost -> retry

        uint32_t start = millis();
        bool answered = false;
        while (millis() - start < PER_TRY_MS) {
            UdsPendingTransaction snap = getLatestResult();
            if (snap.completed) {
                if (snap.is_negative) {                           // module answered negatively -> do not retry
                    diag_sequence_active = prev_seq;
                    Serial.printf("[UDS-DTC] Negative response from ECU 0x%03X.\n", targetModuleId);
                    return false;
                }
                // 1. Multi-Frame ISO-TP response (full_payload): 0x59 0x02 <mask> then 4-byte DTC records
                if (snap.collected_len >= 3 && snap.full_payload[0] == 0x59 && snap.full_payload[1] == 0x02) {
                    for (size_t i = 3; i + 3 < snap.collected_len && *outputCount < maxDtcCount; i += 4) {
                        formatDtcCode(snap.full_payload[i], snap.full_payload[i + 1],
                                      snap.full_payload[i + 2], snap.full_payload[i + 3],
                                      outputDtcRecords[(*outputCount)++]);
                    }
                    diag_sequence_active = prev_seq;
                    Serial.printf("[UDS-DTC] Read %u DTC(s) via ISO-TP Multi-Frame from ECU 0x%03X (try %u).\n", *outputCount, targetModuleId, attempt + 1);
                    return true;
                }
                // 2. Single Frame response (rx_data)
                else if (snap.rx_data[1] == 0x59 && snap.rx_data[2] == 0x02) {
                    uint8_t sf_len = snap.rx_data[0] & 0x0F;
                    if (sf_len >= 6 && snap.rx_dlc >= 8) {
                        formatDtcCode(snap.rx_data[4], snap.rx_data[5], snap.rx_data[6], snap.rx_data[7],
                                      outputDtcRecords[(*outputCount)++]);
                    }
                    diag_sequence_active = prev_seq;
                    Serial.printf("[UDS-DTC] Read %u DTC(s) via Single Frame from ECU 0x%03X (try %u).\n", *outputCount, targetModuleId, attempt + 1);
                    return true;
                }
                answered = true;   // completed but unrecognized shape -> break and retry
                break;
            }
            delay(5);
        }
        (void)answered;
    }
    diag_sequence_active = prev_seq;
    Serial.printf("[UDS-DTC] TIMEOUT: no valid DTC response from ECU 0x%03X after %u tries.\n", targetModuleId, tries);
    return false;
}

bool UdsHandler::clearDtcSynchronous(uint32_t targetModuleId, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    Serial.printf("[UDS-CLEAR] Sending ClearDiagnosticInformation (04 14 FF FF FF) to ECU 0x%03X...\n", targetModuleId);

    // Send ClearDiagnosticInformation (Service 0x14): 04 14 FF FF FF 00 00 00
    if (!sendDiagnosticRequest(targetModuleId, UDS_SID_CLEAR_DTC, 0xFF, 0xFF)) {
        diag_sequence_active = prev_seq;
        return false;
    }

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        UdsPendingTransaction snap = getLatestResult();
        if (snap.completed) {
            diag_sequence_active = prev_seq;
            bool ok = !snap.is_negative;
            Serial.printf("[UDS-CLEAR] ECU 0x%03X ClearDTC result: %s\n", targetModuleId, ok ? "POSITIVE (0x54)" : "NEGATIVE (0x7F)");
            return ok;
        }
        delay(5);
    }
    diag_sequence_active = prev_seq;
    Serial.printf("[UDS-CLEAR] TIMEOUT waiting for ClearDTC (0x14) acknowledgment from ECU 0x%03X.\n", targetModuleId);
    return false;
}

// ============================================================================
// UDS Service 0x2F (InputOutputControlByIdentifier) Actuator Control Engine
// Supports ECM (0x7E0, 7 DIDs) and ABS (0x7E1, DID 0x0540 Hydraulic Pump Motor)
// ============================================================================
struct ActuatorRuntimeSlot {
    uint16_t    did;
    const char *name;
    bool        is_pulse;
    uint32_t    pulse_ms;
    bool        active;
    bool        timer_armed;
    uint32_t    timer_start_ms;
    uint32_t    targetModuleId;
};

static const size_t TOTAL_ACTUATOR_SLOTS = 8;

static ActuatorRuntimeSlot actuator_slots[TOTAL_ACTUATOR_SLOTS] = {
    { HD_DID_ACT_FUEL_PUMP,        "Fuel Pump Relay",          true,  2000, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_COOLING_FAN,      "Cooling Fan Relay",        false,    0, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_FRONT_COIL,       "Front Ignition Coil",      true,  1200, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_REAR_COIL,        "Rear Ignition Coil",       true,  1200, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_FRONT_INJECTOR,   "Front Injector Actuator",  true,  1200, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_REAR_INJECTOR,    "Rear Injector Actuator",   true,  1200, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_CHECK_ENGINE_MIL, "Check Engine Light / MIL", false,    0, false, false, 0, HD_ECU_ECM },
    { HD_DID_ACT_ABS_PUMP_MOTOR,   "ABS Hydraulic Pump Motor", true,  1000, false, false, 0, HD_ECU_ABS }
};

static const char* describeUdsNrc(uint8_t nrc) {
    switch (nrc) {
        case 0x10: return "GeneralReject (0x10)";
        case 0x11: return "ServiceNotSupported (0x11)";
        case 0x12: return "SubFunctionNotSupported (0x12)";
        case 0x13: return "IncorrectMessageLengthOrInvalidFormat (0x13)";
        case 0x21: return "BusyRepeatRequest (0x21)";
        case 0x22: return "Conditions Not Correct (0x22)";
        case 0x24: return "RequestSequenceError (0x24)";
        case 0x31: return "RequestOutOfRange (0x31)";
        case 0x33: return "SecurityAccessDenied (0x33)";
        case 0x72: return "GeneralProgrammingFailure (0x72)";
        case 0x78: return "RequestCorrectlyReceived-ResponsePending (0x78)";
        case 0x7E: return "SubFunctionNotSupportedInActiveSession (0x7E)";
        case 0x7F: return "ServiceNotSupportedInActiveSession (0x7F)";
        default:   return "ECU Negative Response";
    }
}

static ActuatorRuntimeSlot* findActuatorSlot(uint16_t did) {
    for (size_t i = 0; i < TOTAL_ACTUATOR_SLOTS; i++) {
        if (actuator_slots[i].did == did) {
            return &actuator_slots[i];
        }
    }
    return nullptr;
}

bool UdsHandler::readBatteryVoltage0200(uint32_t targetModuleId, float &outputVoltage, float &outputRpm, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    if (readEcmInputs0200Synchronous(targetModuleId, s_batt_ecm_work, timeoutMs)) {
        outputVoltage = s_batt_ecm_work.battery_voltage;
        outputRpm     = s_batt_ecm_work.engine_speed;
        return true;
    }
    return false;
}

bool UdsHandler::isAnyActuatorActive() {
    for (size_t i = 0; i < TOTAL_ACTUATOR_SLOTS; i++) {
        if (actuator_slots[i].active || actuator_slots[i].timer_armed) {
            return true;
        }
    }
    return false;
}

bool UdsHandler::enterExtendedDiagnosticSession(uint32_t targetModuleId, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    if (targetModuleId == 0) targetModuleId = HD_ECU_ABS;
    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    Serial.printf("[UDS-0x10] Transitioning ECU 0x%03X into Extended Diagnostic Session [02 10 03 00 00 00 00 00]\n", targetModuleId);

    if (!sendDiagnosticRequest(targetModuleId, UDS_SID_DIAG_SESSION_CTRL, 0x03, 0x00, "02 10 03 00 00 00 00 00")) {
        diag_sequence_active = prev_seq;
        return false;
    }

    uint32_t wait_start = millis();
    while (millis() - wait_start < timeoutMs) {
        UdsPendingTransaction snap = getLatestResult();
        if (snap.completed) {
            diag_sequence_active = prev_seq;
            bool ok = !snap.is_negative && (snap.rx_dlc >= 2 && snap.rx_data[1] == 0x50);
            Serial.printf("[UDS-0x10] ECU 0x%03X Extended Session (0x10 0x03) response: %s\n",
                          targetModuleId, ok ? "POSITIVE (0x50)" : (snap.is_negative ? "NEGATIVE (0x7F)" : "COMPLETED"));
            return ok;
        }
        delay(4);
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    return false;
}

void UdsHandler::setAbsOutputsPageActive(bool active) {
    if (active) {
        abs_outputs_page_active = true;
        last_abs_tp_80_ms       = millis();
        last_tester_present_ms  = millis();
        // Transition ABS ECU (0x7E1) into Extended Diagnostic Session (02 10 03 00 00 00 00 00)
        enterExtendedDiagnosticSession(HD_ECU_ABS, 350);
        // Send initial TesterPresent with suppressPosRspMsgIndicationBit (02 3E 80 00 00 00 00 00) to 0x7E1
        const uint8_t abs_tp_frame[8] = {0x02, 0x3E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
        CanDriver::transmit(HD_ECU_ABS, false, false, 8, abs_tp_frame);
        Serial.println("[UDS-ABS] ABS Outputs page active -> Extended Session (02 10 03) + 2000ms TesterPresent (02 3E 80) armed on 0x7E1.");
    } else {
        if (abs_outputs_page_active) {
            Serial.println("[UDS-ABS] Stopping 2000ms ABS TesterPresent (02 3E 80) heartbeat on 0x7E1.");
        }
        abs_outputs_page_active = false;
        last_abs_tp_80_ms       = 0;
    }
}

bool UdsHandler::isAbsOutputsPageActive() {
    return abs_outputs_page_active;
}

bool UdsHandler::controlAbsActuatorOutput(uint16_t did, uint8_t state, ActuatorOutputResult &outputResult, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&outputResult, 0, sizeof(ActuatorOutputResult));
    outputResult.did             = did;
    outputResult.requested_state = state;

    const uint32_t targetModuleId = HD_ECU_ABS; // 0x7E1
    ActuatorRuntimeSlot *slot = findActuatorSlot(did);
    const char *actName = slot ? slot->name : getHDDidName(did);

    // 1. Wheel Speed Safety Lock (km/h > 0):
    //    Refuse execution of the ABS Hydraulic Pump actuator test if the vehicle is moving (km/h > 0).
    AbsInputs0200 absSnap = getLatestAbsInputs0200();
    bool ecmValid = false;
    float ecmVehSpeed = 0.0f;
    portENTER_CRITICAL(&udsMux);
    ecmValid    = latest_ecm_inputs.valid;
    ecmVehSpeed = latest_ecm_inputs.vehicle_speed;
    portEXIT_CRITICAL(&udsMux);

    float maxWheelKmh = 0.0f;
    if (absSnap.valid) {
        if (absSnap.front_wheel_speed > maxWheelKmh) maxWheelKmh = absSnap.front_wheel_speed;
        if (absSnap.rear_wheel_speed > maxWheelKmh)  maxWheelKmh = absSnap.rear_wheel_speed;
        if (absSnap.rear_right_wheel_speed > maxWheelKmh) maxWheelKmh = absSnap.rear_right_wheel_speed;
        if (absSnap.vehicle_speed > maxWheelKmh)     maxWheelKmh = absSnap.vehicle_speed;
    }
    if (ecmValid && ecmVehSpeed > maxWheelKmh) {
        maxWheelKmh = ecmVehSpeed;
    }

    if (state == 1 && maxWheelKmh > 0.0f) {
        outputResult.tx_ok           = false;
        outputResult.got_reply       = true;
        outputResult.is_negative     = true;
        outputResult.speed_interlock = true;
        outputResult.nrc             = 0x22;
        snprintf(outputResult.message, sizeof(outputResult.message),
                 "Disabled: Vehicle is Moving (Wheel Speed %.1f km/h > 0 km/h).", maxWheelKmh);
        Serial.printf("[UDS-ABS-0x2F] WHEEL SPEED SAFETY LOCK BLOCKED 0x%04X (%s): Speed=%.2f km/h > 0!\n",
                      did, actName, maxWheelKmh);
        return false;
    }

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // 2. Session Switch Requirement:
    //    Before executing any ABS actuator test from the UI, transition the ABS ECU (0x7E1)
    //    into an Extended Session by sending: 02 10 03 00 00 00 00 00 to CAN ID 0x7E1,
    //    and ensure the 2000ms 02 3E 80 TesterPresent heartbeat is active.
    abs_outputs_page_active = true;
    last_abs_tp_80_ms       = millis();
    enterExtendedDiagnosticSession(targetModuleId, 300);
    delay(15);

    const uint8_t did_hi = (uint8_t)((did >> 8) & 0xFF);
    const uint8_t did_lo = (uint8_t)(did & 0xFF);

    // 3. Safe Actuator Command Map (Target CAN ID: 0x7E1):
    //    - ABS Hydraulic Pump Motor (DID 0x0540):
    //      * Send ON (1000ms pulse): 06 2F 05 40 03 20 20 00
    //      * Return to ECU:          04 2F 05 40 00 00 00 00
    bool isReturnToEcu = (state == 0 || state == 2);
    if (isReturnToEcu) {
        snprintf(outputResult.tx_frame_hex, sizeof(outputResult.tx_frame_hex),
                 "04 2F %02X %02X 00 00 00 00", did_hi, did_lo);
    } else {
        snprintf(outputResult.tx_frame_hex, sizeof(outputResult.tx_frame_hex),
                 "06 2F %02X %02X 03 20 20 00", did_hi, did_lo);
    }

    Serial.printf("[UDS-ABS-0x2F] TX to 0x%03X -> %s [%s] (state=%u)\n",
                  targetModuleId, actName, outputResult.tx_frame_hex, state);

    if (!sendDiagnosticRequest(targetModuleId, UDS_SID_IO_CTRL_BY_ID, did_hi, did_lo, outputResult.tx_frame_hex)) {
        diag_sequence_active = prev_seq;
        snprintf(outputResult.message, sizeof(outputResult.message),
                 "CAN bus transmission error sending 0x2F command to ABS (0x7E1) for %s.", actName);
        return false;
    }
    outputResult.tx_ok = true;

    // 4. Wait for ABS ECU Positive Response (0x6F from 0x7E9) or Negative Response (03 7F 2F [NRC])
    uint32_t wait_start = millis();
    while (millis() - wait_start < timeoutMs) {
        UdsPendingTransaction snap = getLatestResult();
        if (snap.completed) {
            outputResult.got_reply = true;

            char *p = outputResult.rx_frame_hex;
            for (uint8_t i = 0; i < snap.rx_dlc && i < 8; i++) {
                p += sprintf(p, i == 0 ? "%02X" : " %02X", snap.rx_data[i]);
            }

            // Intercept Negative Response Frame: 03 7F 2F [NRC]
            if (snap.is_negative || (snap.rx_dlc >= 4 && snap.rx_data[1] == 0x7F && snap.rx_data[2] == 0x2F)) {
                outputResult.is_negative = true;
                outputResult.nrc         = (snap.rx_dlc >= 4 && snap.rx_data[1] == 0x7F) ? snap.rx_data[3] : 0x10;
                if (outputResult.nrc == 0x22) {
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "ABS Rejected %s (03 7F 2F 22): Conditions Not Correct", actName);
                } else {
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "ABS Rejected %s (03 7F 2F %02X): %s", actName, outputResult.nrc, describeUdsNrc(outputResult.nrc));
                }
                Serial.printf("[UDS-ABS-0x2F] NRC 0x%02X from 0x%03X for %s: %s\n",
                              outputResult.nrc, snap.rx_id, actName, outputResult.message);
                if (slot) {
                    slot->active      = false;
                    slot->timer_armed = false;
                }
                diag_sequence_active = prev_seq;
                return false;
            }

            // Positive Response (0x6F = 0x2F + 0x40)
            outputResult.is_positive = true;
            if (slot) {
                slot->targetModuleId = targetModuleId;
                if (isReturnToEcu) {
                    slot->active             = false;
                    slot->timer_armed        = false;
                    outputResult.current_state = 0;
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "%s returned to ABS ECU control [04 2F %02X %02X 00 00 00 00].", actName, did_hi, did_lo);
                } else {
                    // Non-blocking 1000ms (millis()) pulse -> automatically sends 04 2F 05 40 00 00 00 00 after 1000ms
                    slot->active                 = true;
                    slot->timer_armed            = true;
                    slot->timer_start_ms         = millis();
                    outputResult.current_state     = 2; // PULSING
                    outputResult.pulse_duration_ms = slot->pulse_ms;
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "%s pulsing (1000 ms) [06 2F %02X %02X 03 20 20 00] -> Auto-reset armed.", actName, did_hi, did_lo);
                }
            } else {
                outputResult.current_state     = isReturnToEcu ? 0 : 2;
                outputResult.pulse_duration_ms = ACTUATOR_ABS_PUMP_PULSE_MS;
                snprintf(outputResult.message, sizeof(outputResult.message),
                         "ABS Actuator 0x%04X command acknowledged (0x6F).", did);
            }

            diag_sequence_active = prev_seq;
            return true;
        }
        delay(4);
    }

    // Timeout waiting for ABS ECU reply (0x7E9)
    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    snprintf(outputResult.message, sizeof(outputResult.message),
             "ABS Module Communication Timeout");
    Serial.printf("[UDS-ABS-0x2F] TIMEOUT waiting for 0x6F response from ABS 0x%03X for %s (0x%04X)\n",
                  targetModuleId, actName, did);
    return false;
}

bool UdsHandler::controlActuatorOutput(uint32_t targetModuleId, uint16_t did, uint8_t state, ActuatorOutputResult &outputResult, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    // Route ABS actuator DID 0x0540 or target 0x7E1 directly through the dedicated ABS 0x2F handler
    if (targetModuleId == HD_ECU_ABS || did == HD_DID_ACT_ABS_PUMP_MOTOR) {
        return controlAbsActuatorOutput(did, state, outputResult, timeoutMs);
    }

    memset(&outputResult, 0, sizeof(ActuatorOutputResult));
    outputResult.did             = did;
    outputResult.requested_state = state;

    ActuatorRuntimeSlot *slot = findActuatorSlot(did);
    const char *actName = slot ? slot->name : getHDDidName(did);

    // 1. Engine Safety Interlock (RPM > 0 Check):
    //    Prevent energizing actuators if the engine is currently running.
    bool ecmValid = false;
    float ecmRpm  = 0.0f;
    portENTER_CRITICAL(&udsMux);
    ecmValid = latest_ecm_inputs.valid;
    ecmRpm   = latest_ecm_inputs.engine_speed;
    portEXIT_CRITICAL(&udsMux);

    if (state == 1 && ecmValid && ecmRpm > 50.0f) {
        outputResult.tx_ok         = false;
        outputResult.got_reply     = true;
        outputResult.is_negative   = true;
        outputResult.rpm_interlock = true;
        outputResult.nrc           = 0x22;
        snprintf(outputResult.message, sizeof(outputResult.message),
                 "Engine Safety Interlock: Cannot activate %s while Engine Speed is %.0f RPM (must be 0 RPM).",
                 actName, ecmRpm);
        Serial.printf("[UDS-0x2F] SAFETY INTERLOCK BLOCKED 0x%04X (%s): RPM=%.0f > 0!\n",
                      did, actName, ecmRpm);
        return false;
    }

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    const uint8_t did_hi = (uint8_t)((did >> 8) & 0xFF);
    const uint8_t did_lo = (uint8_t)(did & 0xFF);

    // 2. Construct exact 8-byte UDS 0x2F CAN command frame:
    //    - Return Control to ECU (state == 2, or state == 0 on pulse actuators):
    //      04 2F <DID_H> <DID_L> 00 00 00 00
    //    - ShortTermAdjustment ON / Pulse Fire (state == 1):
    //      06 2F <DID_H> <DID_L> 03 20 20 00
    //    - ShortTermAdjustment OFF for Toggle actuators (state == 0 on 0x0501 or 0x0530):
    //      06 2F <DID_H> <DID_L> 03 20 00 00
    bool isReturnToEcu = (state == 2) || (state == 0 && slot != nullptr && slot->is_pulse);
    if (isReturnToEcu) {
        snprintf(outputResult.tx_frame_hex, sizeof(outputResult.tx_frame_hex),
                 "04 2F %02X %02X 00 00 00 00", did_hi, did_lo);
    } else if (state == 1) {
        snprintf(outputResult.tx_frame_hex, sizeof(outputResult.tx_frame_hex),
                 "06 2F %02X %02X 03 20 20 00", did_hi, did_lo);
    } else {
        snprintf(outputResult.tx_frame_hex, sizeof(outputResult.tx_frame_hex),
                 "06 2F %02X %02X 03 20 00 00", did_hi, did_lo);
    }

    Serial.printf("[UDS-0x2F] TX to 0x%03X -> %s [%s] (state=%u)\n",
                  targetModuleId, actName, outputResult.tx_frame_hex, state);

    if (!sendDiagnosticRequest(targetModuleId, UDS_SID_IO_CTRL_BY_ID, did_hi, did_lo, outputResult.tx_frame_hex)) {
        diag_sequence_active = prev_seq;
        snprintf(outputResult.message, sizeof(outputResult.message),
                 "CAN bus transmission error sending 0x2F command for %s.", actName);
        return false;
    }
    outputResult.tx_ok = true;

    // 3. Wait for ECU Positive Response (0x6F) or Negative Response (0x7F + NRC)
    uint32_t wait_start = millis();
    while (millis() - wait_start < timeoutMs) {
        UdsPendingTransaction snap = getLatestResult();
        if (snap.completed) {
            outputResult.got_reply = true;

            char *p = outputResult.rx_frame_hex;
            for (uint8_t i = 0; i < snap.rx_dlc && i < 8; i++) {
                p += sprintf(p, i == 0 ? "%02X" : " %02X", snap.rx_data[i]);
            }

            if (snap.is_negative) {
                outputResult.is_negative = true;
                outputResult.nrc         = (snap.rx_dlc >= 4 && snap.rx_data[1] == 0x7F) ? snap.rx_data[3] : 0x10;
                snprintf(outputResult.message, sizeof(outputResult.message),
                         "ECU Rejected %s: %s", actName, describeUdsNrc(outputResult.nrc));
                Serial.printf("[UDS-0x2F] NRC 0x%02X from 0x%03X for %s: %s\n",
                              outputResult.nrc, snap.rx_id, actName, outputResult.message);
                if (slot) {
                    slot->active      = false;
                    slot->timer_armed = false;
                }
                diag_sequence_active = prev_seq;
                return false;
            }

            // Positive Response (0x6F = 0x2F + 0x40)
            outputResult.is_positive = true;
            if (slot) {
                slot->targetModuleId = targetModuleId;
                if (isReturnToEcu || state == 0) {
                    slot->active            = false;
                    slot->timer_armed       = false;
                    outputResult.current_state = 0;
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "%s turned OFF (0x6F confirmed).", actName);
                } else if (slot->is_pulse) {
                    slot->active                 = true;
                    slot->timer_armed            = true;
                    slot->timer_start_ms         = millis();
                    outputResult.current_state     = 2; // PULSING
                    outputResult.pulse_duration_ms = slot->pulse_ms;
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "%s pulsing (%u ms) -> Auto-reset armed.", actName, slot->pulse_ms);
                } else {
                    slot->active            = true;
                    slot->timer_armed       = false;
                    outputResult.current_state = 1; // ON
                    snprintf(outputResult.message, sizeof(outputResult.message),
                             "%s toggled ON (0x6F confirmed).", actName);
                }
            } else {
                outputResult.current_state = (state == 1) ? 1 : 0;
                snprintf(outputResult.message, sizeof(outputResult.message),
                         "Actuator 0x%04X command acknowledged (0x6F).", did);
            }

            diag_sequence_active = prev_seq;
            return true;
        }
        delay(4);
    }

    // Timeout waiting for ECU reply
    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    portEXIT_CRITICAL(&udsMux);

    diag_sequence_active = prev_seq;
    snprintf(outputResult.message, sizeof(outputResult.message),
             "Communication Timeout - No Response from ECU (0x%03X) for %s.", targetModuleId, actName);
    Serial.printf("[UDS-0x2F] TIMEOUT waiting for 0x6F response from 0x%03X for %s (0x%04X)\n",
                  targetModuleId, actName, did);
    return false;
}

bool UdsHandler::serviceActuatorTimers(uint16_t &outputResetDid, uint32_t &outputTargetModuleId, char *outputTxHexBuffer, size_t maxTxHexLength) {
    UdsBusLock bus_lock_(0);          // never block the web loop: retry on the next iteration
    if (!bus_lock_.ok) return false;
    uint32_t now = millis();
    for (size_t i = 0; i < TOTAL_ACTUATOR_SLOTS; i++) {
        ActuatorRuntimeSlot &slot = actuator_slots[i];
        if (slot.timer_armed && (now - slot.timer_start_ms >= slot.pulse_ms)) {
            slot.timer_armed = false;
            slot.active      = false;
            outputResetDid    = slot.did;

            const uint8_t did_hi = (uint8_t)((slot.did >> 8) & 0xFF);
            const uint8_t did_lo = (uint8_t)(slot.did & 0xFF);
            const uint8_t reset_frame[8] = {0x04, UDS_SID_IO_CTRL_BY_ID, did_hi, did_lo, 0x00, 0x00, 0x00, 0x00};

            if (outputTxHexBuffer != nullptr && maxTxHexLength > 0) {
                snprintf(outputTxHexBuffer, maxTxHexLength, "04 2F %02X %02X 00 00 00 00", did_hi, did_lo);
            }

            uint32_t targetModuleId = (slot.targetModuleId != 0) ? slot.targetModuleId : ((slot.did == HD_DID_ACT_ABS_PUMP_MOTOR) ? HD_ECU_ABS : HD_ECU_ECM);
            outputTargetModuleId = targetModuleId;
            CanDriver::transmit(targetModuleId, false, false, 8, reset_frame);
            last_tester_present_ms = now;
            if (targetModuleId == HD_ECU_ABS) {
                last_abs_tp_80_ms = now;
            }

            Serial.printf("[UDS-0x2F] Async pulse timer (%u ms) expired for %s (0x%04X on 0x%03X) -> Sent Reset to ECU [04 2F %02X %02X 00 00 00 00]\n",
                          slot.pulse_ms, slot.name, slot.did, targetModuleId, did_hi, did_lo);
            return true;
        }
    }
    return false;
}

void UdsHandler::cleanupAbsActuatorOutputs() {
    UdsBusLock bus_lock_;
    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // 1. Stop the periodic 2000ms 02 3E 80 00 00 00 00 00 TesterPresent heartbeat immediately
    abs_outputs_page_active = false;
    last_abs_tp_80_ms       = 0;

    // 2. Disarm ABS Hydraulic Pump Motor (0x0540) runtime slot and send UDS Reset frame:
    //    04 2F 05 40 00 00 00 00 to 0x7E1
    ActuatorRuntimeSlot *absSlot = findActuatorSlot(HD_DID_ACT_ABS_PUMP_MOTOR);
    if (absSlot) {
        absSlot->active      = false;
        absSlot->timer_armed = false;
    }

    const uint8_t abs_reset_frame[8] = {0x04, UDS_SID_IO_CTRL_BY_ID, 0x05, 0x40, 0x00, 0x00, 0x00, 0x00};
    CanDriver::transmit(HD_ECU_ABS, false, false, 8, abs_reset_frame);
    delay(15);

    // 3. Send Default Session drop: 02 10 01 00 00 00 00 00 to 0x7E1
    const uint8_t default_session_frame[8] = {0x02, UDS_SID_DIAG_SESSION_CTRL, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CanDriver::transmit(HD_ECU_ABS, false, false, 8, default_session_frame);
    last_tester_present_ms = millis();

    diag_sequence_active = prev_seq;
    Serial.println("[UDS-ABS-CLEANUP] Sent ABS Pump Reset [04 2F 05 40 00 00 00 00], Default Session [02 10 01 00 00 00 00 00] to 0x7E1, and stopped 02 3E 80 TesterPresent.");
}

void UdsHandler::cleanupAllActuatorOutputs(uint32_t targetModuleId) {
    UdsBusLock bus_lock_;
    if (targetModuleId == HD_ECU_ABS) {
        cleanupAbsActuatorOutputs();
        return;
    }
    if (targetModuleId == 0) targetModuleId = HD_ECU_ECM;
    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    Serial.printf("[UDS-CLEANUP] Returning control to ECU (0x%03X) for all 7 ECM output actuators + Session Reset (02 10 01)...\n", targetModuleId);

    // 1. Transmit Return Control to ECU (04 2F <DID_H> <DID_L> 00 00 00 00) for all 7 ECM DIDs:
    //    0x0502, 0x0501, 0x0505, 0x0506, 0x0503, 0x0504, 0x0530
    for (size_t i = 0; i < 7; i++) {
        actuator_slots[i].active      = false;
        actuator_slots[i].timer_armed = false;

        const uint8_t did_hi = (uint8_t)((actuator_slots[i].did >> 8) & 0xFF);
        const uint8_t did_lo = (uint8_t)(actuator_slots[i].did & 0xFF);
        const uint8_t return_frame[8] = {0x04, UDS_SID_IO_CTRL_BY_ID, did_hi, did_lo, 0x00, 0x00, 0x00, 0x00};

        CanDriver::transmit(targetModuleId, false, false, 8, return_frame);
        delay(12);
    }

    // 2. Transmit UDS Session Reset frame (02 10 01 00 00 00 00 00) to drop back into Default Session
    const uint8_t default_session_frame[8] = {0x02, UDS_SID_DIAG_SESSION_CTRL, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CanDriver::transmit(targetModuleId, false, false, 8, default_session_frame);
    last_tester_present_ms = millis();

    diag_sequence_active = prev_seq;
    Serial.println("[UDS-CLEANUP] All 7 ECM actuator DIDs returned to ECU (0x00) and Default Session (02 10 01) restored.");
}


// ============================================================================
// Generic DID 0x0200 (SCM, LHC, RHC, EHCU, SPEEDO) - table driven from config.h 5D
// ============================================================================
bool UdsHandler::parseGenInputs0200(uint32_t moduleId, const uint8_t *data, size_t len, GenInputs0200 &out) {
    memset(&out, 0, sizeof(out));
    const GenModuleDef* m = findGenModule(moduleId);
    if (!m || data == nullptr || len < 1) return false;
    uint8_t li = (gen_layout_latched && gen_layout_module == moduleId) ? gen_layout_current : m->default_layout;
    if (li >= m->layout_count) li = m->default_layout;
    const GenLayout &lay = m->layouts[li];
    out.module_id = (uint16_t)moduleId;
    out.layout    = li;
    out.update_ms = millis();
    for (uint8_t i = 0; i < lay.count && out.count < GEN_MAX_ITEMS; i++) {
        const GenItem &it = lay.items[i];
        if ((size_t)it.pos + it.nbytes > len) continue;      // beyond received payload -> not decoded
        float v;
        if (it.kind == GEN_FLAG) {
            v = (data[it.pos] & it.mask) ? 1.0f : 0.0f;
        } else {
            uint32_t raw = 0;
            for (uint8_t b = 0; b < it.nbytes; b++) raw = (raw << 8) | data[it.pos + b];
            v = (float)raw * it.scale;
        }
        out.key[out.count]   = it.key;
        out.kind[out.count]  = it.kind;
        out.value[out.count] = v;
        out.count++;
    }
    out.valid = out.count > 0;
    return out.valid;
}

bool UdsHandler::readGenInputs0200Synchronous(uint32_t targetModuleId, GenInputs0200 &out, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&out, 0, sizeof(out));
    if (!findGenModule(targetModuleId)) return false;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    // Layout from the module's Calibration ID (once per module selection; failed probes retried every 3 s).
    static uint32_t last_probe_ms = 0;
    if ((!gen_layout_latched || gen_layout_module != targetModuleId) &&
        (last_probe_ms == 0 || (millis() - last_probe_ms) > 3000)) {
        last_probe_ms = millis();
        char cal[48] = {0};
        if (readFamilyIdString(targetModuleId, cal, sizeof(cal), 350)) {
            identifyGenLayout(targetModuleId, cal);
        }
        delay(25);
    }

    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) delay(25);
        if (!sendDiagnosticRequest(targetModuleId, UDS_SID_READ_DID, 0x02, 0x00, "03 22 02 00 00 00 00 00")) continue;

        uint32_t wait_start = millis();
        while (millis() - wait_start < timeoutMs) {
            bool done = false, is_neg = false;
            uint16_t rx_did = 0, exp_len = 0;
            uint8_t rx_dlc = 0, sf_data[8] = {0};
            uint8_t buf[96] = {0};

            portENTER_CRITICAL(&udsMux);
            if (active_uds.completed) {
                done    = true;
                is_neg  = active_uds.is_negative;
                exp_len = active_uds.expected_len;
                rx_dlc  = active_uds.rx_dlc;
                memcpy(sf_data, active_uds.rx_data, 8);
                if (exp_len >= 3 && active_uds.full_payload[0] == 0x62) {
                    rx_did = ((uint16_t)active_uds.full_payload[1] << 8) | active_uds.full_payload[2];
                    uint16_t n = exp_len - 3;
                    if (n > sizeof(buf)) n = sizeof(buf);
                    memcpy(buf, active_uds.full_payload + 3, n);
                } else if (rx_dlc >= 4 && sf_data[1] == 0x62) {
                    rx_did = ((uint16_t)sf_data[2] << 8) | sf_data[3];
                }
            }
            portEXIT_CRITICAL(&udsMux);

            if (done) {
                bool ok = false;
                if (!is_neg && rx_did == 0x0200) {
                    if (exp_len > 3) {
                        uint16_t n = exp_len - 3;
                        if (n > sizeof(buf)) n = sizeof(buf);
                        ok = parseGenInputs0200(targetModuleId, buf, n, out);
                    } else if (rx_dlc >= 5 && sf_data[1] == 0x62) {
                        uint8_t sf_len = (sf_data[0] & 0x0F);
                        if (sf_len > 3 && sf_len <= 7) ok = parseGenInputs0200(targetModuleId, &sf_data[4], sf_len - 3, out);
                    }
                }
                portENTER_CRITICAL(&udsMux);
                active_uds.pending     = false;
                active_uds.completed   = false;
                active_uds.isotp_multi = false;
                last_tester_present_ms = millis();
                portEXIT_CRITICAL(&udsMux);
                if (ok) { diag_sequence_active = prev_seq; return true; }
                break;
            }
            delay(4);
        }
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);
    diag_sequence_active = prev_seq;
    return false;
}


// ============================================================================
// Generic single-frame UDS transaction (used by the ECM output-test engine)
// ============================================================================
bool UdsHandler::rawTransaction(uint32_t targetModuleId, const uint8_t *req, uint8_t requestLength, UdsRawResult &res, uint32_t timeoutMs) {
    UdsBusLock bus_lock_;
    memset(&res, 0, sizeof(res));
    if (req == nullptr || requestLength < 1 || requestLength > 60) return false;

    bool prev_seq = diag_sequence_active;
    diag_sequence_active = true;

    char hex[40];
    size_t n = 0;
    if (requestLength <= 7) {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02X", requestLength);
        for (uint8_t i = 0; i < 7; i++) n += (size_t)snprintf(hex + n, sizeof(hex) - n, " %02X", i < requestLength ? req[i] : 0x00);
    } else {
        // ISO-TP First Frame: 1L LL + first 6 data bytes
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02X %02X", 0x10 | ((requestLength >> 8) & 0x0F), requestLength & 0xFF);
        for (uint8_t i = 0; i < 6; i++) n += (size_t)snprintf(hex + n, sizeof(hex) - n, " %02X", req[i]);
        s_txfc_got  = false;
        s_txfc_wait = true;
    }

    bool sent = sendDiagnosticRequest(targetModuleId, req[0], 0, 0, hex);
    if (!sent) {
        s_txfc_wait = false;
        diag_sequence_active = prev_seq;
        return false;
    }

    if (requestLength > 7) {
        // sendDiagnosticRequest() derived the expected response SID from the First Frame header: fix it
        portENTER_CRITICAL(&udsMux);
        active_uds.sid        = req[0];
        active_uds.target_did = 0;
        portEXIT_CRITICAL(&udsMux);

        uint8_t idx = 6, sn = 1, block_sent = 0, bs = 0, st = 0;
        bool ok = true;
        bool need_fc = true;
        while (ok && idx < requestLength) {
            if (need_fc) {
                uint32_t t0 = millis();
                while (!s_txfc_got && millis() - t0 < 350) delay(1);
                if (!s_txfc_got) { ok = false; break; }
                uint8_t fs = s_txfc_fs;
                bs = s_txfc_bs; st = s_txfc_st;
                s_txfc_got = false;
                if (fs == 1) { continue; }          // WAIT: wait for the next FC
                if (fs != 0) { ok = false; break; } // overflow / abort
                need_fc = false;
                block_sent = 0;
            }
            uint8_t frame[8] = {0};
            frame[0] = (uint8_t)(0x20 | (sn & 0x0F));
            uint8_t chunk = (uint8_t)((requestLength - idx) > 7 ? 7 : (requestLength - idx));
            memcpy(frame + 1, req + idx, chunk);
            CanDriver::transmit(targetModuleId, false, false, 8, frame);
            idx += chunk; sn = (sn + 1) & 0x0F; block_sent++;
            if (idx < requestLength) {
                if (st >= 1 && st <= 0x7F) delay(st); else delay(1);
                if (bs != 0 && block_sent >= bs) need_fc = true;
            }
        }
        s_txfc_wait = false;
        if (!ok) {
            portENTER_CRITICAL(&udsMux);
            active_uds.pending = false; active_uds.completed = false; active_uds.isotp_multi = false;
            portEXIT_CRITICAL(&udsMux);
            diag_sequence_active = prev_seq;
            return false;
        }
        portENTER_CRITICAL(&udsMux);
        active_uds.start_ms = millis();
        portEXIT_CRITICAL(&udsMux);
    }
    res.tx_ok = true;

    const uint32_t wait_start = millis();
    uint32_t effective_timeout_ms = timeoutMs;
    uint32_t last_start_ms;
    { portENTER_CRITICAL(&udsMux); last_start_ms = active_uds.start_ms; portEXIT_CRITICAL(&udsMux); }
    // Hard ceiling even across repeated NRC 0x78 (RequestCorrectlyReceived-ResponsePending) extensions below,
    // so a module that never stops saying "still working" cannot hang the caller forever.
    const uint32_t RAW_TRANSACTION_HARD_CAP_MS = 5000;
    while (millis() - wait_start < effective_timeout_ms && millis() - wait_start < RAW_TRANSACTION_HARD_CAP_MS) {
        bool done = false, neg = false;
        uint16_t exp_len = 0;
        uint8_t  buf[sizeof(res.data)] = {0};
        uint32_t cur_start_ms;
        portENTER_CRITICAL(&udsMux);
        cur_start_ms = active_uds.start_ms;
        if (active_uds.completed) {
            done    = true;
            neg     = active_uds.is_negative;
            exp_len = active_uds.expected_len;
            uint16_t n2 = exp_len > sizeof(buf) ? (uint16_t)sizeof(buf) : exp_len;
            memcpy(buf, active_uds.full_payload, n2);
            exp_len = n2;
            active_uds.pending     = false;
            active_uds.completed   = false;
            active_uds.isotp_multi = false;
            last_tester_present_ms = millis();
        }
        portEXIT_CRITICAL(&udsMux);

        // inspectFrame() resets active_uds.start_ms (and swallows the frame, without completing the
        // transaction) whenever it sees NRC 0x78 "RequestCorrectlyReceived-ResponsePending" for our SID -
        // see its comment above ("Extending P2* timer"). Previously that reset only affected an internal
        // timer nothing here looked at: this loop still gave up at the caller's ORIGINAL timeoutMs (callers
        // like cfg_special.cpp's writeBlock() pass ~450-900 ms), so a module that legitimately answers
        // "still working" before finishing a write (e.g. a flash/EEPROM write that takes longer) made this
        // function report "no response" and the caller then moved on (even closing the session) - while the
        // module's real, positive final response arrived moments later, after the fact. Detect that reset
        // here and give the transaction a fresh full timeoutMs window measured from the moment of the 0x78,
        // instead of the one fixed budget set before any frame had even been sent.
        if (cur_start_ms != last_start_ms) {
            last_start_ms = cur_start_ms;
            uint32_t elapsed_at_reset = (uint32_t)(cur_start_ms - wait_start);
            uint32_t extended = elapsed_at_reset + timeoutMs;
            if (extended > effective_timeout_ms) effective_timeout_ms = extended;
        }

        if (done) {
            res.got_reply = true;
            res.len = (uint8_t)exp_len;
            memcpy(res.data, buf, exp_len);
            if (exp_len >= 3 && buf[0] == 0x7F) res.nrc = buf[2];
            res.positive = !neg && exp_len >= 1 && buf[0] == (uint8_t)(req[0] + 0x40);
            diag_sequence_active = prev_seq;
            return true;
        }
        delay(3);
    }

    portENTER_CRITICAL(&udsMux);
    active_uds.pending     = false;
    active_uds.completed   = false;
    active_uds.isotp_multi = false;
    last_tester_present_ms = millis();
    portEXIT_CRITICAL(&udsMux);
    diag_sequence_active = prev_seq;
    return false;
}
