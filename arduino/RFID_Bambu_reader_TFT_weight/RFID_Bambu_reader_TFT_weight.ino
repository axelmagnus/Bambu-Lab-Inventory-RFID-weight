
// RFID_Bambu_reader_TFT_weight.ino
// For Adafruit ESP32-S2 Reverse TFT
// Requirements (see .github/copilot-instructions.md):
// - at startup Scan RFID tag (read UID, code, type, color, TrayUID, weight from load cell). Do NOT send to inventory yet. Display all info on TFT, set TFT background to filament color (HEX if possible). Use a local mapping (e.g., JSON object) to look up filament details from UID.
// - D2 button: Option to send data to inventory sheet. Connect to WiFi if not already on.
// - Apps Script: Accepts filament code and weight, updates inventory sheet. If code exists, update weight; else, add new row. Columns: Time scanned, Filament Code, Type, Name, Weight (g), Image, Tray UID for roll.
// - At startup, print "Tare = D0", "Send to inventory = D2" on TFT.
// - Tare: On press, prompt to place empty roll (247 g), then known weight, then press again to compute/store scale factor in EEPROM (persists across power cycles).
// - Use only SpreadsheetApp in Apps Script (no UrlFetch).
// - No secrets in code; use local env or Script Properties.

// ...existing code...
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
// #include <Wire.h>
//  #include <MFRC522.h>
#include <Adafruit_GFX.h> // Core graphics library
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <MFRC522.h>
#include "material_lookup.h"
#include <mbedtls/md.h>
#include <secrets.h>
// --- HKDF constants and sector key array ---
static const uint8_t HKDF_SALT[16] = {0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff, 0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96};
static const uint8_t HKDF_INFO[7] = {'R', 'F', 'I', 'D', '-', 'A', 0x00};
static byte SECTOR_KEY_A[16][6];

#define LOAD_CELL_PIN A0
#define BUTTON_SEND 2 // D2
// RFID pins (update as needed)
#define RFID_SS 13
#define RFID_RST 11

#define TFT_CS 42
#define TFT_DC 40
#define TFT_RST 41

#define TFT_BACKLITE 45
#define TFT_I2C_POWER 7

// Buzzer pin (suggested: GPIO 6, not used by TFT or RFID)
#define BUZZER_PIN 6

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
MFRC522 rfid(RFID_SS, RFID_RST);

// --- State ---
bool wifi_connected = false;

// --- Decoded filament info ---
char last_uid[16] = "";
char filament_code[8] = "";
char filament_type[16] = "";
char filament_color[21] = "";
char tray_uid[33] = "";
char tray_uid_short[7] = "";
float last_weight = 0;

// --- Weight constants ---
static constexpr float CAL_SLOPE = 1.80f;
static constexpr float CAL_INTERCEPT = -741.0f;
static constexpr float EMPTY_SPOOL_WEIGHT = 247.0f; // grams

// --- Function prototypes ---
// void scanRFID();
void readLoadCell();
void showOnTFT();
void scanRFID();
void handleSend();
void connectWiFi();
const MaterialInfo *lookupMaterial(const char *materialId, const char *variantId);

void hkdfFromUid(const uint8_t *uid, size_t uidLen, uint8_t *out, size_t outLen)
{
    // Step 1: Extract (PRK = HMAC-SHA256(salt, uid))
    uint8_t prk[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, HKDF_SALT, sizeof(HKDF_SALT));
    mbedtls_md_hmac_update(&ctx, uid, uidLen);
    mbedtls_md_hmac_finish(&ctx, prk);
    mbedtls_md_free(&ctx);

    // Step 2: Expand
    size_t pos = 0;
    uint8_t counter = 1;
    uint8_t t[32];
    size_t infoLen = sizeof(HKDF_INFO);
    while (pos < outLen)
    {
        mbedtls_md_context_t ctx2;
        mbedtls_md_init(&ctx2);
        mbedtls_md_setup(&ctx2, info, 1);
        mbedtls_md_hmac_starts(&ctx2, prk, sizeof(prk));
        if (counter > 1)
            mbedtls_md_hmac_update(&ctx2, t, sizeof(t));
        mbedtls_md_hmac_update(&ctx2, HKDF_INFO, infoLen);
        mbedtls_md_hmac_update(&ctx2, &counter, 1);
        mbedtls_md_hmac_finish(&ctx2, t);
        mbedtls_md_free(&ctx2);
        size_t take = (outLen - pos < sizeof(t)) ? (outLen - pos) : sizeof(t);
        memcpy(out + pos, t, take);
        pos += take;
        counter++;
    }
}
// ...existing code...

