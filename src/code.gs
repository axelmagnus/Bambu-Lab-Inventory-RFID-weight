function doGet(e) {
  // Output the content of the 'store index' tab as JSON
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName('store index');
  if (!sheet) {
    return ContentService.createTextOutput('{"error":"store index tab not found"}').setMimeType(ContentService.MimeType.JSON);
  }
  var data = sheet.getDataRange().getValues();
  var headers = data[0];
  var jsonArray = [];
  for (var i = 1; i < data.length; i++) {
    var rowObj = {};
    for (var j = 0; j < headers.length; j++) {
      rowObj[headers[j]] = data[i][j];
    }
    jsonArray.push(rowObj);
  }
  var json = JSON.stringify(jsonArray);
  return ContentService.createTextOutput(json).setMimeType(ContentService.MimeType.JSON);
}
const DEFAULT_SHEET_NAME = 'Inventory';
const IMAGES_SHEET_NAME = 'Store Index';
const TRAY_UID_COLUMN_INDEX = 8; // Column H: Tray UID for roll (also holds chip UID when tray missing)

// New: Inventory columns (1-based):
// 1: Time scanned
// 2: Filament Code
// 3: Type
// 4: Name
// 5: Filament variantId
// 6: Weight (g)
// 7: Image
// 8: Tray UID for roll

/**
 * Webhook entry: accepts JSON body with RFID scan metadata and appends to a sheet.
 */
function doPost(e) {
  try {
    if (!e || !e.postData || !e.postData.contents) {
      return jsonResponse(400, { error: 'No body' });
    }
    const payload = JSON.parse(e.postData.contents || '{}');
    console.log('doPost payload', payload.code || '', JSON.stringify(payload));

    // Accept full Store Index uploads from the scraper (bypasses CSV imports).
    if (payload && payload.action === 'uploadStoreIndex') {
      return handleStoreIndexUpload(payload);
    }

    // Lightweight status probe to debug sheet connectivity without writing data.
    if (payload && payload.action === 'status') {
      const sheetId = getSheetId();
      if (!sheetId) {
        return jsonResponse(500, { error: 'SHEET_ID not configured (set in Script Properties)' });
      }
      return jsonResponse(200, getSheetStatus(sheetId));
    }

    if (!payload.code) {
      return jsonResponse(400, { error: 'code is required' });
    }

    const sheetId = getSheetId();
    if (!sheetId) {
      return jsonResponse(500, { error: 'SHEET_ID not configured (set in Script Properties)' });
    }

    const imageRecord = getImageRecord(sheetId, payload.code);
    console.log('lookup imageRecord', payload.code || '', imageRecord ? imageRecord.imageUrl : '');

    const result = appendRow(sheetId, payload, imageRecord);
    return jsonResponse(200, { ok: true, duplicate: result.duplicate, row: result.row });
  } catch (err) {
    console.error(err);
    return jsonResponse(500, { error: String(err) });
  }
}

/**
 * Append a row to the sheet with the supplied payload.
 */
