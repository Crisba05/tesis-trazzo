// src/drivers/i2c_bus.cpp
#include "drivers/i2c_bus.h"
#include <Wire.h>
#include "config.h"

namespace i2c {
    static SemaphoreHandle_t mtx = nullptr;

    bool begin() {
        if (mtx == nullptr) {
            mtx = xSemaphoreCreateMutex();
        }
        Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
        // 100kHz — safer on long/loose Dupont wiring. Bump to 400000 later
        // once the LCD is confirmed responding reliably.
        Wire.setClock(100000);
        return mtx != nullptr;
    }

    bool take(TickType_t timeoutTicks) {
        if (!mtx) return false;
        return xSemaphoreTake(mtx, timeoutTicks) == pdTRUE;
    }

    void give() {
        if (mtx) xSemaphoreGive(mtx);
    }

    void scan() {
        Serial.println("[i2c] scanning bus...");
        int found = 0;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            uint8_t err = Wire.endTransmission();
            if (err == 0) {
                Serial.printf("[i2c] device @ 0x%02X\n", addr);
                found++;
            }
        }
        if (found == 0) {
            Serial.println("[i2c] NO DEVICES FOUND — check SDA/SCL wiring + pull-ups + power");
        } else {
            Serial.printf("[i2c] scan done: %d device(s)\n", found);
        }
    }

    void recover() {
        Wire.end();
        pinMode(pins::I2C_SCL, OUTPUT);
        pinMode(pins::I2C_SDA, INPUT_PULLUP);
        // Clock 9 SCL edges to free any slave still holding the bus low.
        for (int i = 0; i < 9; ++i) {
            digitalWrite(pins::I2C_SCL, HIGH);
            delayMicroseconds(5);
            digitalWrite(pins::I2C_SCL, LOW);
            delayMicroseconds(5);
        }
        digitalWrite(pins::I2C_SCL, HIGH);
        Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
        Wire.setClock(100000);
    }
}
