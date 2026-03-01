#!/usr/bin/env python3
"""
Retry fetching product pages that previously failed with 429 errors.
Reads URLs from data/failed_429_urls.json and attempts to fetch each one.
Prints results and updates the file to remove successful URLs.
"""
import json
import time
import requests
from pathlib import Path

FAILED_PATH = Path(__file__).resolve().parents[1] / "data" / "failed_429_urls.json"
MAX_ATTEMPTS = 5
BACKOFF = 2.0
MAX_DELAY = 30.0

def retry_url(url):
    for attempt in range(MAX_ATTEMPTS):
        try:
            resp = requests.get(url, timeout=30, headers={"User-Agent": "Mozilla/5.0"})
            if resp.status_code == 200:
                print(f"SUCCESS: {url}")
                return True
            elif resp.status_code == 429:
                delay = min(BACKOFF ** attempt, MAX_DELAY)
                print(f"429 Rate limited: {url} (attempt {attempt+1}), sleeping {delay:.1f}s...")
                time.sleep(delay)
            else:
                print(f"ERROR {resp.status_code}: {url}")
                break
        except Exception as e:
            print(f"EXCEPTION: {url}: {e}")
            break
    return False

def main():
    if not FAILED_PATH.exists():
        print("No failed 429 URLs to retry.")
        return
    urls = json.loads(FAILED_PATH.read_text(encoding="utf-8"))
    if not urls:
        print("No failed 429 URLs to retry.")
        return
    print(f"Retrying {len(urls)} URLs...")
    remaining = []
    for url in urls:
        if not retry_url(url):
            remaining.append(url)
    FAILED_PATH.write_text(json.dumps(remaining, indent=2), encoding="utf-8")
    print(f"Done. {len(remaining)} URLs still failed.")

if __name__ == "__main__":
    main()
