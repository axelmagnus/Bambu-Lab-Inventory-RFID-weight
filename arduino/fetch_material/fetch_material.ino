// All includes at the top, no repeats
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>

const char *ssid = "2.4 MegaPlupp";
const char *password = "azazaz13";
const char *jsonUrl = "https://script.google.com/macros/s/AKfycbyTsT9C3txomAYxMfc1-pk7lRLbdDMB__pL_xTQe_vB2cmww4bMUuVNFRLvygyDsAF9kw/exec";

void setup()
{
    Serial.begin(115200);
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("Connected!");

    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS Mount Failed");
        return;
    }

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(jsonUrl);
    int httpCode = http.GET();
    if (httpCode == 200)
    {
        String payload = http.getString();
        Serial.print("Received payload length: ");
        Serial.println(payload.length());
        Serial.println("--- Received materials.json data ---");
        Serial.println(payload);
        Serial.println("--- End of received data ---");
        File file = SPIFFS.open("/materials.json", "w");
        if (file)
        {
            Serial.println("Writing to /materials.json...");
            file.print(payload);
            file.close();
            Serial.println("File written and closed.");
        }
        else
        {
            Serial.println("Failed to open /materials.json for writing!");
        }
    }
    else
    {
        Serial.printf("HTTP GET failed, error: %d\n", httpCode);
        Serial.println("HTTP GET failed!");
    }
    http.end();
}

void loop()
{
    // Nothing here for now
}