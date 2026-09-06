#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("teletrack boot");
}

void loop() {
    Serial.printf("alive %lu\n", (unsigned long)millis());
    delay(1000);
}

#endif  // PIO_UNIT_TESTING
