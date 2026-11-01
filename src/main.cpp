// Password Vault - ESP32-S3 firmware
//
// The device is a dumb encrypted-blob store: all encryption/decryption happens
// on the client (PC / phone). The device never sees the master password or keys.
//
// Serial protocol (one command per line, '\n' terminated):
//   PING              -> PONG
//   INFO              -> INFO <proto> <max_bytes> <stored_bytes> <seq>
//   GET               -> DATA <blob>   | EMPTY
//   BEGIN <len>       -> READY             (start an upload of <len> bytes)
//   CHUNK <text>      -> ACK <received>    (max 1024 chars per chunk)
//   COMMIT            -> OK <seq>          | ERR <reason>
//   WIPE              -> OK 0              | ERR <reason>
//
// Uploads are chunked with an ACK per chunk because the USB-Serial/JTAG
// driver drops incoming bytes when its RX buffer fills (no backpressure).
//
// Storage: "vault" partition with two 128KB slots (A/B). A write goes to the
// slot not currently in use, so a power loss mid-write never corrupts the last
// good copy. Slots are erased only in whole 64KB blocks because this board's
// flash ignores 4KB sector erase in most of its address space.

#include <Arduino.h>
#include <esp_partition.h>
#include <rom/crc.h>

static const char *PROTO_VERSION = "1";
static const uint32_t SLOT_SIZE = 0x20000;  // 128KB, multiple of 64KB
static const uint32_t MAGIC = 0x544C5656;   // "VVLT"

struct SlotHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
};

static const uint32_t MAX_BLOB = SLOT_SIZE - sizeof(SlotHeader);
static const size_t MAX_CHUNK = 1024;
static const size_t MAX_LINE = MAX_CHUNK + 16;

static const esp_partition_t *vaultPart = nullptr;
static char *lineBuf = nullptr;
static uint8_t *uploadBuf = nullptr;
static uint32_t uploadLen = 0;       // expected size, 0 = no upload in progress
static uint32_t uploadReceived = 0;
static size_t lineLen = 0;
static bool lineOverflow = false;

static uint32_t blobCrc(const uint8_t *data, uint32_t len) {
    return crc32_le(0, data, len);
}