function appendRow(sheetId, data, imageRecord) {
  const ss = SpreadsheetApp.openById(sheetId);
  const sheet = ss.getSheetByName(DEFAULT_SHEET_NAME);
  if (!sheet) {
    throw new Error(`Sheet not found: ${DEFAULT_SHEET_NAME}`);
  }
  const sep = getArgSeparator(ss);
  const ts = new Date();
  const trayUid = data.trayUid || '';
  const chipUid = data.chipUid || data.uid || data.tagUid || '';

  const imageUrl = imageRecord && imageRecord.imageUrl ? imageRecord.imageUrl : (data.imageUrl || '');
  const imageCell = imageUrl ? `=IMAGE("${imageUrl}")` : '';

  const productUrl = (imageRecord && imageRecord.productUrl) || data.productUrl || '';
  const codeCell = productUrl ? `=HYPERLINK("${productUrl}"${sep}"${data.code || ''}")` : (data.code || '');

  const type = data.type || (imageRecord && imageRecord.type) || '';
  const name = data.name || (imageRecord && imageRecord.name) || '';
  const variantId = data.variantId || (imageRecord && imageRecord.variantId) || '';
  const trayCellValue = trayUid || chipUid || 'Tray ID missing';
  const weight = data.weight || '';

  // New Inventory columns:
  // 1: Time scanned
  // 2: Filament Code
  // 3: Type
  // 4: Name
  // 5: Filament variantId
  // 6: Weight (g)
  // 7: Image
  // 8: Tray UID for roll
  const row = [
    ts,                  // A: Time scanned
    codeCell,            // B: Filament Code
    type,                // C: Type
    name,                // D: Name (color / human label)
    variantId,           // E: Filament variantId
    weight,              // F: Weight (g)
    imageCell,           // G: Image
    trayCellValue        // H: Tray UID for roll (or chip UID if tray missing)
  ];

  const cleanTrayUid = trayUid && trayUid !== 'Tray ID missing' ? trayUid : '';
  const dedupeKey = cleanTrayUid || chipUid;
  if (dedupeKey) {
    const existingRow = findRowByColumn(sheet, TRAY_UID_COLUMN_INDEX, dedupeKey);
    if (existingRow) {
      console.log('duplicate tray uid, update existing row', dedupeKey, 'row', existingRow);
      sheet.getRange(existingRow, 1, 1, row.length).setValues([row]);
      return { duplicate: true, row: existingRow, updated: true };
    }
  }
  const targetRow = findFirstEmptyRow(sheet); // first empty row, filling gaps if any
  console.log('appendRow -> sheet', sheet.getName(), 'writingRow', targetRow);
  sheet.getRange(targetRow, 1, 1, row.length).setValues([row]);
  return { duplicate: false, row: targetRow };
}

/**
 * Convert manual Filament Code entries in Inventory to hyperlinks if found in Store Index.
 * If not found, fetch product URL and image from Bambu store and add to Store Index, then hyperlink.
 * Sort Store Index by code.
 */
