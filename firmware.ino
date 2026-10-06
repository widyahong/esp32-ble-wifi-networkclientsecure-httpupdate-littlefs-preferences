/* ================================================================
   GLOBAL CONFIG - EDIT ONLY THIS SECTION
   ================================================================ */
#define BLE_SERVICE_UUID     "0000aaaa-0000-1000-8000-00805f9b34fb"
#define BLE_CHAR_UUID        "0000bbbb-0000-1000-8000-00805f9b34fb"

// Advertised BLE name = BLE_NAME_PREFIX + 12-hex-char MAC.
// Adv packet budget (flags + service UUID + name) is ~20 bytes, MAC takes 12,
// so the prefix has ~8 chars left. static_assert below catches an oversized
// prefix at compile time instead of it silently getting truncated at runtime.
#define BLE_NAME_PREFIX       "wictmgs-"      // <-- set here, max ~8 chars

// OTA URLs stored as hex bytes instead of plain strings, so they don't show
// up on a quick strings/grep pass of the source or the compiled binary. This
// is light obfuscation only (not encryption) - decoded back at runtime.
// All three OTA files live under the same path, so the common prefix is
// stored once and each target's short suffix is appended at runtime:
//   app  -> gce.widyahong.com/wictmgs/electronics/recovery/app.bin
//   data -> gce.widyahong.com/wictmgs/electronics/recovery/data.bin
//   nvs  -> gce.widyahong.com/wictmgs/electronics/recovery/nvs.bin
static const uint8_t OTA_PATH_PREFIX_HEX[] = {
  0x67, 0x63, 0x65, 0x2E, 0x77, 0x69, 0x64, 0x79, 0x61, 0x68, 0x6F, 0x6E,
  0x67, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x77, 0x69, 0x63, 0x74, 0x6D, 0x67,
  0x73, 0x2F, 0x65, 0x6C, 0x65, 0x63, 0x74, 0x72, 0x6F, 0x6E, 0x69, 0x63,
  0x73, 0x2F, 0x72, 0x65, 0x63, 0x6F, 0x76, 0x65, 0x72, 0x79, 0x2F,
};
static const uint8_t OTA_SUFFIX_APP_HEX[]  = { 0x61, 0x70, 0x70, 0x2E, 0x62, 0x69, 0x6E };             // "app.bin"
static const uint8_t OTA_SUFFIX_DATA_HEX[] = { 0x64, 0x61, 0x74, 0x61, 0x2E, 0x62, 0x69, 0x6E };       // "data.bin"
static const uint8_t OTA_SUFFIX_NVS_HEX[]  = { 0x6E, 0x76, 0x73, 0x2E, 0x62, 0x69, 0x6E };             // "nvs.bin"
static const size_t OTA_PATH_PREFIX_HEX_LEN = sizeof(OTA_PATH_PREFIX_HEX);
static const size_t OTA_SUFFIX_APP_HEX_LEN  = sizeof(OTA_SUFFIX_APP_HEX);
static const size_t OTA_SUFFIX_DATA_HEX_LEN = sizeof(OTA_SUFFIX_DATA_HEX);
static const size_t OTA_SUFFIX_NVS_HEX_LEN  = sizeof(OTA_SUFFIX_NVS_HEX);

// File used by the manual BLE DATA:WRITE / DATA:READ commands and by
// UPDATE:DATA. Fixed path, overwritten every time - LittleFS wear-leveling
// happens under the hood at the block level regardless of the filename
// staying the same.
#define DATA_LOCAL_PATH          "/data.bin"

// NVS namespace/key used by the manual BLE NVS:WRITE / NVS:READ commands and
// by UPDATE:NVS. Deliberately separate from NVS_SECURITY_NAMESPACE/NVS_SECURITY_KEY below,
// which the anti-clone binding owns - the two features never touch the same
// namespace or key.
#define NVS_BLOB_NAMESPACE       "nvs"
#define NVS_BLOB_KEY             "blob"

// Chunk size for DATA/NVS read notifies and the write-side chunking the
// browser is expected to use; matches the default BLE MTU payload (~20B).
#define BLE_CHUNK_SIZE           20

