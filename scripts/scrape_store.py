import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional
from urllib.parse import urlparse, urlunparse

import requests
from bs4 import BeautifulSoup
from dotenv import load_dotenv


# Load environment variables from secrets.env
load_dotenv(dotenv_path=Path(__file__).resolve().parents[1] / "scripts" / "secret.env")


# --- Globals and config ---
ROOT = Path(__file__).resolve().parents[1]
STORE_BASE = os.environ.get("STORE_BASE", "https://store.bambulab.com")
COLLECTION_PATH = "/collections/bambu-lab-3d-printer-filament"
PUSH_URL = os.environ.get("WEB_APP_URL")
TAB_JSON = ROOT / "data" / "store_index_tab.json"
OUT_JSON = ROOT / "data" / "store_index.json"
QUEEN_JSON = ROOT / "data" / "queengooborg.json"
MISSING_JSON = ROOT / "data" / "collection_missing_urls.json"


# --- HTTP helpers ---
def fetch(url: str) -> str:
    resp = requests.get(url, timeout=30)
    resp.raise_for_status()
    return resp.text


def normalize_product_url(url: Optional[str]) -> str:
    if not url:
        return ""
    base = urlparse(STORE_BASE)
    parsed = urlparse(url)
    if not parsed.netloc:
        return f"{STORE_BASE.rstrip('/')}/{url.lstrip('/')}"
    return urlunparse((base.scheme or parsed.scheme or "https", base.netloc, parsed.path, parsed.params, parsed.query, parsed.fragment))


# --- Parsing helpers ---
def parse_colors_from_page(page_html: str) -> List[Any]:
    """Parse color options from a Bambu Lab product page."""
    soup = BeautifulSoup(page_html, "html.parser")
    options: List[Any] = []
    for swatch in soup.select('[data-variant-code]'):
        code = swatch.get('data-variant-code', '').strip()
        color = swatch.get('title', '').strip() or swatch.text.strip()
        Option = type('Option', (), {})
        opt = Option()
        opt.code = code
        opt.color = color
        options.append(opt)
    return options


def parse_product_list(html: str) -> List["Product"]:
    """Parse the filament collection page into Product entries."""
    soup = BeautifulSoup(html, "html.parser")
    products: List[Product] = []
    for prod in soup.select('a.product-item'):
        name = prod.get('title', '').strip() or prod.select_one('.product-title').text.strip() if prod.select_one('.product-title') else ''
        slug = prod.get('href', '').split('/')[-1]
        product_url = prod.get('href', '')
        media_files: List[str] = []
        img = prod.select_one('img')
        if img and img.get('src'):
            media_files.append(img['src'])
        products.append(Product(name=name, slug=slug, color_list=None, product_url=product_url, media_files=media_files))
    return products


# --- Product dataclass ---
@dataclass
class Product:
    name: str = ""
    slug: str = ""
    color_list: Optional[List[dict]] = None
    product_url: str = ""
    media_files: Optional[List[str]] = None


# --- Material guessing logic ---
def guess_material(name: str, slug: str) -> str:
    target = (name or "") + " " + (slug or "")
    target = target.lower()
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


# --- Targeted scraping ---
def scrape_product_pages(urls: List[str], variant_hints: Dict[str, Dict[str, str]]) -> List[dict]:
    """Fetch only the requested product pages and build records; minimizes scraping to avoid throttling."""
    records: List[dict] = []
    seen: set[str] = set()
    for raw_url in urls:
        url = normalize_product_url(raw_url)
        if not url or url in seen:
            continue
        seen.add(url)
        hint = variant_hints.get(url, {})
        try:
            page_html = fetch(url)
            time.sleep(0.3)
        except Exception as exc:  # noqa: BLE001
            print(f"WARN: failed to fetch product page {url}: {exc}", file=sys.stderr)
            if hint:
                records.append({
                    "code": hint.get("code", ""),
                    "name": hint.get("name", ""),
                    "color": "",
                    "material": guess_material(hint.get("name", ""), urlparse(url).path),
                    "variantId": hint.get("variantId", ""),
                    "imageUrl": "",
                    "productUrl": url,
                })
            continue

        options = parse_colors_from_page(page_html)
        if options:
            for opt in options:
                records.append({
                    "code": getattr(opt, "code", "") or hint.get("code", ""),
                    "name": hint.get("name", ""),
                    "color": getattr(opt, "color", ""),
                    "material": guess_material(hint.get("name", ""), urlparse(url).path),
                    "variantId": hint.get("variantId", ""),
                    "imageUrl": "",
                    "productUrl": url,
                })
        else:
            records.append({
                "code": hint.get("code", ""),
                "name": hint.get("name", ""),
                "color": "",
                "material": guess_material(hint.get("name", ""), urlparse(url).path),
                "variantId": hint.get("variantId", ""),
                "imageUrl": "",
                "productUrl": url,
            })
    return records


