// Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
#pragma once
// =============================================================================
// ota_helper.h - Shared, validated OTA upload state machine
// Used by the main web server (/api/ota/upload) and Safe Boot (/update).
//
// Writes the uploaded main application EXPLICITLY to the ota_0 partition using
// the ESP-IDF esp_ota_* API (esp_ota_begin / esp_ota_write / esp_ota_end),
// rather than the Arduino Update library's automatic "next update partition"
// selection. When Safe Boot runs from the factory partition, the Arduino
// auto-selection proved unreliable (begin never erased ota_0, so the stream was
// silently discarded and the old firmware stayed in place). Targeting the ota_0
// handle directly makes the write deterministic regardless of which partition
// we are currently booted from.
//
// Checks: ota_0 exists, we are not running from ota_0 (no self-overwrite),
// firmware magic byte (0xE9), esp_ota_begin/write/end results, and sets the
// boot partition to ota_0 on success. Aborts cleanly on any failure and
// exposes the result + a human-readable reason.
// =============================================================================
#include <Arduino.h>
#include <WebServer.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace OtaHelper {

static bool                 otaUploadFailed    = false;
static bool                 otaUploadStarted   = false;
static bool                 otaUploadFinished  = false;
static const char          *otaFailureReason   = "";
static esp_ota_handle_t      otaHandle          = 0;
static const esp_partition_t *otaTarget         = nullptr;
static size_t                otaBytesWritten    = 0;

static inline void feed(HTTPUpload &upload, const char *logTag) {
    if (upload.status == UPLOAD_FILE_START) {
        otaUploadFailed = false; otaUploadStarted = false; otaUploadFinished = false;
        otaFailureReason = ""; otaHandle = 0; otaTarget = nullptr; otaBytesWritten = 0;
        Serial.printf("[%s] Receiving firmware binary: '%s'\n", logTag, upload.filename.c_str());

        // Always target ota_0 explicitly (the main application slot).
        otaTarget = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                             ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!otaTarget) {
            otaUploadFailed = true; otaFailureReason = "no_ota0_partition";
            Serial.printf("[%s] ERROR: ota_0 partition not found.\n", logTag);
            return;
        }
        // Refuse to overwrite the partition we are currently executing from.
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running == otaTarget) {
            otaUploadFailed = true; otaFailureReason = "running_from_ota0";
            Serial.printf("[%s] ERROR: refusing to overwrite ota_0 while running from it "
                          "(update must be performed from Safe Boot / factory).\n", logTag);
            otaTarget = nullptr;
            return;
        }
        Serial.printf("[%s] Target partition ota_0 @ 0x%06x, size %u bytes.\n",
                      logTag, (unsigned)otaTarget->address, (unsigned)otaTarget->size);

        // OTA_WITH_SEQUENTIAL_WRITES erases each flash sector on-the-fly as it is written,
        // instead of erasing the whole 2816 KB partition up front (OTA_SIZE_UNKNOWN). The
        // full up-front erase blocks the single-threaded web server for several seconds at
        // the very start of the upload, which stalled the browser at ~6% and, on a weak Wi-Fi
        // link, caused a reset mid-flash. Sequential writes make begin() return instantly and
        // the upload stream immediately. Our upload is strictly sequential, as this requires.
        esp_err_t err = esp_ota_begin(otaTarget, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
        if (err != ESP_OK) {
            otaUploadFailed = true; otaFailureReason = "begin_failed";
            Serial.printf("[%s] esp_ota_begin failed: %s\n", logTag, esp_err_to_name(err));
            otaTarget = nullptr;
            return;
        }
        otaUploadStarted = true;
        Serial.printf("[%s] esp_ota_begin OK (sequential erase-on-write), ready to stream.\n", logTag);

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (otaUploadFailed || !otaUploadStarted || upload.currentSize == 0) return;
        if (otaBytesWritten == 0 && upload.buf[0] != 0xE9) {  // ESP image magic
            otaUploadFailed = true; otaFailureReason = "bad_image_magic";
            esp_ota_abort(otaHandle); otaUploadStarted = false;
            Serial.printf("[%s] Rejected: not an ESP32 firmware image (first byte 0x%02X).\n",
                          logTag, upload.buf[0]);
            return;
        }
        esp_err_t err = esp_ota_write(otaHandle, upload.buf, upload.currentSize);
        if (err != ESP_OK) {
            otaUploadFailed = true; otaFailureReason = "write_failed";
            Serial.printf("[%s] esp_ota_write failed at %u bytes: %s\n",
                          logTag, (unsigned)otaBytesWritten, esp_err_to_name(err));
            esp_ota_abort(otaHandle); otaUploadStarted = false;
            return;
        }
        otaBytesWritten += upload.currentSize;

    } else if (upload.status == UPLOAD_FILE_END) {
        if (otaUploadFailed || !otaUploadStarted) return;
        esp_err_t err = esp_ota_end(otaHandle);
        if (err != ESP_OK) {
            otaUploadFailed = true; otaFailureReason = "end_failed";
            Serial.printf("[%s] esp_ota_end failed: %s (image may be incomplete/invalid).\n",
                          logTag, esp_err_to_name(err));
            return;
        }
        err = esp_ota_set_boot_partition(otaTarget);
        if (err != ESP_OK) {
            otaUploadFailed = true; otaFailureReason = "set_boot_failed";
            Serial.printf("[%s] esp_ota_set_boot_partition failed: %s\n", logTag, esp_err_to_name(err));
            return;
        }
        otaUploadFinished = true;
        Serial.printf("[%s] Firmware write complete (%u bytes). Boot partition set to ota_0.\n",
                      logTag, (unsigned)otaBytesWritten);

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaUploadFailed = true; otaFailureReason = "aborted";
        if (otaUploadStarted) { esp_ota_abort(otaHandle); otaUploadStarted = false; }
    }
}

static inline bool succeeded() { return otaUploadFinished && !otaUploadFailed; }

// Sends the JSON result; returns true when the caller should reboot.
static inline bool respond(WebServer &server) {
    bool uploadSucceeded = succeeded();
    char jsonResponseBuffer[96];
    snprintf(jsonResponseBuffer, sizeof(jsonResponseBuffer), "{\"ok\":%s,\"error\":\"%s\"}",
             uploadSucceeded ? "true" : "false",
             uploadSucceeded ? "" : (otaFailureReason[0] ? otaFailureReason : "unknown"));
    server.sendHeader("Connection", "close");
    server.send(uploadSucceeded ? 200 : 500, "application/json", jsonResponseBuffer);
    return uploadSucceeded;
}

}  // namespace OtaHelper