#define NVS_SECURITY_NAMESPACE          "security"
#define NVS_SECURITY_KEY            "boundmac"
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Status LED:
// off at idle, solid on only while a partition (app/data/nvs) is actually
// being read or written - OTA to any of the three, or a manual BLE
// DATA/NVS read or write. No activity = no light at all.
// Using the dev board's onboard LED on GPIO2 - same pin/polarity as the
// stock Blink example (active HIGH, plain push-pull), so no special wiring.
#define STATUS_LED_PIN           2

// --- Feature toggles ---
// Use 1/0 (not true/false) so #if actually strips disabled code at compile
// time (saves flash), unlike a runtime "if (flag)" which still compiles both
// branches and just skips one of them at runtime.
#define ENABLE_SERIAL_PRINT    1     // 0 = drop all Serial calls from the binary
#define ENABLE_BLE_NOTIFY      1     // 0 = drop BLE notify calls (independent of Serial)
#define ENABLE_HTTPS           1     // 0 = plain HTTP, drops WiFiClientSecure code
#define ENABLE_DEVICE_BINDING  1     // 0 = drop MAC anti-clone binding entirely
#define ENABLE_STATUS_LED      1     // 0 = drop LED blink code entirely
#define SERIAL_BAUD_RATE       115200
/* ================================================================ */

#include <WiFi.h>
#if ENABLE_HTTPS
  #include <WiFiClientSecure.h>
#endif
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Prefix + 12-char MAC must fit the ~20-char adv name budget.
static_assert((sizeof(BLE_NAME_PREFIX) - 1) <= (20 - 12),
              "BLE_NAME_PREFIX too long - MAC needs 12 of the ~20 available chars");

BLEServer         *pServer         = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;
bool deviceLocked    = false;
bool bleActive       = false;

bool   wifiRequest = false;
String pendingSsid, pendingPass;

enum OtaTarget { OTA_NONE, OTA_APP, OTA_DATA, OTA_NVS };
bool      otaRequest = false;
OtaTarget otaTarget   = OTA_NONE;

/* ---------- DATA write state (browser -> device, raw chunks straight to file) ---------- */
bool   dataWriteActive   = false;
size_t dataWriteExpected = 0;
size_t dataWriteReceived = 0;
File   dataWriteFile;

/* ---------- DATA read state (device -> browser, request-per-chunk, file stays open) ---------- */
bool   dataReadRequest  = false; // "DATA:READ" received, mount + open + send SIZE:
bool   dataNextRequest  = false; // "DATA:NEXT" received, send one more chunk
bool   dataReadActive   = false;
File   dataReadFile;
size_t dataReadRemaining = 0;

/* ---------- NVS blob write state (browser -> device, buffered - Preferences needs the whole blob at once) ---------- */
bool     nvsWriteActive   = false;
size_t   nvsWriteExpected = 0;
size_t   nvsWriteReceived = 0;
uint8_t *nvsWriteBuffer   = nullptr;

/* ---------- NVS blob read state (device -> browser, request-per-chunk out of a buffered copy) ---------- */
bool     nvsReadRequest  = false; // "NVS:READ" received, load blob into RAM + send SIZE:
bool     nvsNextRequest  = false; // "NVS:NEXT" received, send one more chunk
bool     nvsReadActive   = false;
uint8_t *nvsReadBuffer   = nullptr;
size_t   nvsReadRemaining = 0;
size_t   nvsReadOffset    = 0;

/* ---------- Logging: Serial and BLE notify are independent toggles ---------- */
#if ENABLE_SERIAL_PRINT
  #define LOG_SERIAL(msg) Serial.println(msg)
#else
  #define LOG_SERIAL(msg)
#endif

#if ENABLE_BLE_NOTIFY
static inline void notifyBLE(const String &msg) {
  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
    pCharacteristic->notify();
  }
}
static inline void notifyBLERaw(const uint8_t *data, size_t len) {
  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue((uint8_t*)data, len);
    pCharacteristic->notify();
  }
}
#else
static inline void notifyBLE(const String &msg) {}
static inline void notifyBLERaw(const uint8_t *data, size_t len) {}
#endif

