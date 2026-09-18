#include "S3XYStalk.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>

// ============================================================
// S3XY Left Stalk
// ============================================================

static const char* STALK_NAME = "ENH_STLK_L";

static const char* STALK_SERVICE_UUID = "00003D69-87D2-479E-7E45-8551415A6DE1";
static const char* STALK_NOTIFY_UUID = "00003D50-87D2-479E-7E45-8551415A6DE1";
static const char* STALK_ID_UUID = "00003D49-87D2-479E-7E45-8551415A6DE1";

// ============================================================
// Internal state
// ============================================================

static BLEClient* g_client = nullptr;
static BLERemoteService* g_service = nullptr;
static BLERemoteCharacteristic* g_notifyChar = nullptr;
static BLERemoteCharacteristic* g_idChar = nullptr;

static bool g_connected = false;
static bool g_notificationsSubscribed = false;

static uint32_t g_nextScanTime = 0;

static s3xy_stalk_notify_cb_t g_notifyCallback = nullptr;

static Preferences g_prefs;
static String g_savedStalkMac;

// ============================================================
// Utility
// ============================================================

static void cleanupClient() {
  if (g_client) {
    if (g_client->isConnected()) {
      g_client->disconnect();
      delay(150);
    }

    delete g_client;
    g_client = nullptr;
  }

  g_service = nullptr;
  g_notifyChar = nullptr;
  g_idChar = nullptr;
  g_connected = false;
  g_notificationsSubscribed = false;
}

static void loadSavedStalkMac() {
  g_prefs.begin("s3xy", false);
  g_savedStalkMac = g_prefs.getString("stalk_mac", "");
  g_prefs.end();

  if (!g_savedStalkMac.isEmpty()) {
    Serial.printf("[stalk] Loaded saved stalk MAC: %s\n", g_savedStalkMac.c_str());
  }
}

static void saveStalkMac(const BLEAddress& address) {
  String mac = address.toString().c_str();

  if (mac.isEmpty()) {
    return;
  }

  g_savedStalkMac = mac;

  g_prefs.begin("s3xy", false);
  g_prefs.putString("stalk_mac", g_savedStalkMac.c_str());
  g_prefs.end();

  Serial.printf("[stalk] Saved stalk MAC: %s\n", g_savedStalkMac.c_str());
}

static void clearSavedStalkMac() {
  g_savedStalkMac = "";

  g_prefs.begin("s3xy", false);
  g_prefs.remove("stalk_mac");
  g_prefs.end();

  Serial.println("[stalk] Cleared saved stalk MAC");
}

static void printHex(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    Serial.printf("%02X", data[i]);

    if (i + 1 < len)
      Serial.print(" ");
  }
}

// ============================================================
// Notification callback
// ============================================================

static void stalkNotifyCallback(
    BLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t length,
    bool isNotify
) {
  Serial.print("[stalk] RX ");

  if (isNotify)
    Serial.print("notify: ");
  else
    Serial.print("indicate: ");

  printHex(data, length);
  Serial.println();

  if (g_notifyCallback) {
    g_notifyCallback(data, length);
  }
}

// ============================================================
// Client connection callbacks
// ============================================================

class StalkClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* client) override {
    Serial.println("[stalk] Connected");
  }

  void onDisconnect(BLEClient* client) override {
    Serial.println("[stalk] Disconnected");

    g_connected = false;
    g_notificationsSubscribed = false;
    g_service = nullptr;
    g_notifyChar = nullptr;
    g_idChar = nullptr;

    g_nextScanTime = millis() + 1000;
  }
};

// ============================================================
// Find the stalk
// ============================================================

class StalkLocatorCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  explicit StalkLocatorCallbacks(const char* targetAddress = nullptr) {
    if (targetAddress) {
      address_ = targetAddress;
      searchByMac_ = true;
    } else {
      searchByMac_ = false;
    }
  }

  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    bool match = false;

    if (searchByMac_) {
      match = (advertisedDevice.getAddress().toString() == address_);
    } else {
      bool nameMatches =
          advertisedDevice.haveName() &&
          advertisedDevice.getName() == STALK_NAME;

      bool serviceMatches =
          advertisedDevice.haveServiceUUID() &&
          advertisedDevice.isAdvertisingService(BLEUUID(STALK_SERVICE_UUID));

      match = (nameMatches || serviceMatches);
    }

    if (match) {
      if (searchByMac_) {
        Serial.println("[stalk] *** Found stalk by saved MAC ***");
      } else {
        Serial.println("[stalk] *** Found stalk by name & service UUID ***");
      }
      
      result_ = new BLEAdvertisedDevice(advertisedDevice);
      BLEDevice::getScan()->stop(); // Early exit!
    }
  }

  BLEAdvertisedDevice* result() const {
    return result_;
  }

 private:
  String address_;
  bool searchByMac_;
  BLEAdvertisedDevice* result_ = nullptr;
};

static BLEAdvertisedDevice* findStalkByAddress(const char* address) {
  BLEScan* scan = BLEDevice::getScan();

  // Passive scan is enough when looking for a known MAC
  scan->setActiveScan(false); 
  scan->setInterval(100);
  scan->setWindow(80); // 80% duty cycle

  Serial.printf("[stalk] Looking for saved stalk MAC %s\n", address);

  StalkLocatorCallbacks callbacks(address);
  scan->setAdvertisedDeviceCallbacks(&callbacks);

  // Blocks for max 5 seconds; exits instantly if callbacks call stop()
  scan->start(5, false);

  scan->setAdvertisedDeviceCallbacks(nullptr);
  scan->clearResults();
  return callbacks.result();
}

