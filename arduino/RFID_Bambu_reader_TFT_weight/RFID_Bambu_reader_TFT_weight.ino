
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
#include <WiFiClientSecure.h>
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
#include <time.h>
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
bool scan_print_pending = false;              // Only log once per scan for readability
static unsigned long lastMaterialsUpdate = 0; // For throttling materials.json fetches
bool loadcell_print_pending = false;          // Only log load cell once per scan
// --- Global for variant_id (for lookup and update)
char variant_id[9] = "";

// --- Decoded filament info ---
char last_uid[16] = "";
char filament_code[8] = "";
char filament_type[64] = "";
char filament_color[64] = "";
char tray_uid[33] = "";
char tray_uid_short[7] = "";
float last_weight = 0;

// --- Weight constants ---
static constexpr float CAL_SLOPE = 1.725510f;
static constexpr float CAL_INTERCEPT = -1004.43f;
// Tare assumes empty spool ~247 g → mV_tare ≈ (247 - intercept) / slope ≈ 624.6 mV
static constexpr float TARE_MV = 725.25f;
static constexpr unsigned long MATERIALS_UPDATE_COOLDOWN_MS = 5UL * 60UL * 1000UL; // 5 minutes

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
    Serial.printf("WiFi: connecting to SSID '%s'...\n", WIFI_SSID);
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
    Serial.printf("WiFi: %s (status=%d, elapsed=%lu ms, RSSI=%d)\n",
                  wifi_connected ? "connected" : "failed",
                  WiFi.status(), millis() - start, wifi_connected ? WiFi.RSSI() : 0);
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(wifi_connected ? ST77XX_GREEN : ST77XX_RED);
    tft.setTextSize(2);
    tft.print(wifi_connected ? "Connected!" : "WiFi FAIL");
    delay(800);
}