function updateInventoryHyperlinksAndStoreIndex() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const inventory = ss.getSheetByName(DEFAULT_SHEET_NAME);
  const storeIndex = ss.getSheetByName(IMAGES_SHEET_NAME);
  if (!inventory || !storeIndex) {
    throw new Error('Inventory or Store Index sheet not found');
  }
  const invData = inventory.getDataRange().getValues();
  const storeData = storeIndex.getDataRange().getValues();
  const storeDisplay = storeIndex.getDataRange().getDisplayValues();
  const storeHeaders = storeData[0].map(h => String(h || '').trim());
  const idxStore = {
    code: storeHeaders.indexOf('Code'),
    name: storeHeaders.indexOf('Name'),
    color: storeHeaders.indexOf('Color'),
    variantId: storeHeaders.indexOf('VariantId'),
    image: storeHeaders.indexOf('Image'),
    productUrl: storeHeaders.indexOf('ProductUrl'),
    imageUrl: storeHeaders.indexOf('ImageUrl')
  };
  const sep = getArgSeparator(ss);
  // Build a map of Store Index (code,variantId) to row with productUrl (case-sensitive)
  const storeMap = {};
  const codeOnlyMap = {};
  for (let i = 1; i < storeData.length; i++) {
    const rawCodeCell = storeData[i][idxStore.code];
    const code = String(rawCodeCell || '').trim();
    const variantId = String(storeData[i][idxStore.variantId] || '').trim();
    let productUrl = storeData[i][idxStore.productUrl] || '';
    const imageUrl = storeData[i][idxStore.imageUrl] || '';
    const type = storeData[i][idxStore.name] || '';
    const name = storeData[i][idxStore.color] || '';
    if (!productUrl) {
      // Try to extract from hyperlink formula in Code column
      productUrl = extractHyperlinkUrl(rawCodeCell) || productUrl;
    }
    if (code && variantId) {
      storeMap[code + '||' + variantId] = { row: i, productUrl: productUrl, imageUrl: imageUrl };
    }
    if (code && (!codeOnlyMap[code] || (!codeOnlyMap[code].productUrl && productUrl))) {
      codeOnlyMap[code] = { productUrl, imageUrl, type, name, variantId };
    }
  }
  let addedToStore = 0;
  for (let i = 1; i < invData.length; i++) {
    let code = String(invData[i][1] || '').trim();
    let variantId = String(invData[i][4] || '').trim();
    let name = String(invData[i][3] || '').trim(); // Inventory: Name (should map to Color in Store Index)
    let type = String(invData[i][2] || '').trim(); // Inventory: Type (should map to Name in Store Index)
    let image = invData[i][6] || '';
    let cell = inventory.getRange(i+1, 2);
    let cellValue = cell.getValue();
    if (!code || !variantId) continue; // Only process if both present
    let key = code + '||' + variantId;
    let storeEntry = storeMap[key];
    // Always add a new row for a new code+variantId combination
    if (!storeEntry || !storeEntry.productUrl) {
      // Prefer existing Store Index entry with same code (even if different variant)
      const fallbackByCode = codeOnlyMap[code] || {};
      const productUrl = fallbackByCode.productUrl || '';
      const imageUrl = fallbackByCode.imageUrl || image || '';
      const fallbackType = fallbackByCode.type || type;
      const fallbackName = fallbackByCode.name || name;
      // Always store plain code and image in Store Index, but update to hyperlink if productUrl is found
      let codeCell = code;
      if (productUrl) {
        codeCell = `=HYPERLINK("${productUrl}"${sep}"${code}")`;
      }
      const imageCell = imageUrl ? `=IMAGE("${imageUrl}")` : '';
      let newRow = [codeCell, fallbackType, fallbackName, variantId, imageCell, productUrl, imageUrl];
      Logger.log(`[updateInventoryHyperlinksAndStoreIndex] Appending to Store Index: code='${code}', variantId='${variantId}', productUrl='${productUrl}', imageUrl='${imageUrl}' (fallbackByCode=${!!fallbackByCode.productUrl})`);
      storeIndex.appendRow(newRow);
      // Update storeMap for further lookups
      storeMap[key] = { row: storeIndex.getLastRow(), productUrl: productUrl, imageUrl: imageUrl };
      if (!codeOnlyMap[code]) {
        codeOnlyMap[code] = { productUrl, imageUrl, type: fallbackType, name: fallbackName, variantId };
      }
      // Update Inventory cell to hyperlink if productUrl is found
      if (productUrl && (typeof cellValue !== 'string' || !cellValue.startsWith('=HYPERLINK'))) {
        Logger.log(`[updateInventoryHyperlinksAndStoreIndex] Setting Inventory hyperlink for code='${code}', productUrl='${productUrl}'`);
        setHyperlink(cell, productUrl, code, sep);
      }
      // Backfill Inventory image if missing and we have one
      if (imageUrl && (!image || String(image).trim() === '')) {
        inventory.getRange(i + 1, 7).setFormula(`=IMAGE("${imageUrl}")`);
      }
      addedToStore++;
    } else {
      // Store Index has productUrl for this code+variantId, hyperlink if not already
      if (storeEntry.productUrl && (typeof cellValue !== 'string' || !cellValue.startsWith('=HYPERLINK')) ) {
        Logger.log(`[updateInventoryHyperlinksAndStoreIndex] Setting Inventory hyperlink for code='${code}', productUrl='${storeEntry.productUrl}' (from Store Index)`);
        setHyperlink(cell, storeEntry.productUrl, code, sep);
      }
      // Backfill Inventory image from Store Index if missing
      if (storeEntry.imageUrl && (!image || String(image).trim() === '')) {
        inventory.getRange(i + 1, 7).setFormula(`=IMAGE("${storeEntry.imageUrl}")`);
      }
    }
  }

  // After updating Store Index, ensure all Inventory codes are hyperlinked if a Store Index entry exists
  for (let i = 1; i < invData.length; i++) {
    let code = String(invData[i][1] || '').trim();
    let variantId = String(invData[i][4] || '').trim();
    let cell = inventory.getRange(i+1, 2);
    let cellValue = cell.getValue();
    if (!code || !variantId) continue;
    let key = code + '||' + variantId;
    let storeEntry = storeMap[key];
    // Try to get productUrl from Store Index if not in storeMap (in case new rows were just added)
    if ((!storeEntry || !storeEntry.productUrl) && storeIndex) {
      // Find the row in Store Index with matching code and variantId
      const storeDataRows = storeIndex.getDataRange().getValues();
      for (let j = 1; j < storeDataRows.length; j++) {
        const row = storeDataRows[j];
        const codeVal = String(row[idxStore.code] || '').trim();
        const variantVal = String(row[idxStore.variantId] || '').trim();
        const productUrl = row[idxStore.productUrl] || '';
        if (codeVal === code && variantVal === variantId && productUrl) {
          storeEntry = { row: j, productUrl };
          break;
        }
      }
    }
    if (storeEntry && storeEntry.productUrl) {
      if (typeof cellValue !== 'string' || !cellValue.startsWith('=HYPERLINK')) {
        setHyperlink(cell, storeEntry.productUrl, code, sep);
      }
    }
  }
  // Sort Store Index by the value of the Code column (A-Z), skipping the header row, using built-in sort
  if (addedToStore > 0) {
    const lastRow = storeIndex.getLastRow();
    const lastCol = storeIndex.getLastColumn();
    if (lastRow > 1) {
      // Activate the range excluding the header
      const sortRange = storeIndex.getRange(2, 1, lastRow - 1, lastCol);
      sortRange.sort({column: 1, ascending: true});
    }
  }
}