// Single call site for "log this everywhere"; each sink can be disabled on its own.
static inline void deviceLog(const String &msg) {
  LOG_SERIAL(msg);
  notifyBLE(msg);
}

/* ---------- Status LED: plain push-pull OUTPUT, active HIGH ----------
   Same pin/polarity as the stock Arduino Blink example for this board - the
   onboard LED is wired for a normal driven HIGH/LOW output, not open-drain,
   so that's what this uses. No timer, no extra library - just digitalWrite.
   Off whenever nothing is happening; ledSetBusy(true) turns it on solid,
   ledSetBusy(false) turns it off. Called around every partition op: OTA to
   app/data/nvs, and manual BLE DATA/NVS read or write. ---------- */
#if ENABLE_STATUS_LED
volatile bool ledBusy = false;

static inline void ledApply(bool on) {
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

void setupStatusLed() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  ledApply(false);
}

// true when a partition op starts (OTA app/data/nvs, or manual DATA/NVS
// read/write), false when it ends. Safe to call repeatedly with the same
// value (no-op if already in that state).
void ledSetBusy(bool busy) {
  if (busy == ledBusy) return;
  ledBusy = busy;
  ledApply(busy);
}
#else
void setupStatusLed() {}
void ledSetBusy(bool busy) {}
#endif

/* ---------- MAC helper (used for binding + BLE name) ---------- */
static String getMacHex() {
  uint64_t mac = ESP.getEfuseMac();
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%012llX", mac);
  return String(macStr);
}

/* ---------- Anti-clone: bind firmware to this chip's MAC ----------
   Uses its own namespace/key (NVS_SECURITY_NAMESPACE / NVS_SECURITY_KEY), entirely
   separate from NVS_BLOB_NAMESPACE / NVS_BLOB_KEY used by the manual
   NVS:WRITE/NVS:READ/UPDATE:NVS feature below. ---------- */
#if ENABLE_DEVICE_BINDING
void checkDeviceBinding() {
  String mac = getMacHex();

  Preferences prefs;
  prefs.begin(NVS_SECURITY_NAMESPACE, false);
  String stored = prefs.getString(NVS_SECURITY_KEY, "");

  if (stored.length() == 0) {
    prefs.putString(NVS_SECURITY_KEY, mac);
    deviceLog("[SEC] first boot, bound " + mac);
    deviceLocked = false;
  } else if (stored == mac) {
    deviceLog("[SEC] MAC ok");
    deviceLocked = false;
  } else {
    deviceLog("[SEC] MAC mismatch, locked");
    deviceLocked = true;
  }
  prefs.end();
}
#else
void checkDeviceBinding() {
  deviceLocked = false;
}
#endif

/* ---------- BLE server callbacks ---------- */
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { deviceConnected = true; }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    if (bleActive) {
      BLEDevice::startAdvertising();
    }
  }
};

