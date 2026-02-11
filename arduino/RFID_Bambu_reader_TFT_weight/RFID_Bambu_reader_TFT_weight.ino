// RFID_Bambu_reader_TFT_weight.ino
// For Adafruit ESP32-S2 Reverse TFT
// Requirements (see .github/copilot-instructions.md):
// - D0 button: Tare for weighing empty filament roll.
// - D1 button: Scan RFID tag (read UID, code, type, color, TrayUID, weight from load cell). Do NOT send to inventory yet. Display all info on TFT, set TFT background to filament color (HEX if possible). Use a local mapping (e.g., JSON object) to look up filament details from UID.
// - D2 button: Option to send data to inventory sheet. Connect to WiFi if not already on.
// - Apps Script: Accepts filament code and weight, updates inventory sheet. If code exists, update weight; else, add new row. Columns: Time scanned, Filament Code, Type, Name, Weight (g), Image, Tray UID for roll.
// - At startup, print "Tare = D0", "Send to inventory = D2" on TFT.
// - Tare: On press, prompt to place empty roll (247 g), then known weight, then press again to compute/store scale factor in EEPROM (persists across power cycles).
// - Use only SpreadsheetApp in Apps Script (no UrlFetch).
// - No secrets in code; use local env or Script Properties.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
// #include <Wire.h>
//  #include <MFRC522.h>
#include <Adafruit_GFX.h> // Core graphics library
#include <Adafruit_ST7789.h>
#include <SPI.h>
// --- WiFi/HTTP secrets (from secrets.h, not in code) ---
#include "secrets.h"
// --- Pin definitions (update as needed) ---

// --- Pin definitions (update as needed) ---
// Pin mapping for 128x64 TFT (ST7735 or similar)
#define LOAD_CELL_PIN A0
#define BUTTON_SEND 2 // D2
// #define RFID_SS 7
// #define RFID_RST 6

#define TFT_CS 42
#define TFT_DC 40
#define TFT_RST 41
#define TFT_BACKLITE 45
#define TFT_I2C_POWER 7

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
// MFRC522 rfid(RFID_SS, RFID_RST);

// --- Filament metadata mapping (UID to info) ---
struct FilamentInfo
{
    const char *code;
    const char *color_name;
    const char *type;
    const char *tray_uid;
    uint32_t color_hex; // 0xRRGGBB
};

// Example: replace with real UIDs and data
FilamentInfo filament_db[] = {
    {"A1B2C3D4", "Red", "PLA", "TRAY123", 0xFF0000},
    {"E5F6A7B8", "Blue", "PETG", "TRAY456", 0x0000FF},
    // ...
};
const int FILAMENT_DB_SIZE = sizeof(filament_db) / sizeof(filament_db[0]);

// --- Weight constants ---
static constexpr float CAL_SLOPE = 1.80f;
static constexpr float CAL_INTERCEPT = -741.0f;
static constexpr float EMPTY_SPOOL_WEIGHT = 247.0f; // grams

// --- State ---
char last_uid[16] = "";
FilamentInfo *last_filament = nullptr;
float last_weight = 0;
bool wifi_connected = false;

// --- Function prototypes ---
// void scanRFID();
void readLoadCell();
void showOnTFT();
void handleSend();
void connectWiFi();

// --- WiFi connect ---
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.print("Connecting WiFi...");
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
    {
        delay(200);
    }
    wifi_connected = (WiFi.status() == WL_CONNECTED);
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(wifi_connected ? ST77XX_GREEN : ST77XX_RED);
    tft.setTextSize(2);
    tft.print(wifi_connected ? "WiFi OK" : "WiFi FAIL");
    delay(800);
}

// --- Send to inventory sheet ---
void handleSend()
{
    if (!wifi_connected)
        connectWiFi();
    if (!wifi_connected || !last_filament)
        return;
    HTTPClient http;
    http.begin(WEB_APP_URL);
    http.addHeader("Content-Type", "application/json");
    String payload = String("{\"code\":\"") + last_filament->code +
                     "\",\"type\":\"" + last_filament->type +
                     "\",\"name\":\"" + last_filament->color_name +
                     "\",\"weight\":" + String(last_weight, 1) +
                     ",\"trayUid\":\"" + last_filament->tray_uid +
                     "\",\"uid\":\"" + last_uid + "\"}";
    int httpCode = http.POST(payload);
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(httpCode > 0 && httpCode < 400 ? ST77XX_GREEN : ST77XX_RED);
    tft.setTextSize(2);
    tft.print(httpCode > 0 && httpCode < 400 ? "Sent!" : "Send FAIL");
    http.end();
}