void deriveKeysFromUid(const byte *uid, byte uidLen)
{
    uint8_t derived[16 * 6];
    hkdfFromUid(uid, uidLen, derived, sizeof(derived));
    for (uint8_t s = 0; s < 16; s++)
    {
        memcpy(SECTOR_KEY_A[s], derived + s * 6, 6);
    }
}

// --- WiFi connect ---
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    // Only show splash if not already connected
    if (!wifi_connected)
    {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(0, 0);
        tft.setTextColor(ST77XX_WHITE);
        tft.setTextSize(2);
        tft.print("Connecting to\n");
        tft.setCursor(0, 32);
        tft.print(WIFI_SSID);
    }
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
    {
        delay(200);
    }
    wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (!wifi_connected)
    {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(0, 0);
        tft.setTextColor(wifi_connected ? ST77XX_GREEN : ST77XX_RED);
        tft.setTextSize(2);
        tft.print(wifi_connected ? "WiFi OK" : "WiFi FAIL");
        delay(800);
    }
}

// --- Send to inventory sheet ---
void handleSend()
{
    // Placeholder for future webhook logic
    if (!wifi_connected)
        connectWiFi();
    if (!wifi_connected)
        return;
    // Update only last row with 'Sending...'
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 118);
    tft.printf("Sending....         ");
    HTTPClient http;
    http.begin(WEB_APP_URL);
    http.addHeader("Content-Type", "application/json");
    String payload = String("{\"code\":\"") + filament_code +
                     "\",\"type\":\"" + filament_type +
                     "\",\"name\":\"" + filament_color +
                     "\",\"weight\":" + String((int)last_weight) +
                     ",\"trayUid\":\"" + tray_uid +
                     "\",\"uid\":\"" + last_uid + "\"}";
    int httpCode = http.POST(payload);
    tft.setTextColor((httpCode > 0 && httpCode < 400) ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 118);
    if (httpCode > 0 && httpCode < 400)
    {
        tft.printf("Sent!              ");
    }
    else
    {
        tft.printf("Send FAIL          ");
    }
    http.end();
    delay(1000);
    // Restore 'Send to inventory' prompt
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 118);
    tft.printf("<- Send to inventory");
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(115200);

    pinMode(TFT_BACKLITE, OUTPUT);
    digitalWrite(TFT_BACKLITE, HIGH);
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    pinMode(BUTTON_SEND, INPUT_PULLDOWN);

    // Buzzer setup
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    SPI.begin();
    rfid.PCD_Init(); // Initialize RFID reader

    tft.init(135, 240); // Init ST7789 240x135
    tft.setRotation(3); // Adafruit recommends rotation 3 for landscape
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 72);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(3);
    tft.print("Scan RFID!");
}

void loop()
{
    scanRFID();
    readLoadCell();
    showOnTFT();
    Serial.printf("UID: %s  Code: %s  Type: %s  Color: %s  TrayUID: %s  Weight: %d g\n",
                  last_uid, filament_code, filament_type, filament_color, tray_uid, (int)last_weight);
    // Wait for D2 press to send (no WiFi in loop)
    if (digitalRead(BUTTON_SEND) == HIGH && strlen(filament_code) > 0) // Only send if RFID scanned
    {
        Serial.printf("Send button pressed %s\n", filament_code);
        handleSend();
        delay(500); // debounce: prevent multiple sends per press
    }
}

void readLoadCell()
{
    // Average 10 samples for stable reading
    long mv_sum = 0;
    const int samples = 20;
    for (int i = 0; i < samples; ++i)
    {
        mv_sum += analogReadMilliVolts(LOAD_CELL_PIN);
        // delay(5); // Short delay between samples
    }
    int mv = mv_sum / samples;
    float gross_weight = CAL_SLOPE * mv + CAL_INTERCEPT;
    float net_weight = gross_weight - EMPTY_SPOOL_WEIGHT;
    last_weight = (int)net_weight;
}