# --- Tab and queen helpers ---
def normalize_tab_row(row: dict) -> dict:
    out: dict = {}
    for k, v in row.items():
        lk = k.lower()
        if lk == "image":
            continue
        out[lk] = v if not isinstance(v, dict) else ""
    for f in ["code", "name", "color", "material", "variantid", "imageurl", "producturl"]:
        out.setdefault(f, "")
    return out


def load_tab_data(tab_path: Path) -> dict:
    if not tab_path.exists():
        print(f"[WARN] {tab_path} not found. Run fetch_store_index_tab.py first.")
        return {}
    tab_data = json.loads(tab_path.read_text(encoding="utf-8"))
    return {str(normalize_tab_row(entry)["code"]): normalize_tab_row(entry) for entry in tab_data}


def load_queen_data(path: Path) -> dict:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(e.get("filamentCode")): e for e in data}


def load_missing_data() -> dict:
    if not MISSING_JSON.exists():
        return {"missingProductUrls": [], "missingReferenceVariants": [], "missingQueenCodes": []}
    try:
        data = json.loads(MISSING_JSON.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return {"missingProductUrls": [], "missingReferenceVariants": [], "missingQueenCodes": []}
    for key in ["missingProductUrls", "missingReferenceVariants", "missingQueenCodes"]:
        data.setdefault(key, [])
    return data


def find_missing_variantids(tab_lookup: dict) -> List[str]:
    return [code for code, entry in tab_lookup.items() if not entry.get("variantid")]


def warn(msg: str) -> None:
    print(f"[WARN] {msg}")


# --- Fallback full scraping (unused in main) ---
def build_records(products: Iterable[Product]) -> List[dict]:
    records: List[dict] = []
    for product in products:
        if not product.slug:
            continue
        url = normalize_product_url(product.product_url) or f"{STORE_BASE}/products/{product.slug}"
        try:
            page_html = fetch(url)
            time.sleep(0.25)
        except Exception as exc:  # noqa: BLE001
            print(f"WARN: failed to fetch product page {url}: {exc}", file=sys.stderr)
            continue
        options = parse_colors_from_page(page_html)
        if not options:
            print(f"WARN: no color options found in {url}", file=sys.stderr)
            continue
        color_entries = product.color_list or []
        if len(color_entries) != len(options):
            print(f"WARN: color count mismatch for {product.name} ({len(options)} options vs {len(color_entries)} feed)", file=sys.stderr)
        image_url = ""
        if product.media_files:
            image_url = product.media_files[0]
            if not image_url.startswith("http"):
                image_url = f"{STORE_BASE.rstrip('/')}/{image_url.lstrip('/')}"
        for idx, opt in enumerate(options):
            color_entry = color_entries[idx] if idx < len(color_entries) else {}
            records.append({
                "code": getattr(opt, "code", ""),
                "name": product.name,
                "color": getattr(opt, "color", ""),
                "material": guess_material(product.name, product.slug),
                "variantId": color_entry.get("variantId", ""),
                "imageUrl": image_url,
                "productUrl": url,
            })
    return records


# --- Output helpers ---
def write_json(records: List[dict]) -> None:
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with OUT_JSON.open("w", encoding="utf-8") as fh:
        json.dump(records, fh, ensure_ascii=False, indent=2)


def push_store_index(records: List[dict]) -> None:
    payload = {"action": "uploadStoreIndex", "records": []}
    for rec in records:
        payload["records"].append({
            "code": rec.get("code") or "",
            "name": rec.get("name") or "",
            "color": rec.get("color") or "",
            "imageUrl": rec.get("imageUrl") or "",
            "productUrl": rec.get("productUrl") or "",
        })
    try:
        resp = requests.post(PUSH_URL, json=payload, timeout=30)
        resp.raise_for_status()
        print(f"Pushed {len(records)} records to Store Index via webhook")
    except Exception as exc:  # noqa: BLE001
        print(f"WARN: failed to push Store Index to webhook: {exc}", file=sys.stderr)


def save_missing_data(data: dict) -> None:
    try:
        MISSING_JSON.parent.mkdir(parents=True, exist_ok=True)
        MISSING_JSON.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception as exc:  # noqa: BLE001
        print(f"WARN: failed to write missing data: {exc}", file=sys.stderr)


# --- Main flow ---
def main() -> int:
    subprocess.run([sys.executable, str(ROOT / "scripts" / "fetch_queengooborg_readme.py")], check=True)

    tab_lookup = load_tab_data(TAB_JSON)
    queen_lookup = load_queen_data(QUEEN_JSON)
    missing_variantids = find_missing_variantids(tab_lookup)
    if missing_variantids:
        print(f"[INFO] Filaments missing VariantId in tab: {missing_variantids}")

    missing_data = load_missing_data()
    missing_refs = missing_data.get("missingReferenceVariants", [])
    missing_urls = missing_data.get("missingProductUrls", [])

    variant_hints: Dict[str, Dict[str, str]] = {}
    for ref in missing_refs:
        url = normalize_product_url(ref.get("productUrl", ""))
        if not url:
            continue
        variant_hints[url] = {
            "code": str(ref.get("code", "")),
            "name": ref.get("name", ""),
            "variantId": ref.get("variantId", ""),
        }
    target_urls = {normalize_product_url(u) for u in missing_urls if u}
    target_urls.update(variant_hints.keys())

    scraped: List[dict] = scrape_product_pages(list(target_urls), variant_hints)
    print(f"Scraped {len(scraped)} records from {len(target_urls)} targeted product pages.")

    merged = dict(tab_lookup)
    for rec in scraped:
        code = str(rec.get("code", ""))
        if not code:
            continue
        if code in merged:
            for k, v in rec.items():
                key = k.lower()
                if not merged[code].get(key) and v:
                    merged[code][key] = v
                elif merged[code].get(key) and v and merged[code][key] != v:
                    warn(f"Tab and scraped mismatch for {code} field '{key}': tab='{merged[code][key]}' scraped='{v}' (keeping tab)")
        else:
            merged[code] = {k.lower(): v for k, v in rec.items()}

    for code, q in queen_lookup.items():
        if code in merged:
            if not merged[code].get("variantid") and q.get("variantId"):
                merged[code]["variantid"] = q["variantId"]
        else:
            merged[code] = {
                "code": code,
                "name": q.get("category", ""),
                "color": q.get("color", ""),
                "material": q.get("category", ""),
                "variantid": q.get("variantId", ""),
                "imageurl": q.get("imageUrl", ""),
                "producturl": q.get("productUrl", ""),
            }

    for ref in missing_refs:
        code = str(ref.get("code", "")).strip()
        if not code:
            continue
        if code not in merged:
            merged[code] = {
                "code": code,
                "name": ref.get("name", ""),
                "color": ref.get("color", ""),
                "material": "",
                "variantid": ref.get("variantId", ""),
                "imageurl": ref.get("imageUrl", ""),
                "producturl": ref.get("productUrl", ""),
            }
        else:
            if not merged[code].get("variantid") and ref.get("variantId"):
                merged[code]["variantid"] = ref.get("variantId")
            if not merged[code].get("producturl") and ref.get("productUrl"):
                merged[code]["producturl"] = ref.get("productUrl")

    missing_queen_codes = [code for code in queen_lookup if code not in tab_lookup]
    missing_data["missingQueenCodes"] = missing_queen_codes
    save_missing_data(missing_data)

    merged_list = list(merged.values())
    write_json(merged_list)
    if PUSH_URL:
        push_store_index(merged_list)
    print(f"Wrote {len(merged_list)} records to {OUT_JSON}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())