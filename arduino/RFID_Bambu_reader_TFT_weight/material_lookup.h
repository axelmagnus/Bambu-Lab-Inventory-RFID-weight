#pragma once

#include <stddef.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <vector>

struct MaterialInfo
{
    String materialId;
    String variantId;
    String filamentCode;
    String name;
    String color;
    String productUrl;
};

// Holds all loaded materials
static std::vector<MaterialInfo> loadedMaterials;

// Load materials.json from SPIFFS into loadedMaterials
inline bool loadMaterialsFromSPIFFS(const char *jsonPath = "/materials.json")
{
    loadedMaterials.clear();
    File file = SPIFFS.open(jsonPath, "r");
    if (!file)
    {
        Serial.println("Failed to open materials.json from SPIFFS");
        return false;
    }
    DynamicJsonDocument doc(128 * 1024); // Adjust size as needed
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err)
    {
        Serial.print("Failed to parse materials.json: ");
        Serial.println(err.c_str());
        return false;
    }
    for (JsonObject obj : doc.as<JsonArray>())
    {
        MaterialInfo info;
        info.materialId = obj["materialId"].as<String>();
        info.variantId = obj["variantId"].as<String>();
        info.filamentCode = obj["filamentCode"].as<String>();
        info.name = obj["material"].as<String>();
        info.color = obj["color"].as<String>();
        info.productUrl = obj["productUrl"].as<String>();
        loadedMaterials.push_back(info);
    }
    Serial.printf("Loaded %d materials from SPIFFS\n", loadedMaterials.size());
    return true;
}

// Lookup by filament code
inline const MaterialInfo *lookupMaterial(const char *filamentCode)
{
    for (const auto &mat : loadedMaterials)
    {
        if (mat.filamentCode == filamentCode)
        {
            return &mat;
        }
    }
    return nullptr;
}

// Lookup by materialId and variantId
inline const MaterialInfo *lookupMaterial(const char *materialId, const char *variantId)
{
    for (const auto &mat : loadedMaterials)
    {
        if (mat.materialId == materialId && mat.variantId == variantId)
        {
            return &mat;
        }
    }
    // Fallback: match materialId with blank variantId
    for (const auto &mat : loadedMaterials)
    {
        if (mat.materialId == materialId && mat.variantId.length() == 0)
        {
            return &mat;
        }
    }
    return nullptr;
}
