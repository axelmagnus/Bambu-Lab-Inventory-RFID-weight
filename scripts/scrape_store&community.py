#!/usr/bin/env python3
"""
Sync all filament data sources (tab, store, Queen README), merge into union, update outputs, and push to tab (Google Sheet) via Apps Script Web App.

Outputs:
- data/filament.json (structured payload)
- data/filament.csv (sheet-friendly flat table)
- Updates the tab (store_index) in Google Sheets with any missing codes from the union.
"""
import argparse
import csv
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple
from urllib.parse import urlparse, urlunparse

import requests
from bs4 import BeautifulSoup

ROOT = Path(__file__).resolve().parents[1]
SECRETS_ENV = ROOT / "scripts" / "secret.env"
TAB_JSON = ROOT / "data" / "store_index_tab.json"
STORE_SCRAPE_JSON = ROOT / "data" / "store_index.json"
QUEEN_JSON = ROOT / "data" / "queengooborg.json"
FILAMENT_JSON = ROOT / "data" / "filament.json"
FILAMENT_CSV = ROOT / "data" / "filament.csv"
README_URL = "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/README.md"
STORE_BASE = os.environ.get("STORE_BASE", "https://store.bambulab.com")
COLLECTION_PATH = os.environ.get("STORE_COLLECTION_PATH", "/collections/bambu-lab-3d-printer-filament")
COLLECTION_URL = f"{STORE_BASE.rstrip('/')}{COLLECTION_PATH}"


def load_local_env(env_path: Path) -> None:
    if not env_path.exists():
        return
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val