/**
 * Fetch product URL and image from Bambu store for a given filament code.
 * Returns { productUrl, imageUrl } or empty strings if not found.
 */
function fetchBambuStoreProduct(code) {
  try {
    // Example: search page for code, then parse first result
    // Adjust URL and selectors as needed for Bambu store
    var searchUrl = 'https://eu.store.bambulab.com/search?q=' + encodeURIComponent(code) + '&type=product';
    Logger.log(`[fetchBambuStoreProduct] Fetching URL: ${searchUrl}`);
    var html = UrlFetchApp.fetch(searchUrl, {muteHttpExceptions: true, followRedirects: true, validateHttpsCertificates: true}).getContentText();
      // Try to find product link in HTML (look for the first product link in the search results)
      var productMatch = html.match(/<a[^>]+href="([^"]*\/products\/[^"?#]*)"[^>]*>\s*([^<]*)\s*<\/a>/i);
      var productUrl = productMatch ? 'https://eu.store.bambulab.com' + productMatch[1] : '';
      // Try to find the product image by looking for an <img> tag near the product link (fallback: any product image)
      var imageUrl = '';
      if (productUrl) {
        // Try to find the image URL that appears before or after the product link
        var imgMatch = html.match(/<img[^>]+src="([^"]+)"[^>]*class="[^\"]*product-card-image[^\"]*"/i);
        if (imgMatch) {
          imageUrl = imgMatch[1];
        }
      }
      Logger.log(`[fetchBambuStoreProduct] code='${code}', productUrl='${productUrl}', imageUrl='${imageUrl}'`);
      return { productUrl: productUrl, imageUrl: imageUrl };
  } catch (e) {
    Logger.log(`[fetchBambuStoreProduct] error for code '${code}': ${e}`);
    return { productUrl: '', imageUrl: '' };
  }
}

function findFirstEmptyRow(sheet) {
  const lastRow = sheet.getLastRow();
  if (lastRow === 0) {
    return 1;
  }
  const colA = sheet.getRange(1, 1, lastRow, 1).getValues();
  for (let i = 0; i < colA.length; i++) {
    const cell = String(colA[i][0] || '').trim();
    if (!cell) {
      return i + 1;
    }
  }
  return lastRow + 1;
}

function findRowByColumn(sheet, columnIndex, value) {
  const lastRow = sheet.getLastRow();
  if (!lastRow) return null;
  const colValues = sheet.getRange(1, columnIndex, lastRow, 1).getValues();
  for (let i = 0; i < colValues.length; i++) {
    const cell = String(colValues[i][0] || '').trim();
    if (cell && cell === String(value).trim()) {
      return i + 1; // 1-based row
    }
  }
  return null;
}

