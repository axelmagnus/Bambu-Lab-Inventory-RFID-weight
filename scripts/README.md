# Bambu Lab Inventory Scripts

## Workflow Overview

1. **Scrape Store**
   - Run `scrape_store.py` to fetch product data from the Bambu Lab store (base URL in `secret.env`).
   - The script looks up `variantid` using `readmequeen.md` (fetched from the reference repo).
   - Output: `store_index.json` (in the data folder).

2. **Push to Google Sheet**
   - Run `push_store_index.py` to upload `store_index.json` to the "store index" tab in the Google Sheet.
   - Sheet ID and credentials are set in `secret.env`.

3. **Calibrate Load Cell**
   - Upload and run `load_cell_adc_logger.ino` on your ESP32.
   - For each calibration point:
     - Place a known weight on the load cell.
     - Type the filament weight (in grams, an empty roll weighs about 250 grams) into the Serial Monitor and press Enter.
     - The sketch will save the reading and print the details.
   - When finished, type `x` in the Serial Monitor and press Enter.
   - The sketch will output all calibration points as a Python-style list of lists. Copy/paste this into a file for use with `calc_slope_from_calibration.py`.
   - Run `calc_slope_from_calibration.py` to calculate the slope/intercept for weight conversion.

## Handling Missing VariantIDs
- If a scanned filament's `variantid` is missing, update the Google Sheet's "store index" tab.
- Press "Update inventory" on the device to fetch the latest data and retry lookup.

## Script Details
- `scrape_store.py`: Scrapes store, matches variantid, writes `store_index.json`.
- `push_store_index.py`: Pushes JSON to Google Sheet tab.
- `calc_slope_from_calibration.py`: Calculates calibration slope/intercept from pasted data.

## Secrets
- Store base URL, Sheet ID, and credentials are set in `scripts/secret.env` (never commit real secrets).

## Example Usage
```bash
python scripts/scrape_store.py
python scripts/push_store_index.py
python scripts/calc_slope_from_calibration.py
```

## Updating for New Filaments
- Edit the "store index" tab in the Google Sheet.
- Press "Update inventory" on the device to fetch the latest data.
- No firmware recompile needed for new filaments.