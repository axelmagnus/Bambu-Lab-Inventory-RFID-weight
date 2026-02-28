#pragma once

#include <stddef.h>

struct MaterialInfo
{
    const char *materialId;   // e.g., "GFA50"
    const char *variantId;    // e.g., "A00-K0"
    const char *filamentCode; // 5-digit code as string, e.g., "10100"
    const char *name;         // material name/category, e.g., "PLA Basic"
    const char *color;        // human color name
    const char *productUrl;   // product page URL (when known)
};

// Curated entries first, then auto-included generated list from Bambu-Lab-RFID-Library README (materialId may be blank). Extend if needed; sketch prints a fallback when missing.
static const MaterialInfo MATERIALS[] = {
// Optionally add custom overrides above (rarely needed)
#include "generated/materials_snippet.h"
};

static const size_t MATERIAL_COUNT = sizeof(MATERIALS) / sizeof(MATERIALS[0]);

// Lookup MaterialInfo by filament code (returns nullptr if not found)
inline const MaterialInfo *lookupMaterial(const char *filamentCode)
{
    for (size_t i = 0; i < MATERIAL_COUNT; ++i)
    {
        if (MATERIALS[i].filamentCode && filamentCode && strcmp(MATERIALS[i].filamentCode, filamentCode) == 0)
        {
            return &MATERIALS[i];
        }
    }
    return nullptr;
}

// Overload: lookup by materialId and variantId, with fallback to blank variantId
inline const MaterialInfo *lookupMaterial(const char *materialId, const char *variantId)
{
    // Debug: print what is being searched
    Serial.print("lookupMaterial: materialId=");
    Serial.print(materialId ? materialId : "(null)");
    Serial.print(" variantId=");
    Serial.println(variantId ? variantId : "(null)");

    // Normalize materialId: strip 'GF' prefix if present (e.g., 'GFS04' -> 'S04')
    char normId[16];
    if (materialId && strncmp(materialId, "GF", 2) == 0 && strlen(materialId) < sizeof(normId) - 1)
    {
        strncpy(normId, materialId + 2, sizeof(normId) - 1);
        normId[sizeof(normId) - 1] = '\0';
        materialId = normId;
        Serial.print("lookupMaterial: normalized materialId to ");
        Serial.println(materialId);
    }

    // First, try to match both materialId and variantId
    for (size_t i = 0; i < MATERIAL_COUNT; ++i)
    {
        if (MATERIALS[i].materialId && MATERIALS[i].variantId && materialId && variantId &&
            strcmp(MATERIALS[i].materialId, materialId) == 0 && strcmp(MATERIALS[i].variantId, variantId) == 0)
        {
            Serial.println("lookupMaterial: found exact match");
            return &MATERIALS[i];
        }
    }
    // Fallback: try to match materialId with blank variantId
    for (size_t i = 0; i < MATERIAL_COUNT; ++i)
    {
        if (MATERIALS[i].materialId && materialId &&
            strcmp(MATERIALS[i].materialId, materialId) == 0 &&
            (!MATERIALS[i].variantId || MATERIALS[i].variantId[0] == '\0'))
        {
            Serial.println("lookupMaterial: found fallback blank variantId");
            return &MATERIALS[i];
        }
    }
    // Debug: print all S04 and S04-Y0 entries for troubleshooting
    Serial.println("lookupMaterial: not found, dumping S04/S04-Y0 entries:");
    for (size_t i = 0; i < MATERIAL_COUNT; ++i)
    {
        if ((MATERIALS[i].materialId && strstr(MATERIALS[i].materialId, "S04")) ||
            (MATERIALS[i].variantId && strstr(MATERIALS[i].variantId, "S04")) ||
            (MATERIALS[i].variantId && strstr(MATERIALS[i].variantId, "S04-Y0")))
        {
            Serial.print("  [");
            Serial.print(i);
            Serial.print("] materialId=");
            Serial.print(MATERIALS[i].materialId ? MATERIALS[i].materialId : "(null)");
            Serial.print(" variantId=");
            Serial.print(MATERIALS[i].variantId ? MATERIALS[i].variantId : "(null)");
            Serial.print(" code=");
            Serial.print(MATERIALS[i].filamentCode ? MATERIALS[i].filamentCode : "(null)");
            Serial.print(" name=");
            Serial.print(MATERIALS[i].name ? MATERIALS[i].name : "(null)");
            Serial.print(" color=");
            Serial.println(MATERIALS[i].color ? MATERIALS[i].color : "(null)");
        }
    }
    return nullptr;
}