/* ---------- BLE write handler ----------
   Three modes, mutually exclusive:
   - dataWriteActive == true : every write is raw bytes appended straight to
     the open LittleFS file, until dataWriteExpected is reached.
   - nvsWriteActive  == true : every write is raw bytes copied into
     nvsWriteBuffer (malloc'd once up front), until nvsWriteExpected is
     reached, then committed with a single Preferences::putBytes() call -
     unlike LittleFS, NVS has no "write partial, continue later" API.
   - otherwise: the write is parsed as a text command (WIFI:, UPDATE:*,
     DATA:*, NVS:*, or echo).
   getValue() is used directly (not re-wrapped through c_str()+String()),
   so embedded 0x00 bytes in binary chunks are preserved correctly.
------------------------------------------------------------------------ */
class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String raw = pChar->getValue();

    if (dataWriteActive) {
      size_t n = raw.length();
      if (n > 0) {
        dataWriteFile.write((const uint8_t*)raw.c_str(), n);
        dataWriteReceived += n;
      }
      if (dataWriteReceived >= dataWriteExpected) {
        dataWriteFile.close();
        LittleFS.end();
        dataWriteActive = false;
        ledSetBusy(false);
        deviceLog("[DATA] write done, " + String(dataWriteReceived) + " bytes");
      }
      return;
    }

    if (nvsWriteActive) {
      size_t n = raw.length();
      size_t room = nvsWriteExpected - nvsWriteReceived;
      if (n > room) n = room; // never overrun the malloc'd buffer
      if (n > 0) {
        memcpy(nvsWriteBuffer + nvsWriteReceived, raw.c_str(), n);
        nvsWriteReceived += n;
      }
      if (nvsWriteReceived >= nvsWriteExpected) {
        Preferences prefs;
        prefs.begin(NVS_BLOB_NAMESPACE, false);
        size_t written = prefs.putBytes(NVS_BLOB_KEY, nvsWriteBuffer, nvsWriteExpected);
        prefs.end();
        free(nvsWriteBuffer);
        nvsWriteBuffer = nullptr;
        nvsWriteActive = false;
        ledSetBusy(false);
        if (written == nvsWriteExpected) {
          deviceLog("[NVS] write done, " + String(written) + " bytes");
        } else {
          deviceLog("[NVS] write incomplete, " + String(written) + "/" + String(nvsWriteExpected));
        }
      }
      return;
    }

    String value = raw;
    value.trim();
    if (value.length() == 0) return;

    if (value.startsWith("WIFI:")) {
      String rest = value.substring(5);
      int sep = rest.indexOf('|');
      if (sep > 0) {
        pendingSsid = rest.substring(0, sep);
        pendingPass = rest.substring(sep + 1);
        wifiRequest = true;
      } else {
        deviceLog("ERR: use WIFI:ssid|pass");
      }
    } else if (value == "UPDATE:APP") {
      otaRequest = true;
      otaTarget = OTA_APP;
    } else if (value == "UPDATE:DATA") {
      otaRequest = true;
      otaTarget = OTA_DATA;
    } else if (value == "UPDATE:NVS") {
      otaRequest = true;
      otaTarget = OTA_NVS;
    } else if (value.startsWith("DATA:WRITE:")) {
      size_t size = value.substring(11).toInt();
      if (!LittleFS.begin(true)) { // true = format if mount fails
        deviceLog("[DATA] mount failed");
      } else {
        dataWriteFile = LittleFS.open(DATA_LOCAL_PATH, "w"); // "w" truncates = overwrite old file
        if (!dataWriteFile) {
          deviceLog("[DATA] write open failed");
          LittleFS.end();
        } else {
          dataWriteExpected = size;
          dataWriteReceived = 0;
          if (size == 0) {
            dataWriteFile.close();
            LittleFS.end();
            deviceLog("[DATA] write done, 0 bytes");
          } else {
            dataWriteActive = true;
            ledSetBusy(true);
          }
        }
      }
    } else if (value == "DATA:READ") {
      dataReadRequest = true;
    } else if (value == "DATA:NEXT") {
      dataNextRequest = true;
    } else if (value.startsWith("NVS:WRITE:")) {
      size_t size = value.substring(10).toInt();
      if (size == 0) {
        deviceLog("[NVS] write done, 0 bytes");
      } else {
        nvsWriteBuffer = (uint8_t*)malloc(size);
        if (!nvsWriteBuffer) {
          deviceLog("[NVS] write malloc failed");
        } else {
          nvsWriteExpected = size;
          nvsWriteReceived = 0;
          nvsWriteActive = true;
          ledSetBusy(true);
        }
      }
    } else if (value == "NVS:READ") {
      nvsReadRequest = true;
    } else if (value == "NVS:NEXT") {
      nvsNextRequest = true;
    } else {
      deviceLog("ECHO: " + value);
    }
  }
};

/* ---------- Bring BLE up ---------- */
void setupBLE() {
  String bleName = String(BLE_NAME_PREFIX) + getMacHex();

  BLEDevice::init(bleName.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
      BLE_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new WriteCallbacks());

  pService->start();
  pServer->getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  pServer->getAdvertising()->start();

  bleActive = true;
  deviceLog("[BLE] adv as " + bleName);
}

