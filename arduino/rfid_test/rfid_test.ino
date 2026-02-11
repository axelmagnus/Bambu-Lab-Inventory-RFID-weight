// RFID Reader Test for ESP32-S2 Reverse TFT Feather
// Uses MFRC522/RC522 module
// Connect as follows:
// SS (SDA)  -> GPIO10 (D10)
// RST       -> GPIO9  (D9)
// SCK       -> GPIO36 (SCK)
// MOSI      -> GPIO35 (MOSI)
// MISO      -> GPIO37 (MISO)
// 3.3V      -> 3.3V
// GND       -> GND

#include <SPI.h>
#include <MFRC522.h>

#include <mbedtls/md.h>

#define SS_PIN 13  // D13
#define RST_PIN 11 // D11

MFRC522 mfrc522(SS_PIN, RST_PIN);

// HKDF constants (from your OLED project)
static const uint8_t HKDF_SALT[16] = {0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff, 0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96};
static const uint8_t HKDF_INFO[7] = {'R', 'F', 'I', 'D', '-', 'A', 0x00};
static byte SECTOR_KEY_A[16][6];

void hkdfFromUid(const uint8_t *uid, size_t uidLen, uint8_t *out, size_t outLen)
{
    // Step 1: Extract (PRK = HMAC-SHA256(salt, uid))
    uint8_t prk[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, HKDF_SALT, sizeof(HKDF_SALT));
    mbedtls_md_hmac_update(&ctx, uid, uidLen);
    mbedtls_md_hmac_finish(&ctx, prk);
    mbedtls_md_free(&ctx);

    // Step 2: Expand
    size_t pos = 0;
    uint8_t counter = 1;
    uint8_t t[32];
    size_t infoLen = sizeof(HKDF_INFO);
    while (pos < outLen)
    {
        mbedtls_md_context_t ctx2;
        mbedtls_md_init(&ctx2);
        mbedtls_md_setup(&ctx2, info, 1);
        mbedtls_md_hmac_starts(&ctx2, prk, sizeof(prk));
        if (counter > 1)
            mbedtls_md_hmac_update(&ctx2, t, sizeof(t));
        mbedtls_md_hmac_update(&ctx2, HKDF_INFO, infoLen);
        mbedtls_md_hmac_update(&ctx2, &counter, 1);
        mbedtls_md_hmac_finish(&ctx2, t);
        mbedtls_md_free(&ctx2);
        size_t take = (outLen - pos < sizeof(t)) ? (outLen - pos) : sizeof(t);
        memcpy(out + pos, t, take);
        pos += take;
        counter++;
    }
}

void deriveKeysFromUid(const byte *uid, byte uidLen)
{
    uint8_t derived[16 * 6];
    hkdfFromUid(uid, uidLen, derived, sizeof(derived));
    for (uint8_t s = 0; s < 16; s++)
    {
        memcpy(SECTOR_KEY_A[s], derived + s * 6, 6);
    }
}

