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
// Calibration now uses empty spool as zero (TARE_MV)
static constexpr float CAL_SLOPE = 1.750685f;
static constexpr float CAL_INTERCEPT = -1006.87f;
// Tare assumes empty spool ~247 g → mV_tare ≈ (247 - intercept) / slope ≈ 549 mV
static constexpr float TARE_MV = 575.14f;

// Store calibration points
struct CalPoint
{
    float weight_g;
    int raw;
    int mv;
    float avg_raw;
    float avg_mv;
};
static constexpr int MAX_POINTS = 32;
CalPoint cal_points[MAX_POINTS];
int cal_count = 0;

void handleSerialInput()
{
    if (!Serial.available())
        return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
        return;
    if (line.equalsIgnoreCase("x"))
    {
        // Output all calibration points as a list of lists
        Serial.println(F("\nCalibration points for Python script (copy/paste):"));
        Serial.print("[");
        for (int i = 0; i < cal_count; ++i)
        {
            Serial.print("[");
            Serial.print(cal_points[i].weight_g, 3);
            Serial.print(", ");
            Serial.print(cal_points[i].raw);
            Serial.print(", ");
            Serial.print(cal_points[i].mv);
            Serial.print(", ");
            Serial.print(cal_points[i].avg_raw, 2);
            Serial.print(", ");
            Serial.print(cal_points[i].avg_mv, 2);
            Serial.print("]");
            if (i < cal_count - 1)
                Serial.print(", ");
        }
        Serial.println("]\n");
        return;
    }
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

    // Save calibration point
    if (cal_count < MAX_POINTS)
    {
        cal_points[cal_count].weight_g = weight;
        cal_points[cal_count].raw = latest_raw;
        cal_points[cal_count].mv = latest_mv;
        cal_points[cal_count].avg_raw = latest_avg_raw;
        cal_points[cal_count].avg_mv = latest_avg_mv;
        cal_count++;
    }

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