def fetch_tab() -> List[Dict[str, str]]:
    fetch_url = os.environ.get("WEB_APP_URL")
    print("[INFO] Fetching tab from Apps Script...")
    if not fetch_url:
        raise RuntimeError("WEB_APP_URL is not set; populate scripts/secret.env")
    params = {"action": "fetchStoreIndex"}
    resp = requests.get(fetch_url, params=params, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    print(f"[INFO] Tab fetched: {len(data)} rows.")
    TAB_JSON.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
    return data


def fetch_queen() -> List[Dict[str, str]]:
    print(f"[INFO] Fetching Queen README from {README_URL} ...")
    resp = requests.get(README_URL, timeout=30)
    resp.raise_for_status()
    readme_text = resp.text
    records = parse_queen_table(readme_text)
    print(f"[INFO] Queen README parsed: {len(records)} records.")
    QUEEN_JSON.write_text(json.dumps(records, indent=2, ensure_ascii=False), encoding="utf-8")
    return records


def normalize_product_url(url: Optional[str]) -> str:
    if not url:
        return ""
    base = urlparse(STORE_BASE)
    parsed = urlparse(url)
    if not parsed.netloc:
        return f"{STORE_BASE.rstrip('/')}/{url.lstrip('/')}"
    return urlunparse((base.scheme or parsed.scheme or "https", base.netloc, parsed.path, parsed.params, parsed.query, parsed.fragment))


def fetch_html(url: str, retries: int = 2, delay: float = 1.0) -> str:
    for attempt in range(retries + 1):
        try:
            resp = requests.get(url, timeout=30)
            if resp.status_code == 429:
                print(f"[ERROR] 429 Too Many Requests for {url}")
            resp.raise_for_status()
            return resp.text
        except requests.exceptions.HTTPError as e:
            if hasattr(e.response, 'status_code') and e.response.status_code == 429:
                print(f"[ERROR] 429 Too Many Requests for {url}")
            if attempt >= retries:
                raise
            time.sleep(delay)
        except Exception:
            if attempt >= retries:
                raise
            time.sleep(delay)


def parse_collection_products(html: str) -> List[str]:
    soup = BeautifulSoup(html, "html.parser")
    links: Set[str] = set()
    for anchor in soup.find_all("a", href=True):
        href = anchor.get("href", "")
        if "/products/" not in href:
            continue
        # Only keep product links that have a variant/id query param (not just the heading page)
        if ("?variant=" not in href and "?id=" not in href):
            url = normalize_product_url(href.split("?")[0])
            # Skip known multi-filament/bundle pages that will never be in queen readme
            if "beginner-s-filament-pack" in url or "bundle" in url:
                pass  # skip bundle/multi-filament page silently
            else:
                pass  # skip heading/parent product page silently
            continue
        url = normalize_product_url(href.split("?")[0])
        links.add(url)
    return sorted(links)


def parse_product_variants(html: str) -> List[Dict[str, str]]:
    soup = BeautifulSoup(html, "html.parser")
    variants: List[Dict[str, str]] = []

    # Shopify embeds variant JSON in a script tag with "variants" key.
    for script in soup.find_all("script"):
        text = script.string or ""
        if "variants" not in text or "product" not in text:
            continue
        match = re.search(r"product\s*=\s*(\{.*?\});", text, re.DOTALL)
        if not match:
            continue
        try:
            product_data = json.loads(match.group(1))
        except Exception:  # noqa: BLE001
            continue
        for variant in product_data.get("variants", []):
            code = str(variant.get("sku") or variant.get("barcode") or "").strip()
            color = str(variant.get("title") or variant.get("option1") or "").strip()
            image_url = ""
            if variant.get("featured_media") and variant["featured_media"].get("src"):
                image_url = variant["featured_media"].get("src", "")
            if not image_url and variant.get("featured_image"):
                image_url = variant["featured_image"].get("src", "")
            variants.append(
                {
                    "code": code,
                    "color": color,
                    "variantid": variant.get("sku", ""),
                    "imageurl": image_url,
                }
            )
        break

    return variants


def scrape_store_new(existing_codes: Set[str]) -> List[Dict[str, str]]:
    print(f"[INFO] Scraping store collection: {COLLECTION_URL}")
    collection_html = fetch_html(COLLECTION_URL)
    product_urls = parse_collection_products(collection_html)
    print(f"[INFO] Found {len(product_urls)} product URLs in collection.")
    new_rows: List[Dict[str, str]] = []

    for url in product_urls:
        # Only scrape if at least one variant code is missing from existing_codes
        # (i.e., not present in tab or store cache)
        product_html = fetch_html(url)
        variants = parse_product_variants(product_html)
        variant_codes = [clean_code(v.get("code", "")) for v in variants]
        # If all variant codes are already present, skip scraping this product page
        if not any(code and code not in existing_codes for code in variant_codes):
            print(f"[INFO] Skipping product page (all variants present in tab/store): {url}")
            continue
        print(f"[INFO] Scraping product page: {url}")
        material_guess = ""
        for v in variants:
            code = clean_code(v.get("code", ""))
            if not code or code in existing_codes:
                continue
            row = {
                "code": code,
                "name": "",
                "color": v.get("color", ""),
                "variantid": clean_variant_id(v.get("variantid", "")),
                "imageurl": v.get("imageurl", ""),
                "producturl": normalize_product_url(url),
                "material": material_guess,
            }
            new_rows.append(row)
            existing_codes.add(code)

    return new_rows


def parse_queen_table(readme_text: str) -> List[Dict[str, str]]:
    lines = readme_text.splitlines()
    materials: List[Dict[str, str]] = []
    current_category = ""
    in_table = False
    for line in lines:
        if line.startswith("#### "):
            current_category = line[5:].strip()
        if re.match(r"\|\s*Color\s*\|\s*Filament Code\s*\|\s*Variant ID\s*\|", line):
            in_table = True
            continue
        if in_table and re.match(r"\|\s*-+\s*\|", line):
            continue
        if in_table and line.strip().startswith("|"):
            parts = [p.strip() for p in line.strip("|").split("|")]
            if len(parts) >= 4 and parts[1].isdigit():
                materials.append(
                    {
                        "color": parts[0],
                        "filamentCode": parts[1],
                        "variantId": parts[2] if parts[2] and parts[2] != "?" else "",
                        "category": current_category,
                    }
                )
            continue
        if in_table and not line.strip().startswith("|"):
            in_table = False
    return materials


def load_json(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        return []
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        raise RuntimeError(f"Failed to read {path}: {exc}") from exc


def normalize_row(row: Dict[str, str]) -> Dict[str, str]:
    out = {k.lower(): v for k, v in row.items()}
    for field in ["code", "name", "color", "variantid", "imageurl", "producturl", "material"]:
        if field not in out:
            out[field] = ""
    # Flatten Image field from tab if present
    if "image" in row and not out.get("imageurl"):
        out["imageurl"] = row.get("image", "") if isinstance(row.get("image"), str) else ""
    return out


def clean_code(code: str) -> str:
    return str(code).strip()


def clean_variant_id(variant: str) -> str:
    if not variant:
        return ""
    token = variant.split()[0]
    token = token.split("(")[0]
    token = token.split(",")[0]
    token = token.strip()
    if len(token) > 6 and re.match(r"^[A-Z0-9]{3}-", token):
        return token[:6]
    return token


def build_queen_lookup(queen_records: List[Dict[str, str]]) -> Dict[str, str]:
    return {clean_code(r.get("filamentCode", "")): clean_variant_id(r.get("variantId", "")) for r in queen_records}


def build_store_lookup(store_records: List[Dict[str, str]]) -> Dict[str, Dict[str, str]]:
    lookup = {}
    for r in store_records:
        norm = normalize_row(r)
        code = clean_code(norm.get("code", ""))
        if code:
            lookup[code] = norm
    return lookup


def write_filament_json(path: Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8")


def write_filament_csv(path: Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = ["code", "name", "color", "material", "variantid", "producturl", "imageurl"]
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({fn: row.get(fn, "") for fn in fieldnames})


def merge_sources(
    tab_rows: List[Dict[str, str]],
    store_lookup: Dict[str, Dict[str, str]],
    queen_lookup: Dict[str, str],
) -> Tuple[List[Dict[str, str]], Dict[str, int]]:
    merged: List[Dict[str, str]] = []
    stats = {"filled_variant": 0, "filled_image": 0, "filled_product": 0, "added_from_store": 0, "added_from_queen": 0}

    # Build code sets for all sources

    tab_lookup = {clean_code(normalize_row(r).get("code", "")): normalize_row(r) for r in tab_rows}
    store_lookup_clean = {clean_code(k): v for k, v in store_lookup.items()}
    queen_lookup_clean = {clean_code(k): v for k, v in queen_lookup.items()}
    print(f"[DEBUG] Tab codes: {sorted(tab_lookup.keys())}")
    print(f"[DEBUG] Store codes: {sorted(store_lookup_clean.keys())}")
    print(f"[DEBUG] Queen codes: {sorted(queen_lookup_clean.keys())}")
    all_codes = set(tab_lookup) | set(store_lookup_clean) | set(queen_lookup_clean)
    print(f"[DEBUG] Union of all codes: {sorted(all_codes)}")

    for code in sorted(all_codes):
        # Start with tab row if present, else store, else queen
        src = tab_lookup.get(code, store_lookup_clean.get(code, {})).copy()
        store_row = store_lookup_clean.get(code, {})
        # If not in tab or store, create minimal row from queen
        if not src:
            src = {"code": code}
        # Fill from queen if missing
        if not src.get("variantid"):
            variant = queen_lookup_clean.get(code, "") or store_row.get("variantid", "")
            if variant:
                src["variantid"] = clean_variant_id(variant)
                stats["filled_variant"] += 1
        if not src.get("imageurl") and store_row.get("imageurl"):
            src["imageurl"] = store_row["imageurl"]
            stats["filled_image"] += 1
        if not src.get("producturl") and store_row.get("producturl"):
            src["producturl"] = store_row["producturl"]
            stats["filled_product"] += 1
        if not src.get("material") and store_row.get("material"):
            src["material"] = store_row["material"]
        # Fill from queen if still missing variantid
        if not src.get("variantid") and queen_lookup_clean.get(code, ""):
            src["variantid"] = clean_variant_id(queen_lookup_clean[code])
            stats["filled_variant"] += 1
        # Track source for stats
        if code not in tab_lookup and code in store_lookup_clean:
            stats["added_from_store"] += 1
        if code not in tab_lookup and code not in store_lookup_clean and code in queen_lookup_clean:
            stats["added_from_queen"] += 1
        merged.append(src)

    return merged, stats


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch tab + Queen README, optionally crawl store for new filaments, and emit filament.json/filament.csv."
    )
    parser.add_argument("--json-output", default=str(FILAMENT_JSON), help="Path for merged JSON (default: data/filament.json)")
    parser.add_argument("--csv-output", default=str(FILAMENT_CSV), help="Path for merged CSV (default: data/filament.csv)")
    parser.add_argument("--no-fetch-queen", action="store_true", help="Use existing queengooborg.json instead of fetching README")
    parser.add_argument("--no-scrape-store", action="store_true", help="Skip live store collection crawl (use cache only)")
    args = parser.parse_args()

    load_local_env(SECRETS_ENV)

    # Always refresh tab first, then operate on the local file copy
    tab_rows = fetch_tab()
    if not tab_rows:
        raise SystemExit("Tab data is empty after fetch; aborting.")

    queen_records = load_json(QUEEN_JSON) if args.no_fetch_queen else fetch_queen()
    queen_lookup = build_queen_lookup(queen_records)

    store_records = load_json(STORE_SCRAPE_JSON)
    store_lookup = build_store_lookup(store_records)

    scraped_new = []
    if not args.no_scrape_store:
        tab_codes = {clean_code(normalize_row(r).get("code", "")) for r in tab_rows}
        existing_codes = set(store_lookup.keys()) | tab_codes
        scraped_new = scrape_store_new(existing_codes)
        for row in scraped_new:
            code = clean_code(row.get("code", ""))
            if not code:
                continue
            store_lookup[code] = normalize_row(row)


    merged, stats = merge_sources(tab_rows, store_lookup, queen_lookup)

    # --- PUSH TO TAB LOGIC (from push_store_index.py) ---
    def build_payload(records):
        payload = {"action": "uploadStoreIndex", "records": []}
        for rec in records:
            payload["records"].append({
                "code": rec.get("code") or "",
                "name": rec.get("name") or "",
                "color": rec.get("color") or "",
                "variantId": rec.get("variantid") or rec.get("variantId") or "",
                "imageUrl": rec.get("imageurl") or rec.get("imageUrl") or "",
                "productUrl": rec.get("producturl") or rec.get("productUrl") or "",
            })
        return payload

    # Find codes missing from tab but present in merged
    tab_codes = {str(r.get("code")).strip() for r in tab_rows}
    merged_codes = {str(r.get("code")).strip() for r in merged}
    missing_for_tab = [r for r in merged if str(r.get("code")).strip() not in tab_codes]
    if missing_for_tab:
        print(f"[INFO] Adding {len(missing_for_tab)} missing codes to tab...")
        # Add missing codes to tab_rows and push
        updated_tab = tab_rows + missing_for_tab
        payload = build_payload(updated_tab)
        push_url = os.environ.get("WEB_APP_URL")
        if not push_url:
            print("ERROR: WEB_APP_URL is not set. Populate scripts/secret.env.", file=sys.stderr)
            return 1
        print(f"[DEBUG] Sending to {push_url}")
        for i, rec in enumerate(payload["records"][:10]):
            print(f"[DEBUG] Payload {i}: code={rec.get('code')}, variantId={rec.get('variantId')}, productUrl={rec.get('productUrl')}, imageUrl={rec.get('imageUrl')}")
        try:
            resp = requests.post(push_url, json=payload, timeout=30)
            print(f"[DEBUG] Response status: {resp.status_code}")
            print(f"[DEBUG] Response text: {resp.text}")
            resp.raise_for_status()
            print(f"Pushed {len(updated_tab)} records to Store Index via {push_url}")
        except Exception as exc:
            status = getattr(resp, "status_code", "?") if 'resp' in locals() else "?"
            text = getattr(resp, "text", "") if 'resp' in locals() else ""
            print(f"ERROR: push failed (status {status}): {exc}\n{text}", file=sys.stderr)
            return 1
    else:
        print("[INFO] No missing codes to add to tab.")

    # Filter out empty/non-filament records (all key fields empty)
    def is_real_filament(row):
        return any(str(row.get(f, "")).strip() for f in ["code", "name", "color", "variantid"])

    filtered = [row for row in merged if is_real_filament(row)]

    for row in filtered:
        if not row.get("producturl"):
            row["producturl"] = COLLECTION_URL

    json_path = Path(args.json_output)
    csv_path = Path(args.csv_output)
    write_filament_json(json_path, filtered)
    write_filament_csv(csv_path, filtered)

    missing_store_codes = []
    for row in merged:
        code_str = str(row.get("code", "")).strip()
        if code_str and code_str not in store_lookup:
            missing_store_codes.append(code_str)

    print(f"Wrote {len(merged)} rows to {json_path}")
    print(f"Wrote CSV to {csv_path}")
    print(
        "Filled variantIds: {filled_variant}, images: {filled_image}, productUrls: {filled_product}, added from store: {added_from_store}".format(
            **stats
        )
    )
    if scraped_new:
        print(f"Scraped {len(scraped_new)} new codes from store collection (not cached).")
    else:
        if args.no_scrape_store:
            print("Store scraping skipped (cache-only mode).")
        else:
            print("Scraped 0 new codes from store collection (none found).")
    if missing_store_codes:
        print(f"Codes absent from store lookup (likely queen-only or tab-only): {', '.join(sorted(missing_store_codes))}")
    print("Uses store_index.json cache plus live crawl by default; add --no-scrape-store to disable crawling.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
