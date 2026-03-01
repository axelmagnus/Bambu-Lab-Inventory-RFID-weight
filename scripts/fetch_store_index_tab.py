"""
Fetches the latest data from the Google Sheet 'store index' tab via Apps Script Web App and writes it to store_index.json.
Relies on WEB_APP_URL in scripts/secret.env.
"""
import os
import json
import sys
from pathlib import Path
import requests

ROOT = Path(__file__).resolve().parents[1]
SECRETS_ENV = ROOT / "scripts" / "secret.env"
OUTPUT_PATH = ROOT / "data" / "store_index_tab.json"

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

def main() -> int:
    load_local_env(SECRETS_ENV)
    fetch_url = os.environ.get("WEB_APP_URL")
    if not fetch_url:
        print("ERROR: WEB_APP_URL is not set. Populate scripts/secret.env.", file=sys.stderr)
        return 1

    params = {"action": "fetchStoreIndex"}
    print(f"[DEBUG] Requesting {fetch_url} with params {params}")
    try:
        resp = requests.get(fetch_url, params=params, timeout=30)
        print(f"[DEBUG] Response status: {resp.status_code}")
        print(f"[DEBUG] Response text: {resp.text[:200]}")
        resp.raise_for_status()
        json_data = resp.json()
    except Exception as exc:
        status = getattr(resp, "status_code", "?") if 'resp' in locals() else "?"
        text = getattr(resp, "text", "") if 'resp' in locals() else ""
        print(f"ERROR: fetch failed (status {status}): {exc}\n{text}", file=sys.stderr)
        return 1

    with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
        json.dump(json_data, f, indent=2, ensure_ascii=False)

    print(f"Fetched {len(json_data)} rows from Store Index and wrote to {OUTPUT_PATH}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
