# SPIFFS Formatting: Usage & Warnings

**What does formatting SPIFFS do?**
- Erases all files on the ESP32 SPIFFS filesystem.
- Use only for first-time setup or if the filesystem is corrupted.

**How to use:**
1. Flash `spiff_format.ino` to your ESP32.
2. Open Serial Monitor at 115200 baud.
3. Wait for confirmation: `[INFO] SPIFFS formatted successfully.`
4. The sketch will create an empty `materials.json` file.
5. Re-flash your main application sketch after formatting.

**Important:**
- Formatting is **not** needed for normal operation or every upload.
- All files (including calibration, materials, logs) will be erased.
- Only use if you have backup or can restore required files.

**Recovery:**
- If `materials.json` is missing or SPIFFS errors occur, format SPIFFS and re-upload files.

---

**Summary:**
- Use `spiff_format.ino` only for recovery or first-time setup.
- Never format SPIFFS as part of normal workflow.