// --- Send to inventory sheet ---
void handleSend()
{
    // Placeholder for future webhook logic
    wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (!wifi_connected)
        connectWiFi();
    if (!wifi_connected)
    {
        Serial.println("Send aborted: WiFi not connected");
        return;
    }
    // --- NTP Time Sync (after WiFi) ---
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Waiting for NTP time sync");
    time_t now = time(nullptr);
    int ntp_wait = 0;
    while (now < 1672531200 && ntp_wait < 20)
    { // 2023-01-01 epoch, max 20s
        delay(1000);
        Serial.print(".");
        now = time(nullptr);
        ntp_wait++;
    }
    Serial.println();
    if (now >= 1672531200)
    {
        Serial.printf("NTP time set: %s\n", ctime(&now));
    }
    else
    {
        Serial.println("NTP time sync failed!");
    }
    Serial.printf("Send: code='%s' type='%s' name='%s' variant='%s' tray='%s' uid='%s' weight=%d g\n",
                  filament_code, filament_type, filament_color, variant_id, tray_uid, last_uid, (int)last_weight);
    // Update only last row with 'Sending...'
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 118);
    tft.printf("Sending....         ");
    WiFiClientSecure client;
    client.setInsecure(); // Allow HTTPS without bundling root certs
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Follow Apps Script redirects
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    http.setReuse(false);
    String payload;
    // Always send code field, even if unknown
    payload = String("{\"code\":\"") + filament_code +
              "\",\"type\":\"" + filament_type +
              "\",\"name\":\"" + filament_color +
              "\",\"variantId\":\"" + variant_id +
              "\",\"weight\":" + String((int)last_weight) +
              ",\"trayUid\":\"" + tray_uid +
              "\",\"uid\":\"" + last_uid + "\"}";

    // Debug: print payload before sending
    Serial.println("--- HTTP Payload ---");
    Serial.println(payload);
    // Check for non-printable characters
    bool hasNonPrintable = false;
    for (size_t i = 0; i < payload.length(); ++i)
    {
        char c = payload[i];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c > 126)
        {
            hasNonPrintable = true;
            Serial.printf("Non-printable char at %d: 0x%02X\n", (int)i, (unsigned char)c);
        }
    }
    if (!hasNonPrintable)
    {
        Serial.println("No non-printable characters in payload.");
    }
    Serial.println("--------------------");
    int httpCode = -1;
    bool success = false;
    String lastResp = "";
    for (int attempt = 0; attempt < 3 && !success; ++attempt)
    {
        String url = WEB_APP_URL;
        bool useHttp10 = false;
        if (attempt == 1)
        {
            useHttp10 = true; // Retry with HTTP/1.0 (sometimes needed for proxies)
        }
        else if (attempt == 2 && url.startsWith("https://"))
        {
            url.replace("https://", "http://"); // Last resort: plain HTTP
        }
        http.begin(client, url);
        http.useHTTP10(useHttp10);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("User-Agent", "ESP32-RFID-Inventory/1.0");
        httpCode = http.POST(payload);
        Serial.printf("Send attempt %d: url=%s http10=%d code=%d wifiStatus=%d\n",
                      attempt + 1, url.c_str(), useHttp10, httpCode, WiFi.status());
        if (httpCode > 0)
        {
            success = (httpCode < 400);
            String resp = http.getString();
            lastResp = resp;
            Serial.printf("Send response: %s\n", resp.c_str());
            if (!success)
            {
                Serial.printf("Send error: HTTP %d, response: %s\n", httpCode, resp.c_str());
            }
        }
        else
        {
            lastResp = http.errorToString(httpCode);
            Serial.printf("Send error: %s\n", lastResp.c_str());
        }
        http.end();
    }
    Serial.printf("Send final: len=%d httpCode=%d success=%d\n", payload.length(), httpCode, success);

    // Show HTTP result on TFT for user feedback
    tft.setTextColor((httpCode > 0 && httpCode < 400) ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 118);
    if (httpCode > 0 && httpCode < 400)
    {
        tft.printf("Sent! (%d)         ", httpCode);
    }
    else
    {
        tft.printf("FAIL %d           ", httpCode);
        // Show first 16 chars of error/response on next line
        tft.setCursor(0, 140);
        String shortErr = lastResp.substring(0, 16);
        tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        tft.printf("%s", shortErr.c_str());
    }
    delay(1200);
    // Clear the sent/sent fail message by overwriting at the same position
    tft.setCursor(0, 118);
    tft.setTextColor(ST77XX_BLACK, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.printf("                    ");
    tft.setCursor(0, 140);
    tft.printf("                    ");
    // Immediately restore the green prompt
    tft.setCursor(0, 118);
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
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
    delay(800);
    // Clear the sent/sent fail message by overwriting at the same position
    tft.setCursor(0, 118);
    tft.setTextColor(ST77XX_BLACK, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.printf("                    ");
    // Immediately restore the green prompt
    tft.setCursor(0, 118);
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.printf("<- Update inventory");
}

void setup()
{
    // Mount SPIFFS once at startup
    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS mount failed at startup!");
    }
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

    // Mount SPIFFS and load materials.json
    if (!loadMaterialsFromSPIFFS())
    {
        Serial.println("Failed to load materials.json from SPIFFS");
    }

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
    if (scan_print_pending)
    {
        Serial.printf("UID: %s  Code: %s  Type: %s  Color: %s  TrayUID: %s  Weight: %d g\n",
                      last_uid, filament_code, filament_type, filament_color, tray_uid, (int)last_weight);
        scan_print_pending = false;
    }

    // Lookup by variantId in materials.json
    bool variant_found = false;
    if (strlen(variant_id) > 0)
    {
        for (const auto &mat : loadedMaterials)
        {
            if (mat.variantId == String(variant_id))
            {
                variant_found = true;
                break;
            }
        }
    }

    // Wait for D2 press to send (no WiFi in loop)
    if (digitalRead(BUTTON_SEND) == HIGH)
    {
        Serial.printf("Send button pressed %s\n", filament_code);
        if (!variant_found)
        {
            // Show warning and send anyway
            tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
            tft.setTextSize(2);
            tft.setCursor(0, 118);
            tft.printf("Unknown variant, sent");
            // Negative buzzer sound
            tone(BUZZER_PIN, 1200, 120);
            delay(150);
            tone(BUZZER_PIN, 900, 120);
            delay(150);
            noTone(BUZZER_PIN);
            handleSend();
            // After sending, try to update materials.json
            tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
            tft.setTextSize(2);
            tft.setCursor(0, 118);
            tft.printf("Updating materials...");
            if (updateMaterialsJsonFromWeb())
            {
                tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
                tft.setTextSize(2);
                tft.setCursor(0, 118);
                tft.printf("Materials updated");
            }
            else
            {
                tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
                tft.setTextSize(2);
                tft.setCursor(0, 118);
                tft.printf("Update failed");
            }
            delay(1000);
        }
        else
        {
            // Success feedback
            handleSend();
            tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
            tft.setTextSize(2);
            tft.setCursor(0, 118);
            tft.printf("Sent! Inventory ok");
            delay(1000);
        }
        // Restore prompt
        tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
        tft.setTextSize(2);
        tft.setCursor(0, 118);
        tft.printf("<- Update inventory");
    }
}
// --- Download materials.json from web and save to SPIFFS ---
bool updateMaterialsJsonFromWeb()
{
    // Skip if we refreshed recently
    if (lastMaterialsUpdate && millis() - lastMaterialsUpdate < MATERIALS_UPDATE_COOLDOWN_MS)
    {
        Serial.printf("materials.json: skipped (cooldown %lu ms remaining)\n",
                      MATERIALS_UPDATE_COOLDOWN_MS - (millis() - lastMaterialsUpdate));
        return true; // treat as success to avoid blocking UI
    }
    if (!wifi_connected)
    {
        connectWiFi();
    }
    if (!wifi_connected)
    {
        Serial.println("WiFi not connected, cannot update materials.json");
        return false;
    }
    wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (!wifi_connected)
    {
        Serial.println("Update materials aborted: WiFi not connected");
        return false;
    }
    Serial.println("materials.json: fetching from web app...");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    http.setReuse(false);
    int httpCode = -1;
    bool success = false;
    for (int attempt = 0; attempt < 3 && !success; ++attempt)
    {
        String url = WEB_APP_URL;
        bool useHttp10 = false;
        if (attempt == 1)
        {
            useHttp10 = true;
        }
        else if (attempt == 2 && url.startsWith("https://"))
        {
            url.replace("https://", "http://");
        }
        http.begin(client, url);
        http.useHTTP10(useHttp10);
        http.addHeader("User-Agent", "ESP32-RFID-Inventory/1.0");
        httpCode = http.GET();
        Serial.printf("materials.json attempt %d: url=%s http10=%d code=%d\n",
                      attempt + 1, url.c_str(), useHttp10, httpCode);
        if (httpCode > 0 && httpCode < 400)
        {
            success = true;
            String resp = http.getString();
            Serial.printf("materials.json response: %s\n", resp.c_str());
        }
        else if (httpCode <= 0)
        {
            Serial.printf("materials.json error: %s\n", http.errorToString(httpCode).c_str());
        }
        else
        {
            String resp = http.getString();
            Serial.printf("materials.json HTTP error %d, response: %s\n", httpCode, resp.c_str());
        }
        http.end();
    }
    if (!success)
    {
        http.end();
        // Do not hammer: set timestamp even on failure to avoid repeated long fetches in quick succession
        lastMaterialsUpdate = millis();
        return false;
    }
    // Successful code is in httpCode from last attempt; reopen to read payload
    WiFiClientSecure client2;
    client2.setInsecure();
    HTTPClient http2;
    http2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http2.setConnectTimeout(6000);
    http2.setTimeout(6000);
    http2.setReuse(false);
    http2.begin(client2, WEB_APP_URL);
    http2.useHTTP10(false);
    http2.addHeader("User-Agent", "ESP32-RFID-Inventory/1.0");
    int httpCode2 = http2.GET();
    Serial.printf("materials.json final fetch: code=%d\n", httpCode2);
    if (httpCode2 != 200)
    {
        http2.end();
        lastMaterialsUpdate = millis();
        return false;
    }
    String payload = http2.getString();
    http2.end();
    Serial.println("materials.json: payload fetched");
    File file = SPIFFS.open("/materials.json", "w");
    if (file)
    {
        file.print(payload);
        file.close();
        Serial.println("materials.json updated from web.");
        lastMaterialsUpdate = millis();
        return loadMaterialsFromSPIFFS();
    }
    else
    {
        Serial.println("Failed to open /materials.json for writing!");
        lastMaterialsUpdate = millis();
        return false;
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
        delay(5); // Short delay between samples
    }
    int mv = mv_sum / samples;
    float filament_weight = CAL_SLOPE * mv + CAL_INTERCEPT;
    last_weight = (int)filament_weight;
    if (loadcell_print_pending)
    {
        Serial.printf("LoadCell: mv=%d filament=%.1f stored=%d\n", mv, filament_weight, (int)last_weight);
        loadcell_print_pending = false;
    }
}