function getSheetStatus(sheetId) {
  const ss = SpreadsheetApp.openById(sheetId);
  const sheet = ss.getSheetByName(DEFAULT_SHEET_NAME);
  if (!sheet) {
    return { sheetFound: false, sheetName: DEFAULT_SHEET_NAME };
  }
  const lastRow = sheet.getLastRow();
  const lastCol = sheet.getLastColumn();
  let sample = [];
  if (lastRow > 0 && lastCol > 0) {
    const rowsToFetch = Math.min(3, lastRow);
    sample = sheet.getRange(1, 1, rowsToFetch, Math.min(5, lastCol)).getValues();
  }
  return {
    sheetFound: true,
    sheetName: sheet.getName(),
    lastRow,
    lastCol,
    sample
  };
}

/**
 * Lookup image metadata in the Images sheet; optionally refresh from STORE_INDEX_URL when missing.
 */
function getImageRecord(sheetId, code) {
  if (!code) return null;
  const ss = SpreadsheetApp.openById(sheetId);
  const sheet = ss.getSheetByName(IMAGES_SHEET_NAME);
  let record = sheet ? findImageRow(sheet, code) : null;
  return record;
}

function findImageRow(sheet, code) {
  const range = sheet.getDataRange();
  const values = range.getValues();
  const displays = range.getDisplayValues();
  if (!values || values.length < 2) return null;
  const headers = values[0].map(h => String(h || '').trim().toLowerCase());
  const idx = {
    code: headers.indexOf('code'),
    name: headers.indexOf('name'),
    color: headers.indexOf('color'),
    material: headers.indexOf('material'),
    variantId: headers.indexOf('variantid'),
    imageUrl: headers.indexOf('imageurl'),
    productUrl: headers.indexOf('producturl')
  };
  for (let i = 1; i < values.length; i++) {
    const row = values[i];
    const rowDisp = displays[i];
    const rowCode = idx.code >= 0 ? row[idx.code] : row[0];
    const rowCodeDisp = idx.code >= 0 ? rowDisp[idx.code] : rowDisp[0];
    const codeCandidate = String(rowCodeDisp || rowCode || '').trim();
    if (codeCandidate === String(code).trim()) {
      return {
        code: codeCandidate,
        name: idx.name >= 0 ? row[idx.name] : '',
        color: idx.color >= 0 ? row[idx.color] : '',
        material: idx.material >= 0 ? row[idx.material] : '',
        variantId: idx.variantId >= 0 ? row[idx.variantId] : '',
        imageUrl: idx.imageUrl >= 0 ? row[idx.imageUrl] : '',
        productUrl: idx.productUrl >= 0 ? row[idx.productUrl] : ''
      };
    }
  }
  return null;
}

/**
 * Populate the Store Index sheet from a JSON string (array of objects with Code/Name/Color/ImageUrl).
 * Run this manually after generating data/store_index.json; paste its contents into jsonText.
 */
