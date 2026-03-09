#!/usr/bin/env python3
"""
Merge store_index_tab.json (tab) and store_index.json (local), filling missing fields and warning on mismatches.
Outputs merged store_index.json and store_index_tab.tsv.
"""
import json
import csv
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[1]
TAB_JSON = ROOT / "data" / "store_index_tab.json"
LOCAL_JSON = ROOT / "data" / "store_index.json"
TSV_OUT = ROOT / "data" / "store_index_tab.tsv"
MERGED_JSON = ROOT / "data" / "store_index.json"

FIELDS = [
    "code", "name", "color", "material", "variantId", "imageUrl", "productUrl"
]


def load_json(path):
    if not path.exists():
        return []
    with open(path, encoding="utf-8") as f:
        return json.load(f)

def warn(msg):
    print(f"[WARN] {msg}")

def normalize_tab_row(row):
    # Map capitalized tab keys to lowercase, flatten Image
    out = {}
    for k, v in row.items():
        lk = k.lower()
        if lk == "image":
            out["imageUrl"] = ""  # Ignore formula/image cell
        else:
            out[lk] = v if not isinstance(v, dict) else ""
    # Ensure all expected fields
    for f in FIELDS:
        if f not in out:
            out[f] = ""
    return out

def merge(tab, local):
    merged = {}
    local_by_code = {r["code"]: r for r in local}
    tab_by_code = {normalize_tab_row(r)["code"]: normalize_tab_row(r) for r in tab}
    all_codes = {str(c) for c in local_by_code} | {str(c) for c in tab_by_code}
    for code in sorted(all_codes):
        l = local_by_code.get(code, {})
        t = tab_by_code.get(code, {})
        merged_row = {}
        for f in FIELDS:
            lv = l.get(f, "")
            tv = t.get(f, "")
            if lv and tv and lv != tv:
                warn(f"Mismatch for {code} field '{f}': local='{lv}' tab='{tv}' (using tab)")
            merged_row[f] = tv or lv
        merged[code] = merged_row
    return list(merged.values())

def write_tsv(rows, path):
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in FIELDS})

def main():
    tab = load_json(TAB_JSON)
    local = load_json(LOCAL_JSON)
    merged = merge(tab, local)
    # Write merged JSON
    with open(MERGED_JSON, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2, ensure_ascii=False)
    print(f"Wrote merged store_index.json with {len(merged)} records.")
    # Write TSV
    write_tsv(merged, TSV_OUT)
    print(f"Wrote store_index_tab.tsv with {len(merged)} records.")

if __name__ == "__main__":
    main()