void showOnTFT()
{
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); // White text, black background
    tft.setTextSize(3);
    tft.setCursor(0, 0);
    tft.printf("Filament,g:");
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
        tft.printf("%s  %s", filament_code, tray_uid_short[0] ? tray_uid_short : "NoTray");

        // Next row: color (size 2)
        tft.setCursor(0, 77);
        tft.setTextSize(2);
        // Only display first 20 chars of filament_color
        char color_display[21];
        strncpy(color_display, filament_color, 20);
        color_display[20] = '\0';
        tft.printf("%s", color_display);
        // Next row: type and trayUID (size 2)
        tft.setCursor(0, 95);
        tft.setTextSize(2);
        // Only display first 20 chars of filament_type
        char type_display[21];
        strncpy(type_display, filament_type, 20);
        type_display[20] = '\0';
        tft.printf(type_display);

        // Next row: send to inventory (size 3)
        tft.setCursor(0, 118);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(2);
        tft.printf("<- Update inventory");
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
        scan_print_pending = true;
        loadcell_print_pending = true;
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
            // Set global variant_id for use in lookups
            strncpy(variant_id, variant, sizeof(variant_id) - 1);
            variant_id[sizeof(variant_id) - 1] = '\0';
            // Use unified material lookup
            const MaterialInfo *info = lookupMaterial(nullptr, variant);
            if (info)
            {
                strncpy(filament_code, info->filamentCode.c_str(), sizeof(filament_code) - 1);
                strncpy(filament_type, info->name.c_str(), sizeof(filament_type) - 1);
                strncpy(filament_color, info->color.c_str(), sizeof(filament_color) - 1);
            }
            else
            {
                strncpy(filament_code, "?", sizeof(filament_code) - 1);
                strncpy(filament_type, material, sizeof(filament_type) - 1);
                strncpy(filament_color, variant, sizeof(filament_color) - 1);
            }
            Serial.printf("RFID block1 material='%s' variant='%s' code='%s' type='%s' color='%s'\n",
                          material, variant, filament_code, filament_type, filament_color);
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
            Serial.printf("RFID tray UID=%s\n", tray_uid);
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
