// Simple ADC logger for the amplified FX29 (0.5–4.5 V out). For ESP32-S2.
// Adjust LOAD_CELL_PIN to match your board’s ADC-capable pin.
// Uses analogReadMilliVolts() for direct mV readings. Apply known weights and
// type the weight (g) into Serial to tag the latest reading; the latest
// voltage is printed once you enter a weight.

#include <Arduino.h>

// TODO: set this to the ADC pin you wire the load cell output to.
// Pick an ADC1-capable pin on the ESP32-S2; e.g., A0 on the Adafruit ESP32-S2 Reverse TFT.
static constexpr int LOAD_CELL_PIN = A0;

// Simple moving average to smooth readings
static constexpr int AVG_WINDOW = 8;
int32_t acc = 0;
int idx = 0;
int samples[AVG_WINDOW] = {0};

// Latest values cached so we can print them when a weight is entered.
static int latest_raw = 0;
static int latest_mv = 0;
static float latest_avg_raw = 0;
static float latest_avg_mv = 0;

// Calibration (using avg mV): weight_g ≈ slope * mV + intercept
// Updated with zero-weight reading (~410 mV) and full spool (~1100 mV @ 1247.5 g)
static constexpr float CAL_SLOPE = 1.80f;
static constexpr float CAL_INTERCEPT = -741.0f;
// Tare assumes empty spool ~247 g → mV_tare ≈ (247 - intercept) / slope ≈ 549 mV
static constexpr float TARE_MV = 549.0f;

void handleSerialInput()
{
    if (!Serial.available())
        return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
        return;
    float weight = line.toFloat();
    Serial.print(F("Tagging weight (g): "));
    Serial.println(weight, 3);
    Serial.print(F("Reading: raw="));
    Serial.print(latest_raw);
    Serial.print(F(", mV="));
    Serial.print(latest_mv);
    Serial.print(F(", avg_raw="));
    Serial.print(latest_avg_raw, 2);
    Serial.print(F(", avg_mV="));
    Serial.println(latest_avg_mv, 2);

    // Compute estimated gross and filament-only weights from avg mV
    float est_gross = CAL_SLOPE * latest_avg_mv + CAL_INTERCEPT;
    float est_filament = CAL_SLOPE * (latest_avg_mv - TARE_MV);
    Serial.print(F("Est gross (g): "));
    Serial.print(est_gross, 2);
    Serial.print(F(", est filament (g): "));
    Serial.println(est_filament, 2);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println(F("Load cell ADC logger (ESP32-S2)"));
    Serial.println(F("Apply known weights; type weight (g) in Serial to tag the latest reading."));
    Serial.println(F("Outputs readings only when you enter a weight."));
}

void loop()
{
    int raw = analogRead(LOAD_CELL_PIN);
    int mv = analogReadMilliVolts(LOAD_CELL_PIN);

    acc -= samples[idx];
    samples[idx] = raw;
    acc += raw;
    idx = (idx + 1) % AVG_WINDOW;
    float avg_raw = static_cast<float>(acc) / AVG_WINDOW;
    // Convert avg raw to mV using mv/raw ratio from this sample
    float mv_per_count = (raw > 0) ? (static_cast<float>(mv) / raw) : 0.0f;
    float avg_mv = avg_raw * mv_per_count;

    latest_raw = raw;
    latest_mv = mv;
    latest_avg_raw = avg_raw;
    latest_avg_mv = avg_mv;

    handleSerialInput();

    delay(50);
}