/* ---------- Tear BLE down to free RAM/flash during OTA ---------- */
void teardownBLE() {
  if (!bleActive) return;
  LOG_SERIAL("[BLE] off for OTA");
  deviceConnected = false;
  BLEDevice::deinit(true);
  pServer = nullptr;
  pCharacteristic = nullptr;
  bleActive = false;
}

/* ---------- Decode prefix + target suffix back to a URL string ---------- */
String buildOtaUrl(OtaTarget target) {
#if ENABLE_HTTPS
  String url = "https://";
#else
  String url = "http://";
#endif

  for (size_t i = 0; i < OTA_PATH_PREFIX_HEX_LEN; i++) {
    url += (char)OTA_PATH_PREFIX_HEX[i];
  }

  const uint8_t *suffix = nullptr;
  size_t suffixLen = 0;
  switch (target) {
    case OTA_APP:  suffix = OTA_SUFFIX_APP_HEX;  suffixLen = OTA_SUFFIX_APP_HEX_LEN;  break;
    case OTA_DATA: suffix = OTA_SUFFIX_DATA_HEX; suffixLen = OTA_SUFFIX_DATA_HEX_LEN; break;
    case OTA_NVS:  suffix = OTA_SUFFIX_NVS_HEX;  suffixLen = OTA_SUFFIX_NVS_HEX_LEN;  break;
    default: break;
  }
  for (size_t i = 0; i < suffixLen; i++) {
    url += (char)suffix[i];
  }
  return url;
}

/* ---------- OTA: app partition via HTTPUpdate ---------- */
void doOtaApp() {
  ledSetBusy(true);
  deviceLog("OTA app: start");
  if (WiFi.status() != WL_CONNECTED) {
    deviceLog("OTA app: no WiFi");
    ledSetBusy(false);
    return;
  }

  teardownBLE(); // give HTTPUpdate/TLS maximum heap headroom, no fragmentation risk

  String url = buildOtaUrl(OTA_APP);
  t_httpUpdate_return ret;
  httpUpdate.rebootOnUpdate(true);

#if ENABLE_HTTPS
  WiFiClientSecure client;
  client.setInsecure();
  ret = httpUpdate.update(client, url);
#else
  WiFiClient client;
  ret = httpUpdate.update(client, url);
#endif

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      LOG_SERIAL("OTA app fail: " + httpUpdate.getLastErrorString());
      setupBLE();
      ledSetBusy(false);
      deviceLog("OTA app failed");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      setupBLE();
      ledSetBusy(false);
      deviceLog("OTA app: none");
      break;
    case HTTP_UPDATE_OK:
      // Reboots automatically (rebootOnUpdate(true)); no need to turn the
      // LED off - the restart does that for free.
      LOG_SERIAL("OTA app ok, rebooting");
      break;
  }
}

/* ---------- OTA: data partition via LittleFS, streamed straight to file ---------- */
void doOtaData() {
  ledSetBusy(true);
  deviceLog("OTA data: start");
  if (WiFi.status() != WL_CONNECTED) {
    deviceLog("OTA data: no WiFi");
    ledSetBusy(false);
    return;
  }

  teardownBLE();

  String url = buildOtaUrl(OTA_DATA);
  bool ok = false;

  if (!LittleFS.begin(true)) {
    LOG_SERIAL("OTA data fail: mount failed");
  } else {
#if ENABLE_HTTPS
    WiFiClientSecure client;
    client.setInsecure();
#else
    WiFiClient client;
#endif
    HTTPClient http;
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        int total = http.getSize(); // -1 if server didn't send Content-Length
        File f = LittleFS.open(DATA_LOCAL_PATH, "w"); // "w" truncates = overwrite old file
        if (f) {
          int written = http.writeToStream(&f); // streamed - RAM use stays ~constant
          f.close();
          ok = (written > 0) && (total < 0 || written == total);
        } else {
          LOG_SERIAL("OTA data fail: cannot open file");
        }
      } else {
        LOG_SERIAL("OTA data fail: HTTP " + String(code));
      }
      http.end();
    }
    LittleFS.end();
  }

  if (ok) {
    LOG_SERIAL("OTA data ok, rebooting");
    delay(500);
    ESP.restart();
  } else {
    setupBLE(); // bring BLE back so client can be told / retry
    ledSetBusy(false);
    deviceLog("OTA data failed");
  }
}