static BLEAdvertisedDevice* findStalk() {
  BLEScan* scan = BLEDevice::getScan();

  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80); // 80% duty cycle

  Serial.println("[stalk] Scanning...");

  StalkLocatorCallbacks callbacks;
  scan->setAdvertisedDeviceCallbacks(&callbacks);

  // Scan up to 5 sec to catch a sleeping stalk's slow background advertisements
  scan->start(5, false);

  scan->setAdvertisedDeviceCallbacks(nullptr);
  scan->clearResults();

  return callbacks.result();
}

// ============================================================
// Connect and subscribe
// ============================================================

static bool connectToStalk() {
  cleanupClient();

  BLEAdvertisedDevice* device = nullptr;

  if (!g_savedStalkMac.isEmpty()) {
    device = findStalkByAddress(g_savedStalkMac.c_str());
  }

  if (!device) {
    device = findStalk();
  }

  if (!device) {
    Serial.println("[stalk] Stalk not found");
    return false;
  }

  Serial.printf(
      "[stalk] Connecting to %s\n",
      device->getAddress().toString().c_str()
  );

  // A scan can still be completing internally when start() returns. Stop it
  // explicitly before asking NimBLE to create a connection.
  BLEDevice::getScan()->stop();
  delay(200);

  g_client = BLEDevice::createClient();

  if (!g_client) {
    Serial.println("[stalk] Failed to create BLE client");
    delete device;
    return false;
  }

  g_client->setClientCallbacks(
      new StalkClientCallbacks()
  );

  if (!g_client->connect(device)) {
    Serial.println("[stalk] Connection failed");

    delete device;
    cleanupClient();
    return false;
  }

  Serial.println("[stalk] GATT connected");

  // ----------------------------------------------------------
  // Find service
  // ----------------------------------------------------------

  g_service =
      g_client->getService(
          BLEUUID(STALK_SERVICE_UUID)
      );

  if (!g_service) {
    Serial.println(
        "[stalk] ERROR: Service 3D69 not found"
    );

    g_client->disconnect();
    cleanupClient();
    delete device;
    return false;
  }

  Serial.println("[stalk] Found service 3D69");

  g_notifyChar = g_service->getCharacteristic(
      BLEUUID(STALK_NOTIFY_UUID)
  );

  if (!g_notifyChar) {
    Serial.println(
        "[stalk] ERROR: Characteristic 3D50 not found"
    );
    g_client->disconnect();
    cleanupClient();
    delete device;
    return false;
  }

  Serial.printf(
      "[stalk] 3D50 handle=%u notify=%d indicate=%d read=%d write=%d\n",
      g_notifyChar->getHandle(),
      g_notifyChar->canNotify(),
      g_notifyChar->canIndicate(),
      g_notifyChar->canRead(),
      g_notifyChar->canWrite()
  );

  if (!g_notifyChar->canNotify()) {
    Serial.println(
        "[stalk] ERROR: 3D50 does not advertise NOTIFY"
    );

    g_client->disconnect();
    cleanupClient();
    delete device;
    return false;
  }

  Serial.println("[stalk] Registering for notifications...");

  g_notifyChar->registerForNotify(
      stalkNotifyCallback,
      true,
      true
  );
  g_notificationsSubscribed = true;

  Serial.println("[stalk] Notification subscription registered");

  g_idChar = g_service->getCharacteristic(
      BLEUUID(STALK_ID_UUID)
  );

  if (!g_idChar ||
      (!g_idChar->canWrite() && !g_idChar->canWriteNoResponse())) {
    Serial.println(
        "[stalk] ERROR: Characteristic 3D49 is not writable"
    );
    g_client->disconnect();
    cleanupClient();
    delete device;
    return false;
  }

  saveStalkMac(device->getAddress());
  delete device;

  g_connected = true;
  return true;
}

// ============================================================
// Public API
// ============================================================

void s3xy_stalk_begin() {

  Serial.println("[stalk] Beginning central...");

  // IMPORTANT:
  // Do NOT call BLEDevice::init() here.
  //
  // S3XYButton already initialized the global BLE stack.
  // We simply create a BLEClient from that same stack.

  loadSavedStalkMac();
  g_nextScanTime = 0;
}

void s3xy_stalk_loop() {

  if (g_connected) {

    if (!g_client || !g_client->isConnected()) {
      cleanupClient();
    } else {
      return;
    }
  }

  if (millis() < g_nextScanTime)
    return;

  // Backoff timer
  if (!connectToStalk()) {
    Serial.println("[stalk] Not found. Will retry in 5 seconds...");
    g_nextScanTime = millis() + 5000;
  } else {
    g_nextScanTime = millis();
  }
}

bool s3xy_stalk_connected() {
  return s3xy_stalk_ready();
}

bool s3xy_stalk_ready() {
  return g_connected && g_notificationsSubscribed;
}

void s3xy_stalk_on_notify(
    s3xy_stalk_notify_cb_t cb
) {
  g_notifyCallback = cb;
}
