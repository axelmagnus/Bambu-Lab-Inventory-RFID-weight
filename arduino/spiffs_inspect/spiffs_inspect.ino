// SPIFFS File Inspector for ESP32
#include <SPIFFS.h>

void setup()
{
    Serial.begin(115200);
    delay(1000);
    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    File file = SPIFFS.open("/materials.json", "r");
    if (!file)
    {
        Serial.println("File not found!");
        return;
    }
    Serial.println("--- /materials.json contents ---");
    while (file.available())
    {
        Serial.write(file.read());
    }
    file.close();
    Serial.println("\n--- End of file ---");
}

void loop()
{
    // Nothing to do here
}