function importStoreIndexFromJson(jsonText) {
  if (!jsonText) {
    throw new Error('jsonText is required');
  }
  const data = JSON.parse(jsonText);
  if (!Array.isArray(data)) {
    throw new Error('jsonText must be a JSON array');
  }
  const ss = SpreadsheetApp.getActive();
  const sep = getArgSeparator(ss);
  const headers = ['Code', 'Name', 'Color', 'VariantId', 'Image', 'ProductUrl', 'ImageUrl'];
  const sheet = ss.getSheetByName(IMAGES_SHEET_NAME) || ss.insertSheet(IMAGES_SHEET_NAME);
  // Read existing data
  const existing = sheet.getDataRange().getValues();
  const headerMap = (existing.length > 0) ? existing[0].map(h => String(h || '').trim().toLowerCase()) : headers.map(h => h.toLowerCase());
  const idx = {
    code: headerMap.indexOf('Code'),
    variantId: headerMap.indexOf('VariantId')
  };
  console.log(`[DEBUG] idx mapping:`, JSON.stringify(idx));
  
  // Build a map of existing codes to row index and variantId, using display values for code (handles formulas)
  const displays = sheet.getDataRange().getDisplayValues();
  const codeRowMap = {};
  for (let i = 1; i < existing.length; i++) {
    const rowDisp = displays[i];
    const codeDisp = idx.code >= 0 ? String(rowDisp[idx.code] || '').trim() : '';
    const variantId = idx.variantId >= 0 ? String(rowDisp[idx.variantId] || '').trim() : '';
    if (codeDisp) {
      codeRowMap[codeDisp] = { rowIdx: i, variantId: variantId };
    }
  }
  // Prepare new/updated rows
  let updates = [];
  let added = 0;
  let updated = 0;
  for (let item of data) {
    const code = String(item.code || '').trim();
    const variantId = String(item.variantId || '').trim();
    const imageUrl = item.imageUrl || '';
    const productUrl = item.productUrl || '';
    const codeCell = productUrl ? `=HYPERLINK("${productUrl}"${sep}"${code}")` : code;
    const imageCell = imageUrl ? `=IMAGE("${imageUrl}")` : '';
    const rowArr = [
      codeCell,
      item.name || '',
      item.color || '',
      variantId,
      imageCell,
      productUrl,
      imageUrl
    ];
    if (code in codeRowMap) {
      const prevVariantId = codeRowMap[code].variantId;
      console.log(`[DEBUG] Matching code: '${code}' | Existing variantId: '${prevVariantId}' | New variantId: '${variantId}'`);
      if ((variantId && (!prevVariantId || variantId.length > prevVariantId.length))) {
        sheet.getRange(codeRowMap[code].rowIdx + 1, idx.variantId + 1).setValue(variantId);
        updated++;
        console.log(`[DEBUG] Updated variantId for code '${code}' to '${variantId}' at row ${codeRowMap[code].rowIdx + 1}`);
      } else {
        console.log(`[DEBUG] Skipped update for code '${code}': existing variantId is more complete or new is empty.`);
      }
    } else {
      updates.push(rowArr);
      added++;
      console.log(`[DEBUG] Added new filament code: '${code}' with variantId: '${variantId}'`);
    }
  }
  // If sheet is empty, write headers
  if (existing.length === 0) {
    sheet.getRange(1, 1, 1, headers.length).setValues([headers]);
  }
  // Append new filaments
  if (updates.length) {
    sheet.getRange(sheet.getLastRow() + 1, 1, updates.length, headers.length).setValues(updates);
  }
  // Optionally, report summary
  console.log(`importStoreIndexFromJson: ${added} new filaments added, ${updated} variantId updated.`);
}

/**
 * Prompt-driven import: asks for either the raw JSON contents or a Drive file URL/ID for store_index.json.
 * Allows non-technical users to populate Store Index without handling formulas manually.
 */
function importStoreIndexInteractive() {
  const ui = SpreadsheetApp.getUi();
  const res = ui.prompt(
    'Import store_index.json',
    'Paste the JSON contents from data/store_index.json, or provide a Drive file URL/ID where you uploaded store_index.json.',
    ui.ButtonSet.OK_CANCEL
  );
  if (res.getSelectedButton() !== ui.Button.OK) {
    return;
  }
  const input = res.getResponseText();
  if (!input) {
    ui.alert('No input provided');
    return;
  }
  const jsonText = resolveJsonInput(input);
  importStoreIndexFromJson(jsonText);
  ui.alert('Store Index imported');
}

/**
 * Import from a Drive file ID (bypasses UI prompt).
 * Usage: importStoreIndexFromDrive('your-file-id');
 */
function importStoreIndexFromDrive(fileId) {
  if (!fileId) {
    throw new Error('fileId is required');
  }
  const file = DriveApp.getFileById(fileId);
  const jsonText = file.getBlob().getDataAsString();
  importStoreIndexFromJson(jsonText);
}

/**
 * Handle direct Store Index uploads from the scraper via POST.
 * Expects lowercase keys: { action: 'uploadStoreIndex', records: [ { code, name, color, variantId, imageUrl, productUrl } ] }
 */
