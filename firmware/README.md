# Firmware

Place microcontroller projects here (Arduino/PlatformIO). Target options:
- ESP32 (recommended for more GPIO/ADC for load cell + RFID)
- ESP8266 (pin-limited; may need I2C amplifiers)
- Other MCUs as needed

Include sketches/projects under subfolders, with their own env files kept out of VCS.

Examples:
- `fx29_i2c_example/`: minimal I2C reader for the TE FX29 I2C variant (not for amplified analog parts).
- `load_cell_adc_logger/`: analog logger for the amplified FX29 (0.5–4.5 V out) on ESP32-S2; prints raw ADC and mV so you can record weight/voltage pairs for calibration. Set `LOAD_CELL_PIN` to your chosen ADC pin.