// Returns true if the slot holds a valid blob; fills hdr.
static bool readSlotHeader(int slot, SlotHeader &hdr) {
    if (esp_partition_read(vaultPart, slot * SLOT_SIZE, &hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (hdr.magic != MAGIC || hdr.len > MAX_BLOB) {
        return false;
    }
    // Verify CRC over the stored data in chunks.
    uint8_t chunk[512];
    uint32_t crc = 0;
    for (uint32_t off = 0; off < hdr.len; off += sizeof(chunk)) {
        uint32_t n = min((uint32_t)sizeof(chunk), hdr.len - off);
        if (esp_partition_read(vaultPart, slot * SLOT_SIZE + sizeof(hdr) + off, chunk, n) != ESP_OK) {
            return false;
        }
        crc = crc32_le(crc, chunk, n);
    }
    return crc == hdr.crc;
}

// Index of the newest valid slot, or -1 if none.
static int activeSlot(SlotHeader &hdr) {
    SlotHeader h[2];
    bool ok[2] = {readSlotHeader(0, h[0]), readSlotHeader(1, h[1])};
    int best = -1;
    if (ok[0]) best = 0;
    if (ok[1] && (best < 0 || h[1].seq > h[0].seq)) best = 1;
    if (best >= 0) hdr = h[best];
    return best;
}

static bool writeSlot(int slot, uint32_t seq, const uint8_t *data, uint32_t len) {
    uint32_t base = slot * SLOT_SIZE;
    if (esp_partition_erase_range(vaultPart, base, SLOT_SIZE) != ESP_OK) return false;
    if (len > 0 && esp_partition_write(vaultPart, base + sizeof(SlotHeader), data, len) != ESP_OK) return false;
    // Header last: the slot only becomes valid once data is fully written.
    SlotHeader hdr = {MAGIC, seq, len, blobCrc(data, len)};
    if (esp_partition_write(vaultPart, base, &hdr, sizeof(hdr)) != ESP_OK) return false;
    // Read back and verify (catches flash that silently failed to erase).
    SlotHeader check;
    return readSlotHeader(slot, check) && check.seq == seq;
}

static void cmdInfo() {
    SlotHeader hdr;
    int slot = activeSlot(hdr);
    Serial.printf("INFO %s %u %u %u\n", PROTO_VERSION, (unsigned)MAX_BLOB,
                  slot >= 0 ? (unsigned)hdr.len : 0u, slot >= 0 ? (unsigned)hdr.seq : 0u);
}

static void cmdGet() {
    SlotHeader hdr;
    int slot = activeSlot(hdr);
    if (slot < 0 || hdr.len == 0) {
        Serial.println("EMPTY");
        return;
    }
    Serial.print("DATA ");
    uint8_t chunk[512];
    for (uint32_t off = 0; off < hdr.len; off += sizeof(chunk)) {
        uint32_t n = min((uint32_t)sizeof(chunk), hdr.len - off);
        esp_partition_read(vaultPart, slot * SLOT_SIZE + sizeof(hdr) + off, chunk, n);
        Serial.write(chunk, n);
    }
    Serial.print("\n");
}

static void cmdPut(const uint8_t *blob, uint32_t len);

static void cmdBegin(const char *arg) {
    long len = atol(arg);
    if (len <= 0) {
        Serial.println("ERR bad_length");
        return;
    }
    if ((uint32_t)len > MAX_BLOB) {
        Serial.println("ERR too_large");
        return;
    }
    uploadLen = (uint32_t)len;
    uploadReceived = 0;
    Serial.println("READY");
}

static void cmdChunk(const char *data, size_t len) {
    if (uploadLen == 0) {
        Serial.println("ERR no_upload");
        return;
    }
    if (uploadReceived + len > uploadLen) {
        uploadLen = 0;
        Serial.println("ERR overflow");
        return;
    }
    memcpy(uploadBuf + uploadReceived, data, len);
    uploadReceived += len;
    Serial.printf("ACK %u\n", (unsigned)uploadReceived);
}

static void cmdCommit() {
    if (uploadLen == 0 || uploadReceived != uploadLen) {
        uploadLen = 0;
        Serial.println("ERR incomplete");
        return;
    }
    uploadLen = 0;
    cmdPut(uploadBuf, uploadReceived);
}

static void cmdPut(const uint8_t *blob, uint32_t len) {
    SlotHeader hdr;
    int cur = activeSlot(hdr);
    int target = (cur == 0) ? 1 : 0;
    uint32_t seq = (cur >= 0) ? hdr.seq + 1 : 1;
    if (!writeSlot(target, seq, blob, len)) {
        Serial.println("ERR write_failed");
        return;
    }
    Serial.printf("OK %u\n", (unsigned)seq);
}

static void cmdWipe() {
    if (esp_partition_erase_range(vaultPart, 0, 2 * SLOT_SIZE) != ESP_OK) {
        Serial.println("ERR erase_failed");
        return;
    }
    Serial.println("OK 0");
}

static void handleLine(char *line, size_t len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    if (len == 0) return;

    // CHUNK payload is case-sensitive: handle it before uppercasing.
    if (strncmp(line, "CHUNK ", 6) == 0) {
        cmdChunk(line + 6, len - 6);
        return;
    }
    for (size_t i = 0; i < len; i++) line[i] = toupper((unsigned char)line[i]);

    if (strcmp(line, "PING") == 0) {
        Serial.println("PONG");
    } else if (strcmp(line, "INFO") == 0) {
        cmdInfo();
    } else if (strcmp(line, "GET") == 0) {
        cmdGet();
    } else if (strncmp(line, "BEGIN ", 6) == 0) {
        cmdBegin(line + 6);
    } else if (strcmp(line, "COMMIT") == 0) {
        cmdCommit();
    } else if (strcmp(line, "WIPE") == 0) {
        cmdWipe();
    } else {
        Serial.print("ERR unknown_command ");
        Serial.println(line);
    }
}

void setup() {
    Serial.setRxBufferSize(8192);
    Serial.begin(115200);
    delay(1500);

    lineBuf = (char *)malloc(MAX_LINE + 1);
    uploadBuf = (uint8_t *)malloc(MAX_BLOB);
    vaultPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         (esp_partition_subtype_t)0x40, "vault");

    Serial.println("PASSWORD VAULT");
    if (!lineBuf || !uploadBuf || !vaultPart) {
        Serial.println("ERR init_failed (partizione vault o memoria mancante)");
    } else {
        Serial.println("READY");
    }
}

void loop() {
    if (!lineBuf || !uploadBuf || !vaultPart) {
        delay(1000);
        return;
    }
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            if (lineOverflow) {
                Serial.println("ERR too_large");
            } else {
                lineBuf[lineLen] = '\0';
                handleLine(lineBuf, lineLen);
            }
            lineLen = 0;
            lineOverflow = false;
        } else if (lineLen < MAX_LINE) {
            lineBuf[lineLen++] = c;
        } else {
            lineOverflow = true;
        }
    }
}