function handleStoreIndexUpload(payload) {
  const records = Array.isArray(payload.records) ? payload.records : [];
  if (!records.length) {
    return jsonResponse(400, { error: 'no records' });
  }
  const sheetId = getSheetId();
  if (!sheetId) {
    return jsonResponse(500, { error: 'SHEET_ID not configured (set in Script Properties)' });
  }
  const ss = SpreadsheetApp.openById(sheetId);
  const sheet = ss.getSheetByName(IMAGES_SHEET_NAME) || ss.insertSheet(IMAGES_SHEET_NAME);
  const sep = getArgSeparator(ss);
  const headers = ['Code', 'Name', 'Color', 'VariantId', 'Image', 'ProductUrl', 'ImageUrl'];
  const rows = records.map(r => {
    const imageUrl = r.imageUrl || '';
    const productUrl = r.productUrl || '';
    const code = r.code || '';
    const codeCell = productUrl ? `=HYPERLINK("${productUrl}"${sep}"${code}")` : code;
    const imageCell = imageUrl ? `=IMAGE("${imageUrl}")` : '';
    const variantId = r.variantId || '';
    return [
      codeCell,
      r.name || '',
      r.color || '',
      variantId,
      imageCell,
      productUrl,
      imageUrl
    ];
  });
  sheet.clearContents();
  sheet.getRange(1, 1, 1, headers.length).setValues([headers]);
  sheet.getRange(2, 1, rows.length, headers.length).setValues(rows);
  return jsonResponse(200, { ok: true, rows: rows.length });
}

function resolveJsonInput(input) {
  const trimmed = input.trim();
  // If it looks like JSON array/object, return as-is.
  if ((trimmed.startsWith('[') && trimmed.endsWith(']')) || (trimmed.startsWith('{') && trimmed.endsWith('}'))) {
    return trimmed;
  }
  // Try to extract Drive file ID from URL or plain ID.
  const idMatch = trimmed.match(/[-\w]{25,}/);
  if (!idMatch) {
    throw new Error('Input is neither JSON nor a recognizable Drive file ID/URL');
  }
  const fileId = idMatch[0];
  const file = DriveApp.getFileById(fileId);
  return file.getBlob().getDataAsString();
}

/**
 * Import when JSON is pasted into a sheet cell (e.g., tab "Store Index Source", cell A1).
 * This avoids UI prompts and Drive file IDs.
 */
function importStoreIndexFromSheetCell(sourceSheetName = 'Store Index Source', cellA1 = 'A1') {
  const ss = SpreadsheetApp.getActive();
  const sheet = ss.getSheetByName(sourceSheetName);
  if (!sheet) {
    throw new Error(`Source sheet not found: ${sourceSheetName}`);
  }
  const jsonText = String(sheet.getRange(cellA1).getValue() || '').trim();
  if (!jsonText) {
    throw new Error(`No JSON found in ${sourceSheetName}!${cellA1}`);
  }
  importStoreIndexFromJson(jsonText);
}

function getSheetId() {
  return PropertiesService.getScriptProperties().getProperty('SHEET_ID');
}

function getArgSeparator(spreadsheet) {
  try {
    const locale = (spreadsheet && spreadsheet.getSpreadsheetLocale && spreadsheet.getSpreadsheetLocale()) || '';
    if (locale && locale.match(/^(cs|da|de|es|fi|fr|it|nl|no|pl|pt|ru|sv|tr|hu|ro|sk|sl|hr|sr|bg|uk|et|lv|lt|is|el|he)/i)) {
      return ';';
    }
  } catch (err) {
    console.warn('arg-separator fallback to comma', err);
  }
  return ',';
}

function jsonResponse(_status, body) {
  return ContentService.createTextOutput(JSON.stringify(body))
    .setMimeType(ContentService.MimeType.JSON);
}

// Add top-level menu in Sheets
function onOpen() {
  SpreadsheetApp.getUi()
    .createMenu('Inventory')
    .addItem('Update links', 'updateInventoryHyperlinksAndStoreIndex')
    .addToUi();
}

// Extract URL from =HYPERLINK("url","text") or =HYPERLINK("url";"text")
function extractHyperlinkUrl(cellVal) {
  if (!cellVal) return '';
  const str = String(cellVal);
  if (!str.toUpperCase().startsWith('=HYPERLINK')) return '';
  const match = str.match(/=HYPERLINK\(["']([^"']+)["']/i);
  return match ? match[1] : '';
}

// Apply hyperlink formula and enforce visible hyperlink styling
function setHyperlink(cell, url, text, sep) {
  cell.setFormula(`=HYPERLINK("${url}"${sep}"${text}")`);
  cell.setFontColor('#1155cc');
  cell.setFontLine('underline');
}
