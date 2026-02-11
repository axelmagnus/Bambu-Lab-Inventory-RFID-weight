# Hardware

Document wiring, BOM, and calibration fixtures for RFID reader + load cell + amplifier + MCU.

Notes for load cell options (TE FX29 family):
- Amplified analog variant (e.g., FX293X-100A-0010-L): outputs ~0.5–4.5 V at 3.3 V; use an ADC input. ESP8266 A0 needs a divider unless your board has one; ESP32-S2 ADC can take 0–3.3 V.
- I2C variant: built-in conditioning and digital output; no HX711 required. Read over I2C, mask status bits, and calibrate.
- OLED pull-ups: most SSD1306 OLED breakouts include 4.7 kΩ pull-ups on SDA/SCL; confirm on your board. Those pull-ups typically suffice for sharing the I2C bus.