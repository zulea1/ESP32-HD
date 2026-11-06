/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file async_log.cpp
 * @brief Ring buffer + drain task for the non-blocking serial logger (see async_log.h).
 */
#define ASYNC_LOG_IMPL
#include "async_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

AsyncLog gAsyncLog;

static uint8_t           ringBuffer[LOG_RING_SIZE];
static volatile uint32_t ringHeadIndex = 0, ringTailIndex = 0;      // head: next write, tail: next read (free-running counters)
static volatile uint32_t droppedByteCount = 0;               // bytes dropped since the last notice
static portMUX_TYPE      ringBufferMutex = portMUX_INITIALIZER_UNLOCKED;
static bool              loggerTaskStarted = false;

static void ringPut(const uint8_t *bytes, size_t byteCount) {
    portENTER_CRITICAL(&ringBufferMutex);
    size_t usedBytes = (size_t)(ringHeadIndex - ringTailIndex);
    size_t freeBytes = LOG_RING_SIZE - usedBytes;
    if (byteCount > freeBytes) {                                // never block: drop the whole message
        droppedByteCount += (uint32_t)byteCount;
    } else {
        for (size_t byteIndex = 0; byteIndex < byteCount; byteIndex++) ringBuffer[(ringHeadIndex + byteIndex) % LOG_RING_SIZE] = bytes[byteIndex];
        ringHeadIndex += (uint32_t)byteCount;
    }
    portEXIT_CRITICAL(&ringBufferMutex);
}

size_t AsyncLog::write(uint8_t byteValue) { ringPut(&byteValue, 1); return 1; }
size_t AsyncLog::write(const uint8_t *byteBuffer, size_t byteCount) {
    if (!byteBuffer || !byteCount) return 0;
    ringPut(byteBuffer, byteCount);
    return byteCount;
}

size_t AsyncLog::writeLine(const char *text) {
    char lineBuffer[LOG_LINE_MAX];
    size_t lineLength = text ? strlen(text) : 0;
    if (lineLength > LOG_LINE_MAX - 3) lineLength = LOG_LINE_MAX - 3;
    if (lineLength) memcpy(lineBuffer, text, lineLength);
    lineBuffer[lineLength++] = '\r'; lineBuffer[lineLength++] = '\n';  // one atomic insertion: lines from different tasks never interleave
    ringPut((const uint8_t *)lineBuffer, lineLength);
    return lineLength;
}

size_t AsyncLog::printf(const char *formatString, ...) {
    char lineBuffer[LOG_LINE_MAX];
    va_list variadicArgs; va_start(variadicArgs, formatString);
    int formattedLength = vsnprintf(lineBuffer, sizeof(lineBuffer), formatString, variadicArgs);
    va_end(variadicArgs);
    if (formattedLength <= 0) return 0;
    if ((size_t)formattedLength >= sizeof(lineBuffer)) formattedLength = sizeof(lineBuffer) - 1;
    ringPut((const uint8_t *)lineBuffer, (size_t)formattedLength);
    return (size_t)formattedLength;
}

size_t AsyncLog::drainOnce(size_t maxBytesToDrain) {
    uint8_t drainChunk[256];
    if (maxBytesToDrain > sizeof(drainChunk)) maxBytesToDrain = sizeof(drainChunk);
    size_t drainedByteCount = 0;
    uint32_t droppedByteSnapshot = 0;
    portENTER_CRITICAL(&ringBufferMutex);
    size_t availableByteCount = (size_t)(ringHeadIndex - ringTailIndex);
    drainedByteCount = availableByteCount < maxBytesToDrain ? availableByteCount : maxBytesToDrain;
    for (size_t chunkIndex = 0; chunkIndex < drainedByteCount; chunkIndex++) drainChunk[chunkIndex] = ringBuffer[(ringTailIndex + chunkIndex) % LOG_RING_SIZE];
    ringTailIndex += (uint32_t)drainedByteCount;
    if (drainedByteCount == 0 && droppedByteCount) { droppedByteSnapshot = droppedByteCount; droppedByteCount = 0; }
    portEXIT_CRITICAL(&ringBufferMutex);

    if (drainedByteCount) ::Serial.write(drainChunk, drainedByteCount);             // may block, but only this task
    if (droppedByteSnapshot) {
        char dropNoticeMessage[48];
        int dropNoticeLength = snprintf(dropNoticeMessage, sizeof(dropNoticeMessage), "[LOG] %lu bytes dropped (log too fast)\r\n", (unsigned long)droppedByteSnapshot);
        if (dropNoticeLength > 0) ringPut((const uint8_t *)dropNoticeMessage, (size_t)dropNoticeLength);
        droppedByteTotal_ += droppedByteSnapshot;
    }
    return drainedByteCount;
}

static void logTask(void *) {
    for (;;) {
        if (gAsyncLog.drainOnce(256) == 0) vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void AsyncLog::begin(unsigned long baudRate) {
    ::Serial.begin(baudRate);
    if (!loggerTaskStarted) {
        loggerTaskStarted = true;
        xTaskCreatePinnedToCore(logTask, "logTask", 3072, nullptr, 1, nullptr, 0);   // lowest priority, core 0
    }
}
