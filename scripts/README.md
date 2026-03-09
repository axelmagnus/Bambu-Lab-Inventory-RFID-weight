# Bambu Lab Inventory Scripts

## Workflow Overview

1. **Sync All Data**
   - Run `sync_all_data.py` to fetch the tab, Queen README, and store (if needed), merge all sources, update outputs, and push any missing codes to the Google Sheet tab. Sheet ID and credentials are set in `secret.env`.
   - This script replaces both `push_store_index.py` and `scrape_store.py`.

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
 If a scanned filament's `variantid` is missing, run `sync_all_data.py` to update the Google Sheet's "store index" tab. Press "Update inventory" on the device to fetch the latest data and retry lookup.

## Script Details
 `sync_all_data.py`: Syncs, merges, and pushes all sources to the Google Sheet tab.
 `scrape_store.py`: Scrapes store, matches variantid, writes `store_index.json`.
 `calc_slope_from_calibration.py`: Calculates calibration slope/intercept from pasted data.

## Secrets
- Store base URL, Sheet ID, and credentials are set in `scripts/secret.env` (never commit real secrets).

## Example Usage
```bash
python scripts/sync_all_data.py
python scripts/calc_slope_from_calibration.py
```

## Updating for New Filaments
- Edit the "store index" tab in the Google Sheet.
- Press "Update inventory" on the device to fetch the latest data.
- No firmware recompile needed for new filaments.