/* ---------- OTA: NVS blob via Preferences ----------
   Preferences has no streaming write API, so the whole blob is downloaded
   into one malloc'd buffer sized to the response's Content-Length, then
   committed with a single putBytes() call. A missing Content-Length is
   treated as a failure rather than growing a buffer unboundedly. ---------- */
void doOtaNvs() {
  ledSetBusy(true);
  deviceLog("OTA nvs: start");
  if (WiFi.status() != WL_CONNECTED) {
    deviceLog("OTA nvs: no WiFi");
    ledSetBusy(false);
    return;
  }

  teardownBLE();

  String url = buildOtaUrl(OTA_NVS);
  bool ok = false;

#if ENABLE_HTTPS
  WiFiClientSecure client;
  client.setInsecure();
#else
  WiFiClient client;
#endif
  HTTPClient http;
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      int total = http.getSize();
      if (total <= 0) {
        LOG_SERIAL("OTA nvs fail: unknown content length");
      } else {
        uint8_t *buf = (uint8_t*)malloc(total);
        if (!buf) {
          LOG_SERIAL("OTA nvs fail: malloc failed");
        } else {
          WiFiClient *stream = http.getStreamPtr();
          size_t received = 0;
          unsigned long lastProgress = millis();
          while (received < (size_t)total && (millis() - lastProgress) < 30000) {
            if (stream->available()) {
              int r = stream->read(buf + received, total - received);
              if (r > 0) {
                received += r;
                lastProgress = millis();
              }
            } else {
              delay(1);
            }
          }
          if (received == (size_t)total) {
            Preferences prefs;
            prefs.begin(NVS_BLOB_NAMESPACE, false);
            size_t written = prefs.putBytes(NVS_BLOB_KEY, buf, total);
            prefs.end();
            ok = (written == (size_t)total);
            if (!ok) LOG_SERIAL("OTA nvs fail: putBytes incomplete");
          } else {
            LOG_SERIAL("OTA nvs fail: download incomplete/stalled");
          }
          free(buf);
        }
      }
    } else {
      LOG_SERIAL("OTA nvs fail: HTTP " + String(code));
    }
    http.end();
  }

  if (ok) {
    LOG_SERIAL("OTA nvs ok, rebooting");
    delay(500);
    ESP.restart();
  } else {
    setupBLE();
    ledSetBusy(false);
    deviceLog("OTA nvs failed");
  }
}

/* ---------- DATA:READ - mount, open file, report size, kick off first chunk ---------- */
void handleDataReadRequest() {
  if (!LittleFS.begin(true)) {
    deviceLog("[DATA] mount failed");
    notifyBLE("SIZE:0");
    dataReadActive = false;
    return;
  }

  dataReadFile = LittleFS.open(DATA_LOCAL_PATH, "r");
  if (!dataReadFile) {
    deviceLog("[DATA] read open failed");
    LittleFS.end();
    notifyBLE("SIZE:0");
    dataReadActive = false;
    return;
  }

  dataReadRemaining = dataReadFile.size();
  LOG_SERIAL("[DATA] reading, size=" + String(dataReadRemaining));
  notifyBLE("SIZE:" + String(dataReadRemaining));

  if (dataReadRemaining == 0) {
    dataReadFile.close();
    LittleFS.end();
    dataReadActive = false;
    LOG_SERIAL("[DATA] read complete");
  } else {
    dataReadActive = true; // FS stays mounted and file stays open across NEXT calls
    ledSetBusy(true);
  }
}

