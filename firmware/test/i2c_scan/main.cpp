// test/i2c_scan/main.cpp
// Diagnostic: scans the I2C bus and reports all device addresses found.
// Flash with: pio run -e esp32-dev -t upload
// (temporarily rename src/main.cpp or use a test env)

#include <Arduino.h>
#include <Wire.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    Wire.begin(21, 22);
    delay(200);

    Serial.println();
    Serial.println("=== I2C Bus Scanner ===");
    Serial.println("Scanning addresses 0x01 to 0x7E...");
    Serial.println();

    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            Serial.printf("  FOUND device at 0x%02X", addr);
            if (addr == 0x3C) Serial.print("  <-- OLED SSD1306");
            if (addr == 0x27) Serial.print("  <-- LCD PCF8574 (tipo A)");
            if (addr == 0x3F) Serial.print("  <-- LCD PCF8574 (tipo AT)");
            Serial.println();
            found++;
        }
    }

    Serial.println();
    if (found == 0) {
        Serial.println("NO devices found! Check wiring: SDA=GPIO21, SCL=GPIO22, GND common.");
    } else {
        Serial.printf("Found %d device(s).\n", found);
    }

    Serial.println();
    Serial.println("If LCD shows 0x3F instead of 0x27:");
    Serial.println("  -> Change LCD_I2C_ADDR in include/config.h to 0x3F");
    Serial.println();
    Serial.println("If OLED not found (no 0x3C):");
    Serial.println("  -> Check OLED VCC is on 3.3V rail (NOT 5V)");
    Serial.println("  -> Check SDA/SCL wiring");
}

void loop() {
    delay(10000);
}