FilamentInfo *lookupFilament(const char *uid);
void setTFTBackground(uint32_t color);
void printButtonFunctions();

void setup()
{
    Serial.begin(115200);
    // Print TFT pin assignments
    Serial.print("TFT_CS=");
    Serial.println(TFT_CS);
    Serial.print("TFT_DC=");
    Serial.println(TFT_DC);
    Serial.print("TFT_RST=");
    Serial.println(TFT_RST);
    // Turn on backlight and TFT power as per Adafruit example
    pinMode(TFT_BACKLITE, OUTPUT);
    digitalWrite(TFT_BACKLITE, HIGH);
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    // pinMode(BUTTON_SEND, INPUT_PULLUP);

    Serial.println("RFID Bambu reader with TFT and weight");
    // printButtonFunctions();
    delay(10); // Allow power to stabilize

    Serial.println("Initializing TFT...");
    tft.init(135, 240); // Init ST7789 240x135
    tft.setRotation(1); // Adafruit recommends rotation 3 for landscape
    tft.fillScreen(ST77XX_BLACK);
    // rfid.PCD_Init(); // RFID not used

    Serial.println("Setup complete. Ready to scan.");
    tft.setCursor(0, 0);
    tft.setTextColor(ST77XX_WHITE);

    Serial.println("Scan RFID + weight, then press D2 to send to inventory.");
    tft.setTextSize(1);
    tft.print("Scan RFID + weight\nPress D2 to send");
}

void loop()
{
    // No debounceButtons() needed; removed undefined function
    // Default mode: always scan RFID and weight, show on TFT
    // scanRFID(); // RFID not used
    readLoadCell();
    showOnTFT();
    // Only BUTTON_SEND (D2) is used for sending/updating inventory
    if (digitalRead(BUTTON_SEND) == HIGH) // Active HIGH button
    {
        handleSend();
    }
}
/**
// --- Function implementations ---
void scanRFID()
{
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    {
        strcpy(last_uid, "");
        last_filament = nullptr;
        return;
    }
    char uid_buf[16] = "";
    for (byte i = 0; i < rfid.uid.size; i++)
    {
        sprintf(uid_buf + i * 2, "%02X", rfid.uid.uidByte[i]);
    }
    strncpy(last_uid, uid_buf, sizeof(last_uid) - 1);
    last_uid[sizeof(last_uid) - 1] = '\0';
    last_filament = lookupFilament(last_uid);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}
*/
void readLoadCell()
{
    // Average 10 samples for stable reading
    long mv_sum = 0;
    const int samples = 10;
    for (int i = 0; i < samples; ++i)
    {
        mv_sum += analogReadMilliVolts(LOAD_CELL_PIN);
        delay(5); // Short delay between samples
    }
    int mv = mv_sum / samples;
    float gross_weight = CAL_SLOPE * mv + CAL_INTERCEPT;
    float net_weight = gross_weight - EMPTY_SPOOL_WEIGHT;
    if (net_weight < 0)
        net_weight = 0;
    last_weight = net_weight;
    Serial.print("Reading load cell... Weight: ");
    Serial.print(last_weight, 1);
    Serial.println(" g");
}

void showOnTFT()
{
    tft.fillScreen(last_filament ? last_filament->color_hex : ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    if (last_filament)
    {
        tft.printf("Code: %s\nType: %s\nColor: %s\nTray: %s\nWeight: %.1f g", last_filament->code, last_filament->type, last_filament->color_name, last_filament->tray_uid, last_weight);
    }
    else
    {
        tft.printf("Weight: %.1f g", last_weight);
    }
}

FilamentInfo *lookupFilament(const char *uid)
{
    for (int i = 0; i < FILAMENT_DB_SIZE; ++i)
    {
        if (strcmp(uid, filament_db[i].code) == 0)
            return &filament_db[i];
    }
    return nullptr;
}

void setTFTBackground(uint32_t color)
{
    tft.fillScreen(color);
}

void printButtonFunctions()
{
    Serial.println("Scan = D1, Send = D2");
    tft.setCursor(0, 56);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.print("Scan=D1 Send=D2");
}

// TODO: Add functions for:
