#!/usr/bin/env python3
"""
Quick preview tool: fetch the tab via Apps Script, fetch the live filament collection page only (no product scraping), compare variant/property IDs to find what’s missing, and save missing entries to JSON.
Network: Apps Script (1x) + collection page (1x); no product page hits to avoid 429s.
"""
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Set, Tuple
from urllib.parse import urlparse, urlunparse

import requests
from bs4 import BeautifulSoup

ROOT = Path(__file__).resolve().parents[1]
SECRETS_ENV = ROOT / "scripts" / "secret.env"
TAB_JSON = ROOT / "data" / "store_index_tab.json"
REFERENCE_JSON = ROOT / "data" / "store_index.json"
MISSING_JSON = ROOT / "data" / "collection_missing_urls.json"
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
    if not fetch_url:
        raise RuntimeError("WEB_APP_URL is not set; populate scripts/secret.env")
    params = {"action": "fetchStoreIndex"}
    resp = requests.get(fetch_url, params=params, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    TAB_JSON.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
    print(data)
    return data


def load_reference_rows() -> List[Dict[str, str]]:
    if not REFERENCE_JSON.exists():
        return []
    try:
        return json.loads(REFERENCE_JSON.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return []


def normalize_tab_row(row: Dict[str, str]) -> Dict[str, str]:
    out = {k.lower(): v for k, v in row.items()}
    for field in ["code", "name", "color", "variantid", "imageurl", "producturl", "material"]:
        if field not in out:
            out[field] = ""
    if "image" in row and not out.get("imageurl"):
        out["imageurl"] = row.get("image", "") if isinstance(row.get("image"), str) else ""
    return out


def clean_code(code: str) -> str:
    return str(code).strip()


def extract_variant_from_url(url: str) -> str:
    if not url:
        return ""
    parsed = urlparse(url)
    query = parsed.query or ""
    for part in query.split("&"):
        if part.startswith("variant="):
            return part.split("=", 1)[1]
    return ""


def get_tab_variant_ids(tab_rows: List[Dict[str, str]]) -> Set[str]:
    tab_variant_ids: Set[str] = set()
    for r in tab_rows:
        norm = normalize_tab_row(r)
        vid = norm.get("variantid", "")
        if vid:
            tab_variant_ids.add(str(vid).strip())
        vid_from_url = extract_variant_from_url(norm.get("producturl", ""))
        if vid_from_url:
            tab_variant_ids.add(vid_from_url)
    return tab_variant_ids


def normalize_product_url(url: str) -> str:
    if not url:
        return ""
    base = urlparse(STORE_BASE)
    parsed = urlparse(url)
    # Drop locale segment like /en/ from the path so domains/paths compare cleanly
    path = parsed.path or ""
    parts = [p for p in path.split("/") if p]
    if parts and parts[0].lower() in {"en", "en-us", "en-gb", "en-au", "en-ca", "en-eu"}:
        parts = parts[1:]
    normalized_path = "/" + "/".join(parts)
    if not parsed.netloc:
        return f"{STORE_BASE.rstrip('/')}{normalized_path}"
    return urlunparse((base.scheme or parsed.scheme or "https", base.netloc, normalized_path, parsed.params, parsed.query, parsed.fragment))


def normalize_tab_product_url(url: str) -> str:
    if not url:
        return ""
    parsed = urlparse(normalize_product_url(url))
    return urlunparse((parsed.scheme, parsed.netloc, parsed.path, "", "", ""))


def fetch_html(url: str, retries: int = 2, delay: float = 1.0) -> str:
    for attempt in range(retries + 1):
        try:
            resp = requests.get(url, timeout=30)
            resp.raise_for_status()
            return resp.text
        except Exception:  # noqa: BLE001
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
        links.add(normalize_product_url(href.split("?")[0]))
    return sorted(links)


def parse_collection_property_ids(html: str) -> List[Tuple[str, str]]:
    """Return list of (propertyValueId, seoCode) pairs from collection page JSON blobs."""
    results: List[Tuple[str, str]] = []
    # Find blocks that contain a product seoCode and colorList with propertyValueId entries
    for match in re.finditer(r'"seoCode"\s*:\s*"([^"]+)".*?"colorList"\s*:\s*\[(.*?)\]', html, re.DOTALL):
        seo = match.group(1)
        color_block = match.group(2)
        for pid in re.findall(r'"propertyValueId"\s*:\s*"?(\d+)"?', color_block):
            results.append((pid, seo))
    return results


def fetch_collection_urls() -> List[str]:
    collection_html = fetch_html(COLLECTION_URL)
    return parse_collection_products(collection_html)


def find_missing_variants(tab_rows: List[Dict[str, str]]) -> Dict[str, List[Dict[str, str]]]:
    collection_html = fetch_html(COLLECTION_URL)
    collection_pairs = parse_collection_property_ids(collection_html)

    tab_variant_ids: Set[str] = set()
    for r in tab_rows:
        norm = normalize_tab_row(r)
        vid = norm.get("variantid", "")
        if vid:
            tab_variant_ids.add(str(vid).strip())
        vid_from_url = extract_variant_from_url(norm.get("producturl", ""))
        if vid_from_url:
            tab_variant_ids.add(vid_from_url)

    missing: Dict[str, List[Dict[str, str]]] = {}
    for pid, seo in collection_pairs:
        if pid in tab_variant_ids:
            continue
        missing.setdefault(seo, []).append({"propertyValueId": pid, "productUrl": normalize_product_url(f"/products/{seo}")})
    return missing


def main() -> int:
    load_local_env(SECRETS_ENV)

    tab_rows = fetch_tab()
    normalized_tab_rows = [normalize_tab_row(r) for r in tab_rows]
    tab_codes = {clean_code(r.get("code", "")) for r in normalized_tab_rows}
    tab_variant_ids = get_tab_variant_ids(tab_rows)

    reference_rows = load_reference_rows()
    missing_reference: List[Dict[str, str]] = []
    for r in reference_rows:
        norm = normalize_tab_row(r)
        vid = str(norm.get("variantid", "")).strip() or extract_variant_from_url(norm.get("producturl", ""))
        if not vid:
            continue
        if vid not in tab_variant_ids:
            missing_reference.append(
                {
                    "code": clean_code(norm.get("code", "")),
                    "name": norm.get("name", ""),
                    "variantId": vid,
                    "productUrl": normalize_product_url(norm.get("producturl", "")),
                }
            )

    product_urls: List[str] = []
    missing_variants = find_missing_variants(tab_rows)

    missing_urls = sorted({m["productUrl"] for m in missing_reference if m.get("productUrl")})

    print(f"Tab entries: {len(tab_rows)}")
    print("Tab (code | name | productUrl):")
    for r in sorted(normalized_tab_rows, key=lambda x: clean_code(x.get("code", ""))):
        print(f"{clean_code(r.get('code', ''))} | {r.get('name', '')} | {normalize_tab_product_url(r.get('producturl', ''))}")

    print("\nCollection product pages found: {0}".format(len(product_urls)))
    print("Product URLs missing from tab (would be scraped): {0}".format(len(missing_urls)))
    if missing_urls:
        for url in missing_urls:
            print(url)
    else:
        print("None (all collection products already present in tab).")

    print(f"\nMissing variants vs reference (store_index.json): {len(missing_reference)}")
    for entry in missing_reference:
        print(f"{entry['variantId']} | {entry['code']} | {entry['name']} | {entry['productUrl']}")

    total_missing_variants = sum(len(v) for v in missing_variants.values())
    print(f"\nMissing propertyIds vs tab (collection only): {total_missing_variants}")
    for seo, entries in sorted(missing_variants.items()):
        for entry in entries:
            print(f"{entry['propertyValueId']} | {seo} | {entry['productUrl']}")

    # Save missing URLs to JSON for downstream use
    MISSING_JSON.parent.mkdir(parents=True, exist_ok=True)
    MISSING_JSON.write_text(
        json.dumps(
            {
                "missingProductUrls": missing_urls,
                "missingPropertyIds": missing_variants,
                "missingReferenceVariants": missing_reference,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"\nWrote missing product URLs/propertyIds to {MISSING_JSON.relative_to(ROOT)}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(1)