/* ---------- DATA:NEXT - send one more chunk of the open read file ---------- */
void handleDataNextRequest() {
  if (!dataReadActive) return;

  uint8_t buf[BLE_CHUNK_SIZE];
  size_t toRead = min((size_t)BLE_CHUNK_SIZE, dataReadRemaining);
  size_t n = dataReadFile.read(buf, toRead);

  notifyBLERaw(buf, n);
  dataReadRemaining -= n;

  if (dataReadRemaining == 0 || n == 0) {
    dataReadFile.close();
    LittleFS.end();
    dataReadActive = false;
    ledSetBusy(false);
    LOG_SERIAL("[DATA] read complete");
  }
}

/* ---------- NVS:READ - load the blob into RAM once, report size, kick off first chunk ---------- */
void handleNvsReadRequest() {
  Preferences prefs;
  prefs.begin(NVS_BLOB_NAMESPACE, true); // read-only
  size_t len = prefs.getBytesLength(NVS_BLOB_KEY);

  if (len == 0) {
    prefs.end();
    notifyBLE("SIZE:0");
    nvsReadActive = false;
    LOG_SERIAL("[NVS] read: empty or missing");
    return;
  }

  nvsReadBuffer = (uint8_t*)malloc(len);
  if (!nvsReadBuffer) {
    prefs.end();
    deviceLog("[NVS] read malloc failed");
    notifyBLE("SIZE:0");
    nvsReadActive = false;
    return;
  }

  size_t got = prefs.getBytes(NVS_BLOB_KEY, nvsReadBuffer, len);
  prefs.end();

  nvsReadRemaining = got;
  nvsReadOffset = 0;
  LOG_SERIAL("[NVS] reading, size=" + String(got));
  notifyBLE("SIZE:" + String(got));

  if (got == 0) {
    free(nvsReadBuffer);
    nvsReadBuffer = nullptr;
    nvsReadActive = false;
  } else {
    nvsReadActive = true; // buffer stays allocated across NEXT calls
    ledSetBusy(true);
  }
}

/* ---------- NVS:NEXT - send one more chunk out of the buffered blob ---------- */
void handleNvsNextRequest() {
  if (!nvsReadActive) return;

  size_t toSend = min((size_t)BLE_CHUNK_SIZE, nvsReadRemaining);
  notifyBLERaw(nvsReadBuffer + nvsReadOffset, toSend);
  nvsReadOffset += toSend;
  nvsReadRemaining -= toSend;

  if (nvsReadRemaining == 0) {
    free(nvsReadBuffer);
    nvsReadBuffer = nullptr;
    nvsReadActive = false;
    ledSetBusy(false);
    LOG_SERIAL("[NVS] read complete");
  }
}

void setup() {
#if ENABLE_SERIAL_PRINT
  Serial.begin(SERIAL_BAUD_RATE);
#endif

  checkDeviceBinding();
  if (deviceLocked) {
    LOG_SERIAL("[SEC] halted");
    return; // BLE/WiFi/OTA never start on a MAC-mismatched clone
  }

  setupStatusLed();

  // No LittleFS.begin() here - the data partition is mounted only when
  // DATA:WRITE/DATA:READ/UPDATE:DATA actually need it, per-operation.
  setupBLE();
}

void loop() {
  if (deviceLocked) {
    return;
  }

  if (wifiRequest) {
    wifiRequest = false;
    deviceLog("WiFi: connecting " + pendingSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(pendingSsid.c_str(), pendingPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      deviceLog("WiFi ok: " + WiFi.localIP().toString());
    } else {
      deviceLog("WiFi fail");
    }
  }

  if (otaRequest) {
    otaRequest = false;
    switch (otaTarget) {
      case OTA_APP:  doOtaApp();  break;
      case OTA_DATA: doOtaData(); break;
      case OTA_NVS:  doOtaNvs();  break;
      default: break;
    }
    otaTarget = OTA_NONE;
  }

  if (dataReadRequest) {
    dataReadRequest = false;
    handleDataReadRequest();
  }

  if (dataNextRequest) {
    dataNextRequest = false;
    handleDataNextRequest();
  }

  if (nvsReadRequest) {
    nvsReadRequest = false;
    handleNvsReadRequest();
  }

  if (nvsNextRequest) {
    nvsNextRequest = false;
    handleNvsNextRequest();
  }
}
