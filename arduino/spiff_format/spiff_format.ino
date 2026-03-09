// spiff_format.ino
// Formats SPIFFS and creates an empty materials.json file on ESP32
// WARNING: Formatting SPIFFS will erase all files! Use only for recovery or first-time setup.

#include <SPIFFS.h>

void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
        ; // Wait for serial monitor connection
    }
    Serial.println("[INFO] Formatting SPIFFS...");
    if (!SPIFFS.begin(true))
    {
        Serial.println("[ERROR] SPIFFS Mount Failed");
        return;
    }
    if (SPIFFS.format())
    {
        Serial.println("[INFO] SPIFFS formatted successfully.");
        // Create empty materials.json
        File file = SPIFFS.open("/materials.json", FILE_WRITE);
        if (!file)
        {
            Serial.println("[ERROR] Failed to create materials.json");
        }
        else
        {
            file.print("[]");
            file.close();
            Serial.println("[INFO] Created empty materials.json");
        }
    }
    else
    {
        Serial.println("[ERROR] SPIFFS format failed.");
    }
}

void loop()
{
    // Nothing to do
}
