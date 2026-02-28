import json
import re
from pathlib import Path

# Load READMEqueen.md
readme_path = Path("READMEqueen.md")
with readme_path.open("r", encoding="utf-8") as f:
    readme = f.read()

# Extract all material table rows (Color, Filament Code, Variant ID, Status)
material_pattern = re.compile(r"\|\s*([^|]+?)\s*\|\s*([0-9A-Za-z/ ]+)\s*\|\s*([A-Za-z0-9\-/?(). ]*)\s*\|\s*([✅❌⚠️⏳]+)\s*\|", re.MULTILINE)
readme_materials = set()
for match in material_pattern.finditer(readme):
    color, code, variant, status = match.groups()
    code = code.strip()
    if code and code != "Filament Code":
        readme_materials.add((code, color.strip(), variant.strip()))

# Load materials.json
json_path = Path("arduino/RFID_Bambu_reader_TFT_weight/generated/materials.json")
with json_path.open("r", encoding="utf-8") as f:
    materials_json = json.load(f)

json_materials = set()
for entry in materials_json:
    code = entry.get("filamentCode", "").strip()
    color = entry.get("color", "").strip()
    variant = entry.get("variantId", "").strip()
    if code:
        json_materials.add((code, color, variant))

# Compare
missing_in_json = readme_materials - json_materials
extra_in_json = json_materials - readme_materials


# Print all unique (code, color, variant) from README, using blank variant if variant is '?' or missing
print("\nAll materials from README (for use in scripts):")
for code, color, variant in sorted(readme_materials):
    v = variant if variant and variant != '?' else ''
    print(f"{code} | {color} | {v}")
