#!/usr/bin/env python3
"""
Scrape the Bambu US filament storefront for code/name/color/material/variantId/imageUrl.
Outputs:
- data/store_index.json (array of objects)
- data/store_index.csv (Code,Name,Color,Material,VariantId,ImageUrl)

Usage:
    python scripts/scrape_store.py
    STORE_BASE=https://us.store.bambulab.com python scripts/scrape_store.py
"""
import csv
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional
from urllib.parse import urlparse, urlunparse

import requests
from bs4 import BeautifulSoup
# Add import for queengooborg fetch
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FAILED_429_PATH = ROOT / "data" / "failed_429_urls.json"
OUT_JSON = ROOT / "data" / "store_index.json"
SECRETS_ENV = ROOT / "scripts" / "secret.env"


def load_local_env(env_path: Path) -> None:
    """Load simple KEY=VALUE lines into os.environ if not already set."""
    if not env_path.exists():
        return
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val


load_local_env(SECRETS_ENV)
BASE_STORE = os.environ.get("STORE_BASE", "https://us.store.bambulab.com")
COLLECTION_PATH = "/collections/bambu-lab-3d-printer-filament"
PUSH_URL = os.environ.get("WEB_APP_URL")


def normalize_product_url(url: Optional[str]) -> str:
    """Ensure productUrl uses BASE_STORE host; handle relative paths gracefully."""
    if not url:
        return ""
    base = urlparse(BASE_STORE)
    parsed = urlparse(url)
    if not parsed.netloc:
        # Relative or path-only
        return f"{BASE_STORE.rstrip('/')}/{url.lstrip('/')}"
    return urlunparse((base.scheme or parsed.scheme or "https", base.netloc, parsed.path, parsed.params, parsed.query, parsed.fragment))


@dataclass
class Product:
    name: str
    slug: str
    color_list: List[dict]
    media_files: List[str]
    product_url: str


@dataclass
class ColorOption:
    color: str
    code: str
    index: int


