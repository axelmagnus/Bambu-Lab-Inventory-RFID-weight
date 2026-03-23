// Simple ADC logger for the amplified FX29 (0.5–4.5 V out). For ESP32-S2.
// Adjust LOAD_CELL_PIN to match your board’s ADC-capable pin.
// Uses analogReadMilliVolts() for direct mV readings. Apply known weights and
// type the weight (g) into Serial to tag the latest reading; the latest
// voltage is printed once you enter a weight.

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// TODO: set this to the ADC pin you wire the load cell output to.
// Pick an ADC1-capable pin on the ESP32-S2; e.g., A0 on the Adafruit ESP32-S2 Reverse TFT.
static constexpr int LOAD_CELL_PIN = A0;
#define TFT_CS 42
#define TFT_DC 40
#define TFT_RST 41
#define TFT_BACKLITE 45
#define TFT_I2C_POWER 7
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// Simple moving average to smooth readings
static constexpr int AVG_WINDOW = 30;
int32_t acc = 0;
int idx = 0;
int samples[AVG_WINDOW] = {0};
bool measurements_done;

// Batch averaging for estimated weight
float est_weight_samples[AVG_WINDOW] = {0};
int est_weight_count = 0;
float est_weight_sum = 0;
bool est_weight_ready = false;

// Latest values cached so we can print them when a weight is entered.
static int latest_raw = 0;
static int latest_mv = 0;
static float latest_avg_raw = 0;
static float latest_avg_mv = 0;

// Calibration (using avg mV): weight_g ≈ slope * mV + intercept
// Calibration now uses empty spool as zero (TARE_MV)
// Tare assumes empty spool ~247 g → mV_tare ≈ (247 - intercept) / slope ≈ 628.2 mV
static constexpr float CAL_SLOPE = 1.725510f;
static constexpr float CAL_INTERCEPT = -1004.43f;
// Tare assumes empty spool ~247 g → mV_tare ≈ (247 - intercept) / slope ≈ 624.6 mV
static constexpr float TARE_MV = 725.25f;

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
        // Output all calibration points as a list of lists (Python)
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

        // Output as NumPy array for easy analysis
        // Output as Python array with columns comment for easy analysis
        Serial.println(F("Python array (copy into Python):"));
        Serial.println("[");
        for (int i = 0; i < cal_count; ++i)
        {
            Serial.print("    [");
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
                Serial.print(",");
            Serial.println();
        }
        Serial.println("], columns=[\"weight_g\", \"raw\", \"mv\", \"avg_raw\", \"avg_mv\"])");
        Serial.println("])");

        // Output all calibration points as CSV
        Serial.println(F("Calibration points as CSV (weight_g,raw,mv,avg_raw,avg_mv):"));
        Serial.println(F("weight_g,raw,mv,avg_raw,avg_mv"));
        for (int i = 0; i < cal_count; ++i)
        {
            Serial.print(cal_points[i].weight_g, 3);
            Serial.print(",");
            Serial.print(cal_points[i].raw);
            Serial.print(",");
            Serial.print(cal_points[i].mv);
            Serial.print(",");
            Serial.print(cal_points[i].avg_raw, 2);
            Serial.print(",");
            Serial.println(cal_points[i].avg_mv, 2);
        }

        // Set a flag to end measurements
        measurements_done = true;
        return;
    }
    if (!measurements_done)
    {
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

        // Compute estimated filament (net) weight from avg mV
        float est_filament = CAL_SLOPE * latest_avg_mv + CAL_INTERCEPT;
        Serial.print(F("Est filament (g): "));
        Serial.println(est_filament, 2);
    }
    // Add at the top of the file or before setup()
    bool measurements_done = false;
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println(F("Load cell ADC logger (ESP32-S2)"));
    Serial.println(F("Apply known weights; type weight (g) in Serial to tag the latest reading."));
    Serial.println(F("Outputs readings only when you enter a weight."));

    pinMode(TFT_BACKLITE, OUTPUT);
    digitalWrite(TFT_BACKLITE, HIGH);
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    SPI.begin();
    tft.init(135, 240); // Init ST7789 240x135
    tft.setRotation(3); // Landscape
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 72);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(3);
    tft.print("Loadcell!");
}

void loop()
{
    // Take 30 samples as fast as possible
    float sum_raw = 0;
    float sum_mv = 0;
    float sum_filament_weight = 0;
    for (int i = 0; i < AVG_WINDOW; ++i)
    {
        int raw = analogRead(LOAD_CELL_PIN);
        int mv = analogReadMilliVolts(LOAD_CELL_PIN);
        sum_raw += raw;
        sum_mv += mv;
        float mv_per_count = (raw > 0) ? (static_cast<float>(mv) / raw) : 0.0f;
        float avg_mv = raw * mv_per_count; // for this sample
        float est_filament = CAL_SLOPE * avg_mv + CAL_INTERCEPT;
        sum_filament_weight += est_filament;
        delay(5); // short delay for ADC settling
    }
    float avg_raw = sum_raw / AVG_WINDOW;
    float avg_mv = sum_mv / AVG_WINDOW;
    float avg_filament_weight = sum_filament_weight / AVG_WINDOW;

    latest_raw = (int)avg_raw;
    latest_mv = (int)avg_mv;
    latest_avg_raw = avg_raw;
    latest_avg_mv = avg_mv;

    // Display on TFT
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 20);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.printf("Filament: %.1f g", avg_filament_weight);

    handleSerialInput();

    delay(100);
}
