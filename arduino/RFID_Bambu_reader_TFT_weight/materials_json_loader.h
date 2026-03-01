// Helper for loading materials.json from SPIFFS
#include <SPIFFS.h>
#include <ArduinoJson.h>

struct MaterialEntry
{
    String material;
    String color;
    String filamentCode;
    String variantId;
    String materialId;
};

#define MAX_MATERIALS 256
MaterialEntry materials[MAX_MATERIALS];
size_t materialCount = 0;

bool loadMaterialsJson(const char *path = "/materials.json")
{
    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS mount failed");
        return false;
    }
    File file = SPIFFS.open(path, "r");
    if (!file)
    {
        Serial.println("Failed to open materials.json");
        return false;
    }
    DynamicJsonDocument doc(128 * 1024);
    DeserializationError err = deserializeJson(doc, file);
    if (err)
    {
        Serial.print("JSON parse error: ");
        Serial.println(err.c_str());
        file.close();
        return false;
    }
    materialCount = 0;
    for (JsonObject obj : doc.as<JsonArray>())
    {
        if (materialCount >= MAX_MATERIALS)
            break;
        materials[materialCount].material = obj["material"].as<String>();
        materials[materialCount].color = obj["color"].as<String>();
        materials[materialCount].filamentCode = obj["filamentCode"].as<String>();
        materials[materialCount].variantId = obj["variantId"].as<String>();
        materials[materialCount].materialId = obj["materialId"].as<String>();
        materialCount++;
    }
    file.close();
    Serial.printf("Loaded %zu materials from JSON\n", materialCount);
    return true;
}

// Lookup by filamentCode
const MaterialEntry *findMaterialByCode(const String &code)
{
    for (size_t i = 0; i < materialCount; ++i)
    {
        if (materials[i].filamentCode == code)
            return &materials[i];
    }
    return nullptr;
}