uint16_t le16(const byte *p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
float leFloat(const byte *p)
{
    float f = 0.0f;
    memcpy(&f, p, sizeof(float));
    return f;
}
void printHex(const byte *buffer, byte bufferSize)
{
    for (byte i = 0; i < bufferSize; i++)
    {
        if (buffer[i] < 0x10)
            Serial.print("0");
        Serial.print(buffer[i], HEX);
        if (i + 1 < bufferSize)
            Serial.print(":");
    }
}
void copyTrim(char *dst, size_t dstSize, const byte *src, size_t srcLen)
{
    size_t n = srcLen < (dstSize - 1) ? srcLen : (dstSize - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (int i = static_cast<int>(n) - 1; i >= 0; --i)
    {
        if (dst[i] == ' ' || dst[i] == '\0')
            dst[i] = '\0';
        else
            break;
    }
}

void decodeBlock(uint8_t block, const byte *data)
{
    switch (block)
    {
    case 1:
    {
        char variant[9], material[9];
        copyTrim(variant, sizeof(variant), data, 8);
        copyTrim(material, sizeof(material), data + 8, 8);
        Serial.print("Variant: ");
        Serial.print(variant);
        Serial.print("  Material: ");
        Serial.println(material);
        break;
    }
    case 2:
        Serial.print("Filament type: ");
        for (int i = 0; i < 16; i++)
            Serial.write(data[i]);
        Serial.println();
        break;
    case 5:
        Serial.print("Color RGBA: 0x");
        for (int i = 3; i >= 0; i--)
        {
            if (data[i] < 0x10)
                Serial.print("0");
            Serial.print(data[i], HEX);
        }
        Serial.print("  Weight(g): ");
        Serial.print(le16(&data[4]));
        Serial.print("  Diameter(mm): ");
        Serial.println(leFloat(&data[8]), 3);
        break;
    case 6:
        Serial.print("DryTemp: ");
        Serial.print(le16(&data[0]));
        Serial.print("C  DryTime(h): ");
        Serial.print(le16(&data[2]));
        Serial.print("  BedTemp: ");
        Serial.print(le16(&data[6]));
        Serial.print("C  HotendMax: ");
        Serial.print(le16(&data[8]));
        Serial.print("C  HotendMin: ");
        Serial.println(le16(&data[10]));
        break;
    case 8:
        Serial.print("Nozzle(mm): ");
        Serial.println(leFloat(&data[12]), 3);
        break;
    case 9:
        Serial.print("Tray UID: ");
        printHex(data, 16);
        Serial.println();
        break;
    case 10:
        Serial.print("Spool width(mm): ");
        Serial.println(le16(&data[4]) / 100.0f, 2);
        break;
    case 12:
        Serial.print("Prod date: ");
        for (int i = 0; i < 16; i++)
            Serial.write(data[i]);
        Serial.println();
        break;
    case 14:
        Serial.print("Length(m): ");
        Serial.println(le16(&data[4]));
        break;
    case 16:
        Serial.print("FormatId: ");
        Serial.print(le16(&data[0]));
        Serial.print("  ColorCount: ");
        Serial.print(le16(&data[2]));
        Serial.print("  SecondColor ABGR: 0x");
        for (int i = 7; i >= 4; i--)
        {
            if (data[i] < 0x10)
                Serial.print("0");
            Serial.print(data[i], HEX);
        }
        Serial.println();
        break;
    default:
        break;
    }
}

void readClassic()
{
    static const uint8_t TARGET_BLOCKS[] = {1, 2, 5, 6, 8, 9, 10, 12, 14, 16};
    MFRC522::MIFARE_Key key;
    byte buffer[18];
    byte size = sizeof(buffer);
    int8_t authedSector = -1;
    for (size_t i = 0; i < sizeof(TARGET_BLOCKS); i++)
    {
        uint8_t block = TARGET_BLOCKS[i];
        uint8_t sector = block / 4;
        if (sector != authedSector)
        {
            memcpy(&key.keyByte, SECTOR_KEY_A[sector], 6);
            if (mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid)) != MFRC522::STATUS_OK)
            {
                Serial.print("Auth failed for sector ");
                Serial.println(sector);
                authedSector = -1;
                continue;
            }
            authedSector = sector;
        }
        size = sizeof(buffer);
        if (mfrc522.MIFARE_Read(block, buffer, &size) == MFRC522::STATUS_OK)
        {
            Serial.print("Block ");
            Serial.print(block);
            Serial.print(": ");
            printHex(buffer, 16);
            Serial.println();
            decodeBlock(block, buffer);
        }
        else
        {
            Serial.print("Read failed for block ");
            Serial.println(block);
        }
    }
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
    }
    SPI.begin();
    mfrc522.PCD_Init();
    Serial.println("\nScan a Bambu Lab RFID tag...");
}

void loop()
{
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
    {
        delay(100);
        return;
    }
    Serial.print("Tag UID: ");
    printHex(mfrc522.uid.uidByte, mfrc522.uid.size);
    Serial.println();
    deriveKeysFromUid(mfrc522.uid.uidByte, mfrc522.uid.size);
    readClassic();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    Serial.println("-----");
}
