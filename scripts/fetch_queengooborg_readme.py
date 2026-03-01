#!/usr/bin/env python3
"""
Download the latest README.md from queengooborg/Bambu-Lab-RFID-Library, parse filament table, and save as queengooborg.json.
Then use this to supplement store_index.json generation.
"""
import json
import re
import requests
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
README_URL = "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/README.md"
OUT_JSON = ROOT / "data" / "queengooborg.json"


def fetch_readme(url=README_URL):
    resp = requests.get(url, timeout=30)
    resp.raise_for_status()
    return resp.text


def parse_table(readme_text):
    # Improved parser: find markdown tables with correct headers
    lines = readme_text.splitlines()
    materials = []
    current_category = None
    in_table = False
    headers = []
    for i, line in enumerate(lines):
        if line.startswith('#### '):
            current_category = line[5:].strip()
        # Detect table header
        if re.match(r'\|\s*Color\s*\|\s*Filament Code\s*\|\s*Variant ID\s*\|', line):
            in_table = True
            headers = [h.strip() for h in line.strip('|').split('|')]
            continue
        # Skip separator row
        if in_table and re.match(r'\|\s*-+\s*\|', line):
            continue
        # Parse table rows
        if in_table and line.strip().startswith('|'):
            parts = [p.strip() for p in line.strip('|').split('|')]
            if len(parts) >= 4 and parts[1].isdigit():
                color = parts[0]
                code = parts[1]
                variant = parts[2]
                variant = variant if variant and variant != '?' else ''
                materials.append({
                    'color': color,
                    'filamentCode': code,
                    'variantId': variant,
                    'category': current_category or ''
                })
            continue
        # End of table
        if in_table and not line.strip().startswith('|'):
            in_table = False
    return materials


def main():
    readme_text = fetch_readme()
    materials = parse_table(readme_text)
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with OUT_JSON.open("w", encoding="utf-8") as fh:
        json.dump(materials, fh, ensure_ascii=False, indent=2)
    print(f"Saved {len(materials)} entries to {OUT_JSON}")

if __name__ == "__main__":
    main()