void showOnTFT()
{
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // White text, black background
    tft.setTextSize(3);
    tft.setCursor(0, 0);
    tft.printf("Net weight,g:");
    tft.setCursor(0, 26);
    tft.printf("    ");
    tft.setCursor(0, 26);
    tft.printf("%d", (int)last_weight);

    if (strlen(filament_code))
    {
        // Top left: filament code and weight (size 3)
        tft.setTextColor(ST77XX_WHITE);
        tft.setTextSize(3);
        tft.setCursor(0, 51);
        tft.printf("%s", filament_code);

        // Next row: color (size 2)
        tft.setCursor(0, 77);
        tft.setTextSize(2);
        tft.printf("%s", filament_color);
        // Next row: type and trayUID (size 2)
        tft.setCursor(0, 95);
        tft.setTextSize(2);
        tft.printf("%s  %s", filament_type, tray_uid_short);

        // Next row: send to inventory (size 3)
        tft.setCursor(0, 118);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(2);
        tft.printf("<- Send to inventory");
    }
}

void scanRFID()
{
    bool scanned = false;
    bool tray_missing = true;
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial())
    {
        scanned = true;
        Serial.println(F("RFID Scanned"));
        digitalWrite(LED_BUILTIN, HIGH);
        tft.fillScreen(ST77XX_BLACK);

        // UID to hex string
        char uid_buf[16] = "";
        for (byte i = 0; i < rfid.uid.size; i++)
        {
            sprintf(uid_buf + i * 2, "%02X", rfid.uid.uidByte[i]);
        }
        strncpy(last_uid, uid_buf, sizeof(last_uid) - 1);
        last_uid[sizeof(last_uid) - 1] = '\0';

        // Derive sector keys from UID
        deriveKeysFromUid(rfid.uid.uidByte, rfid.uid.size);

        MFRC522::MIFARE_Key key;
        byte buffer[18];
        byte size = sizeof(buffer);

        // --- Block 1: Use HKDF-derived Key A for sector 0 ---
        bool block1_ok = false;
        memcpy(key.keyByte, SECTOR_KEY_A[0], 6);
        if (rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 1, &key, &(rfid.uid)) == MFRC522::STATUS_OK && rfid.MIFARE_Read(1, buffer, &size) == MFRC522::STATUS_OK)
        {
            block1_ok = true;
        }
        if (block1_ok)
        {
            char variant[9], material[9];
            strncpy(variant, (char *)buffer, 8);
            variant[8] = '\0';
            strncpy(material, (char *)buffer + 8, 8);
            material[8] = '\0';
            const MaterialInfo *info = lookupMaterial(material, variant);
            if (info)
            {
                strncpy(filament_code, info->filamentCode, sizeof(filament_code) - 1);
                strncpy(filament_type, info->name, sizeof(filament_type) - 1);
                strncpy(filament_color, info->color, sizeof(filament_color) - 1);
            }
            else
            {
                strncpy(filament_code, "?", sizeof(filament_code) - 1);
                strncpy(filament_type, material, sizeof(filament_type) - 1);
                strncpy(filament_color, variant, sizeof(filament_color) - 1);
            }
        }
        // --- Block 9: Use HKDF-derived Key A for sector 2 ---
        bool block9_ok = false;
        memcpy(key.keyByte, SECTOR_KEY_A[2], 6);
        if (rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 9, &key, &(rfid.uid)) == MFRC522::STATUS_OK && rfid.MIFARE_Read(9, buffer, &size) == MFRC522::STATUS_OK)
        {
            block9_ok = true;
        }
        if (block9_ok)
        {
            for (int i = 0; i < 16; i++)
            {
                snprintf(tray_uid + i * 2, 3, "%02X", buffer[i]);
            }
            tray_uid[32] = '\0';
            strncpy(tray_uid_short, tray_uid, 6);
            tray_uid_short[6] = '\0';
            tray_missing = false;
        }

        // --- Buzzer feedback: two-tone ---
        if (tray_missing)
        {
            // High then low: tray missing
            tone(BUZZER_PIN, 1200, 120);
            delay(150);
            tone(BUZZER_PIN, 900, 120);
        }
        else
        {
            // Low then high: tray present
            tone(BUZZER_PIN, 900, 120);
            delay(150);
            tone(BUZZER_PIN, 1200, 120);
        }
        delay(150);
        noTone(BUZZER_PIN);
    }
    // Do not clear fields if no new card is present; info persists until next scan
    // Always halt and stop crypto to allow repeated scans
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    // Turn off LED after scan
    digitalWrite(LED_BUILTIN, LOW);
}
