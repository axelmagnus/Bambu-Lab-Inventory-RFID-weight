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
        // Name/material
        if (obj.containsKey("material"))
            info.materialId = obj["material"].as<String>();
        else if (obj.containsKey("Name"))
            info.materialId = obj["Name"].as<String>();
        else
            info.materialId = "";

        // VariantId/variantId
        String rawVariantId = "";
        if (obj.containsKey("variantId"))
            rawVariantId = obj["variantId"].as<String>();
        else if (obj.containsKey("VariantId"))
            rawVariantId = obj["VariantId"].as<String>();
        rawVariantId.toLowerCase();
        info.variantId = rawVariantId;

        // Filament code: filamentCode, Code, code
        if (obj.containsKey("filamentCode"))
            info.filamentCode = obj["filamentCode"].as<String>();
        else if (obj.containsKey("Code"))
            info.filamentCode = String(obj["Code"].as<int>()); // handle int code
        else if (obj.containsKey("code"))
            info.filamentCode = obj["code"].as<String>();
        else
            info.filamentCode = "";

        // Name (again, for display)
        if (obj.containsKey("material"))
            info.name = obj["material"].as<String>();
        else if (obj.containsKey("Name"))
            info.name = obj["Name"].as<String>();
        else
            info.name = "";

        // Color
        if (obj.containsKey("color"))
            info.color = obj["color"].as<String>();
        else if (obj.containsKey("Color"))
            info.color = obj["Color"].as<String>();
        else
            info.color = "";

        // ProductUrl
        if (obj.containsKey("productUrl"))
            info.productUrl = obj["productUrl"].as<String>();
        else if (obj.containsKey("ProductUrl"))
            info.productUrl = obj["ProductUrl"].as<String>();
        else
            info.productUrl = "";

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
    String searchVariant = variantId ? String(variantId) : "";
    searchVariant.toLowerCase();
    for (const auto &mat : loadedMaterials)
    {
        if (mat.variantId == searchVariant)
        {
            return &mat;
        }
    }
    return nullptr;
}