def fetch(url: str, retries: int = 3, backoff: float = 1.5) -> str:
    """HTTP GET, logs 429, no retry or delay."""
    import sys
    resp = requests.get(url, timeout=30, headers={"User-Agent": "Mozilla/5.0"})
    if resp.status_code == 429:
        print(f"[WARN] Rate limited (429) on {url}. Not retrying.", file=sys.stderr)
        # Save failed 429 URL for later retry
        try:
            failed = []
            if FAILED_429_PATH.exists():
                failed = json.loads(FAILED_429_PATH.read_text(encoding="utf-8"))
            if url not in failed:
                failed.append(url)
                FAILED_429_PATH.write_text(json.dumps(failed, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"[ERROR] Could not save failed 429 URL: {e}", file=sys.stderr)
        resp.raise_for_status()
    elif resp.status_code in (500, 502, 503, 504):
        print(f"[WARN] Server error {resp.status_code} on {url}. Not retrying.", file=sys.stderr)
        resp.raise_for_status()
    resp.raise_for_status()
    return resp.text


def parse_product_list(html: str) -> List[Product]:
    idx = html.find("productList")
    if idx == -1:
        raise RuntimeError("productList not found in collection page")
    start = html.find("[", idx)
    level = 0
    in_str = False
    esc = False
    end = None
    for pos, ch in enumerate(html[start:], start):
        if esc:
            esc = False
            continue
        if ch == "\\":
            esc = True
            continue
        if ch == '"':
            in_str = not in_str
            continue
        if in_str:
            continue
        if ch == "[":
            level += 1
        elif ch == "]":
            level -= 1
            if level == 0:
                end = pos
                break
    if end is None:
        raise RuntimeError("could not bracket-match productList array")
    arr = html[start : end + 1]
    data = json.loads(bytes(arr, "utf-8").decode("unicode_escape"))
    products: List[Product] = []
    for item in data:
        slug = item.get("seoCode", "")
        products.append(
            Product(
                name=item.get("name", ""),
                slug=slug,
                color_list=sorted(item.get("colorList", []), key=lambda c: c.get("colorPosition", 0)),
                media_files=item.get("mediaFiles", []) or [],
                product_url=f"{BASE_STORE}/products/{slug}" if slug else "",
            )
        )
    return products


def parse_colors_from_page(html: str) -> List[ColorOption]:
    soup = BeautifulSoup(html, "html.parser")
    opts: List[ColorOption] = []
    idx = 0
    for li in soup.find_all("li"):
        val = li.get("value")
        if not val:
            continue
        m = re.match(r"^(.*) \((\d{5})\)$", val.strip())
        if not m:
            continue
        opts.append(ColorOption(color=m.group(1).strip(), code=m.group(2), index=idx))
        idx += 1
    return opts


def guess_material(name: str, slug: str) -> str:
    target = slug.lower() or name.lower()
    if "pla" in target:
        return "PLA"
    if "pet-cf" in target or "petcf" in target:
        return "PET-CF"
    if "petg" in target:
        return "PETG"
    if "paht" in target:
        return "PAHT"
    if "abs" in target:
        return "ABS"
    if "asa" in target:
        return "ASA"
    if "tpu" in target:
        return "TPU"
    if "pc" in target:
        return "PC"
    return name.split(" ")[0] if name else ""


def build_records(products: Iterable[Product]) -> List[dict]:
    records: List[dict] = []
    for product in products:
        if not product.slug:
            continue
        url = normalize_product_url(product.product_url) or f"{BASE_STORE}/products/{product.slug}"
        try:
            page_html = fetch(url)
            time.sleep(0.25)
        except Exception as exc:
            print(f"WARN: failed to fetch product page {url}: {exc}", file=sys.stderr)
            continue
        options = parse_colors_from_page(page_html)
        if not options:
            print(f"WARN: no color options found in {url}", file=sys.stderr)
            continue
        color_entries = product.color_list
        if len(color_entries) != len(options):
            print(f"WARN: color count mismatch for {product.name} ({len(options)} options vs {len(color_entries)} feed)", file=sys.stderr)
        # Extract imageUrl from product.media_files if available
        image_url = ""
        if product.media_files:
            # Use first image file as default
            image_url = product.media_files[0]
            if not image_url.startswith("http"):
                image_url = f"{BASE_STORE.rstrip('/')}/{image_url.lstrip('/')}"
        # Build records for each color option
        for idx, opt in enumerate(options):
            color_entry = color_entries[idx] if idx < len(color_entries) else {}
            records.append({
                "code": opt.code,
                "name": product.name,
                "color": opt.color,
                "material": guess_material(product.name, product.slug),
                "variantId": color_entry.get("variantId", ""),
                "imageUrl": image_url,
                "productUrl": url
            })
    return records


def write_json(records: List[dict]) -> None:
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with OUT_JSON.open("w", encoding="utf-8") as fh:
        json.dump(records, fh, ensure_ascii=False, indent=2)




def push_store_index(records: List[dict]) -> None:
    """Send the scraped records directly to the Apps Script webhook to populate Store Index."""
    payload = {"action": "uploadStoreIndex", "records": []}
    for rec in records:
        payload["records"].append(
            {
                "code": rec.get("code") or "",
                "name": rec.get("name") or "",
                "color": rec.get("color") or "",
                "imageUrl": rec.get("imageUrl") or "",
                "productUrl": rec.get("productUrl") or "",
            }
        )
    try:
        resp = requests.post(PUSH_URL, json=payload, timeout=30)
        resp.raise_for_status()
        print(f"Pushed {len(records)} records to Store Index via webhook")
    except Exception as exc:  # noqa: BLE001
        print(f"WARN: failed to push Store Index to webhook: {exc}", file=sys.stderr)


def main() -> int:
    # Download and parse latest queengooborg README
    subprocess.run([
        "python3",
        str(ROOT / "scripts" / "fetch_queengooborg_readme.py")
    ], check=True)
    # Load queengooborg.json for supplementing store index
    queengooborg_path = ROOT / "data" / "queengooborg.json"
    if queengooborg_path.exists():
        with queengooborg_path.open("r", encoding="utf-8") as fh:
            queengooborg_materials = json.load(fh)
        print(f"Loaded {len(queengooborg_materials)} entries from queengooborg.json")
    else:
        queengooborg_materials = []
        print("No queengooborg.json found!")
    collection_url = f"{BASE_STORE}{COLLECTION_PATH}"
    try:
        html = fetch(collection_url)
    except Exception as exc:
        print(f"ERROR: Failed to fetch collection page: {exc}", file=sys.stderr)
        html = ""
    try:
        products = parse_product_list(html)
        print(f"Parsed {len(products)} products from store collection page.")
    except Exception as exc:
        print(f"ERROR: Failed to parse product list: {exc}", file=sys.stderr)
        products = []
    try:
        records = build_records(products)
        print(f"Built {len(records)} records from store products.")
    except Exception as exc:
        print(f"ERROR: Failed to build records from products: {exc}", file=sys.stderr)
        records = []
    # --- PATCH: Merge variantId from queengooborg.json into store records ---
    codes_in_store = {r.get("code"): r for r in records}
    for m in queengooborg_materials:
        code = m["filamentCode"]
        variant_id = m.get("variantId", "")
        if code in codes_in_store:
            store_rec = codes_in_store[code]
            # Only update if store record is missing or has empty variantId
            if not store_rec.get("variantId") and variant_id:
                store_rec["variantId"] = variant_id
        else:
            warn_msgs = []
            if not variant_id or str(variant_id).strip() == "":
                warn_msgs.append(f"[WARN] Fallback entry for code {code} is missing variantId.")
            if not m.get("imageUrl") or not m.get("productUrl"):
                warn_msgs.append(f"[WARN] Fallback entry for code {code} is missing imageUrl/productUrl.")
            for msg in warn_msgs:
                print(msg, file=sys.stderr)
            records.append({
                "code": code,
                "name": m.get("category", ""),
                "color": m["color"],
                "material": m.get("category", ""),
                "variantId": variant_id,
                "imageUrl": m.get("imageUrl", ""),
                "productUrl": m.get("productUrl", "")
            })
    write_json(records)
    if PUSH_URL:
        push_store_index(records)
    print(f"Wrote {len(records)} records to {OUT_JSON}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
