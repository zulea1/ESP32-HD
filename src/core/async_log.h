/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file async_log.h
 * @brief Non-blocking serial logger.
 *
 * Every Serial.print/println/printf call in the firmware is redirected (macro at the end of this file) to a RAM ring
 * buffer. A separate low-priority FreeRTOS task (core 0) drains the ring to the real UART. Callers therefore never
 * wait for the UART (115200 baud = ~11 KB/s used to stall the CAN/ISO-TP tasks while the TX FIFO drained).
 * If the ring is full, new text is dropped (never blocks) and a "[LOG] N bytes dropped" notice is printed later.
 */
#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>

#ifndef LOG_BAUD
#define LOG_BAUD 921600          // must match monitor_speed in platformio.ini
#endif
#ifndef LOG_RING_SIZE
#define LOG_RING_SIZE 8192
#endif
#define LOG_LINE_MAX 256         // longest single formatted message (longer ones are truncated)

class AsyncLog : public Print {
public:
    void begin(unsigned long baudRate = LOG_BAUD);          // starts the UART and the drain task
    size_t write(uint8_t byteValue) override;
    size_t write(const uint8_t *byteBuffer, size_t byteCount) override;
    size_t write(const char *text) { return text ? write((const uint8_t *)text, strlen(text)) : 0; }

    using Print::print;
    using Print::println;
    size_t print(const char *text) { return write(text); }
    size_t println() { return write((const uint8_t *)"\r\n", 2); }
    size_t println(const char *text) { return writeLine(text); }
    size_t println(const String &text) { return writeLine(text.c_str()); }
    size_t printf(const char *formatString, ...) __attribute__((format(printf, 2, 3)));

    size_t drainOnce(size_t maxBytesToDrain = 256);           // used by the task (and by host tests)
    uint32_t dropped() const { return droppedByteTotal_; }

private:
    size_t writeLine(const char *text);
    volatile uint32_t droppedByteTotal_ = 0;
};

extern AsyncLog gAsyncLog;

#ifndef ASYNC_LOG_IMPL
// Arduino-ESP32 3.x's own HardwareSerial.h now defines "Serial" as a macro too (#define Serial
// Serial0, to support boards with a USB-CDC/UART0 choice). #undef it first so our redirect below
// is a clean (re)definition instead of a macro-redefinition warning - purely cosmetic, no change
// in behavior either way: Serial.print/println/printf still resolve to gAsyncLog exactly as before.
#undef Serial
#define Serial gAsyncLog        // all firmware code logs through the asynchronous logger
#endif
