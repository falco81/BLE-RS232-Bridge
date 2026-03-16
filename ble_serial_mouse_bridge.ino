// ble_serial_mouse_bridge.ino  v2.0
// BLE Mouse -> RS-232 Serial Mouse Bridge
//
// Hardware: ESP32 WROOM + MAX232
//   GPIO16 -> MAX232 T1IN -> DB9 pin 2 (RD)   TX data to PC
//   GPIO23 <- MAX232 R1OUT <- DB9 pin 7 (RTS) identification from PC
//   GPIO5  <- MAX232 R2OUT <- DB9 pin 4 (DTR) identification from PC
//   GND -- DB9 pin 5 (GND)
//
// Protocols (command "proto M|M3|MZ"):
//   M   = Microsoft 2-button   3B/packet             default for ctmouse
//   M3  = Logitech 3-button    4B/packet  byte3=MB
//   MZ  = IntelliMouse wheel   4B/packet  byte3=MB+wheel  (default)
//
// Dependencies: NimBLE-Arduino >= 1.4.2

#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include "driver/gpio.h"

// ── Piny ──────────────────────────────────────────────────────────────────────
#define TX_PIN   16
#define RTS_PIN  23
#define DTR_PIN   5   // DB9 pin 4 → MAX232 R2OUT → GPIO5

// ── Protokol ──────────────────────────────────────────────────────────────────
typedef enum { PROTO_MS = 0, PROTO_LOGITECH = 1, PROTO_WHEEL = 2 } Protocol;
static volatile Protocol g_proto = PROTO_WHEEL;

// ── Pohyb ─────────────────────────────────────────────────────────────────────
static volatile int  g_scaleDivisor = 4;
static volatile bool g_flipY        = false;
static volatile bool g_flipW        = false;   // scroll wheel inversion

// ── BLE UUID ──────────────────────────────────────────────────────────────────
static BLEUUID HID_SVC_UUID ("00001812-0000-1000-8000-00805f9b34fb");
static BLEUUID HID_RPT_UUID ("00002a4d-0000-1000-8000-00805f9b34fb");
static BLEUUID BAT_SVC_UUID ("0000180f-0000-1000-8000-00805f9b34fb");
static BLEUUID BAT_LVL_UUID ("00002a19-0000-1000-8000-00805f9b34fb");
static BLEUUID RPT_REF_UUID ("00002908-0000-1000-8000-00805f9b34fb");

// ── BLE stav ──────────────────────────────────────────────────────────────────
static NimBLEClient* pClient             = nullptr;
static NimBLERemoteCharacteristic* pBatChar = nullptr;
static unsigned long reconnectAt         = 0;
static int           reconnectFailures   = 0;
static bool          reconnectScanActive = false;
static Preferences   prefs;
static char          savedMAC[18]        = "";
static uint8_t       savedType           = 1;
static int           batteryLevel        = -1;  // -1 = unknown, 0-100 = percent
static unsigned long keepaliveAt         = 0;
#define KEEPALIVE_MS 5000
static unsigned long scanEndAt           = 0;
static int           scanCount           = 0;

// ── Shared mouse state ─────────────────────────────────────────────────────────
static portMUX_TYPE    g_mux      = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t g_accX   = 0;
static volatile int32_t g_accY   = 0;
static volatile int32_t g_accW   = 0;   // scroll wheel accumulator
static volatile uint8_t g_buttons = 0;  // bit0=L bit1=R bit2=M
static volatile bool    g_dirty  = false;
static uint8_t g_prevButtons     = 0;
static volatile bool g_identDone   = false;
static volatile bool g_rtsBlackout = false;

// ── Filtr handles (Report Reference 0x2908) ───────────────────────────────────
#define MAX_MOUSE_HANDLES 8
static uint16_t g_mouseHandles[MAX_MOUSE_HANDLES];
static int      g_mouseHandleCount = 0;
static volatile uint8_t g_filterReportId = 0;

static bool isMouseHandle(uint16_t h) {
  for (int i = 0; i < g_mouseHandleCount; i++)
    if (g_mouseHandles[i] == h) return true;
  return false;
}

// Minimum interval between two idents (ms).
// >= sendMouseIdent() blackout (200 ms) — rate limit fires only if a trigger
// slips through before blackout clears.
#define IDENT_MIN_INTERVAL_MS 200
static unsigned long g_lastIdentTime = 0;

// ── RTS/DTR ISR ───────────────────────────────────────────────────────────────
static volatile bool     g_rtsIdentify = false;
static volatile uint32_t g_rtsTime     = 0;

// ── Debug (throttled 500 ms) ──────────────────────────────────────────────────
static unsigned long g_lastMovePrint = 0;
static int32_t  g_dbgX = 0, g_dbgY = 0, g_dbgW = 0;
static uint32_t g_dbgPkts = 0;

// =============================================================================
//  BLE HID MOUSE REPORT PARSER
// =============================================================================
//
//  [3B]  btn | dx8 | dy8
//  [4B]  btn | dx8 | dy8 | wheel
//  [5B]  btn | dx8 | dy8 | wheel | hwheel    (HID_MOUSE_IN_RPT_LEN = 5)
//  [7B]  Logitech 12-bit packed:
//        btn | extra | X_lo | X_hi|Y_lo | Y_hi | wheel | hwheel
//        X = d[2] | ((d[3]&0x0F)<<8)  signed 12-bit
//        Y = (d[3]>>4) | (d[4]<<4)    signed 12-bit

static inline int16_t sign12(int32_t v) {
  return (v & 0x800) ? (int16_t)(v - 0x1000) : (int16_t)v;
}

static bool parseBLEReport(const uint8_t* d, size_t len,
                             int16_t& dx, int16_t& dy,
                             int8_t& wheel, uint8_t& buttons)
{
  if (len < 3 || len > 10) return false;
  if (d[0] == 0xFF)        return false;   // Logitech vendor specific

  buttons = d[0] & 0x07;
  wheel   = 0;

  if (len <= 5) {
    dx    = (int8_t)d[1];
    dy    = (int8_t)d[2];
    if (len >= 4) wheel = (int8_t)d[3];
    return true;
  }
  if (len == 7) {
    // Logitech 12-bit packed
    dx    = sign12((int32_t)d[2] | (((int32_t)d[3] & 0x0F) << 8));
    dy    = sign12(((int32_t)d[3] >> 4) | ((int32_t)d[4] << 4));
    wheel = (int8_t)d[5];
    return true;
  }
  // fallback 16-bit
  dx = (int16_t)((uint16_t)d[1] | ((uint16_t)d[2] << 8));
  dy = (int16_t)((uint16_t)d[3] | ((uint16_t)d[4] << 8));
  return true;
}

// =============================================================================
//  BIT-BANG RS-232  1200 baud, 7 data bits, 2 stop bits
// =============================================================================
//
//  taskENTER_CRITICAL blocks FreeRTOS/BLE interrupts for precise timing.
//  esp_timer_get_time() = HW timer, works inside critical section.

#define BB_BIT_US 833ULL

static portMUX_TYPE bb_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void bbWait(uint64_t us) {
  uint64_t t = esp_timer_get_time() + us;
  while (esp_timer_get_time() < t) {}
}

static void bbSendByte(uint8_t data) {
  taskENTER_CRITICAL(&bb_mux);
  gpio_set_level((gpio_num_t)TX_PIN, 0);       // start bit
  bbWait(BB_BIT_US);
  for (int i = 0; i < 7; i++) {               // 7 data bits, LSB first
    gpio_set_level((gpio_num_t)TX_PIN, (data >> i) & 1);
    bbWait(BB_BIT_US);
  }
  gpio_set_level((gpio_num_t)TX_PIN, 1);       // stop bit 1
  bbWait(BB_BIT_US);
  gpio_set_level((gpio_num_t)TX_PIN, 1);       // stop bit 2
  bbWait(BB_BIT_US);
  taskEXIT_CRITICAL(&bb_mux);
}

// =============================================================================
//  SERIAL MOUSE PACKET
// =============================================================================
//
//  Byte0: 1(sync) | LB | RB | Y[7:6] | X[7:6]
//  Byte1: 0 | X[5:0]
//  Byte2: 0 | Y[5:0]
//
//  M  → 3 bajty
//  M3 → 4 bajty, byte3: bit5=MB
//  MZ → 4 bajty, byte3: bit5=MB | bits[3:0]=wheel signed 4-bit (-8..+7)

static void sendSerialPacket(int8_t dx, int8_t dy, int8_t dw, uint8_t buttons)
{
  bool LB = buttons & 0x01;
  bool RB = buttons & 0x02;
  bool MB = buttons & 0x04;

  bbSendByte(0x40 | (LB?0x20:0) | (RB?0x10:0)
             | (((uint8_t)dy >> 6) & 0x03) << 2
             | (((uint8_t)dx >> 6) & 0x03));
  bbSendByte((uint8_t)dx & 0x3F);
  bbSendByte((uint8_t)dy & 0x3F);

  if (g_proto == PROTO_LOGITECH) {
    bbSendByte(MB ? 0x20 : 0x00);
  } else if (g_proto == PROTO_WHEEL) {
    int8_t wc = (int8_t)constrain(dw, -8, 7);
    bbSendByte((MB ? 0x20 : 0) | (wc & 0x0F));
  }
}

// =============================================================================
//  IDENTIFIKACE
// =============================================================================

static void sendMouseIdent()
{
  g_rtsBlackout = true;
  gpio_set_level((gpio_num_t)TX_PIN, 1);
  delay(10);

  bbSendByte('M');
  if (g_proto == PROTO_LOGITECH) bbSendByte('3');
  if (g_proto == PROTO_WHEEL)    bbSendByte('Z');

  g_identDone = true;
  const char* s = (g_proto==PROTO_WHEEL) ? "MZ" : (g_proto==PROTO_LOGITECH) ? "M3" : "M";
  Serial.printf("[SERIAL] Ident '%s'\n", s);

  // Long blackout: block movement data AND new ident triggers for 200 ms.
  // mouse.com toggles RTS/DTR multiple times during init — without this,
  // each toggle fires a new ident which desynchronises the driver.
  delay(200);
  g_rtsIdentify = false;  // discard any triggers that arrived during blackout
  g_rtsBlackout = false;
}

// =============================================================================
//  MOVEMENT PROCESSING
// =============================================================================

static void processMouseMovement()
{
  if (!g_identDone || g_rtsBlackout) return;
  if (!g_dirty && g_buttons == g_prevButtons) return;

  int32_t ax, ay, aw;
  uint8_t btns;
  portENTER_CRITICAL(&g_mux);
    ax = g_accX; ay = g_accY; aw = g_accW;
    btns = g_buttons; g_dirty = false;
  portEXIT_CRITICAL(&g_mux);

  int div = (g_scaleDivisor < 1) ? 1 : g_scaleDivisor;
  int32_t sx = ax/div, remX = ax - sx*div;
  int32_t sy = ay/div, remY = ay - sy*div;
  int32_t sw = aw;     // wheel is not scaled

  portENTER_CRITICAL(&g_mux);
    g_accX = remX; g_accY = remY; g_accW = 0;
  portEXIT_CRITICAL(&g_mux);

  if (g_flipY) sy = -sy;
  if (g_flipW) sw = -sw;
  if (sx == 0 && sy == 0 && sw == 0 && btns == g_prevButtons) return;

  do {
    int8_t px = (int8_t)constrain(sx, -127, 127);
    int8_t py = (int8_t)constrain(sy, -127, 127);
    int8_t pw = (int8_t)constrain(sw, -8,   7);
    sendSerialPacket(px, py, pw, btns);
    g_dbgX += px; g_dbgY += py; g_dbgW += pw; g_dbgPkts++;
    sx -= px; sy -= py; sw -= pw;
    g_prevButtons = btns;
    if (!sx && !sy && !sw) break;
  } while (sx || sy || sw);

  unsigned long now = millis();
  if (now - g_lastMovePrint >= 500 && g_dbgPkts > 0) {
    Serial.printf("[MOVE] pkts=%lu X=%d Y=%d W=%d btn=%d%d%d\n",
      g_dbgPkts, (int)g_dbgX, (int)g_dbgY, (int)g_dbgW,
      btns&1, (btns>>1)&1, (btns>>2)&1);
    g_dbgX = g_dbgY = g_dbgW = 0; g_dbgPkts = 0;
    g_lastMovePrint = now;
  }
}

// =============================================================================
//  BLE CALLBACK
// =============================================================================

static void notifyCallback(NimBLERemoteCharacteristic* pChar,
                            uint8_t* pData, size_t length, bool)
{
  if (g_mouseHandleCount > 0 && !isMouseHandle(pChar->getHandle())) return;

  int16_t dx = 0, dy = 0; int8_t dw = 0; uint8_t btns = 0;
  if (!parseBLEReport(pData, length, dx, dy, dw, btns)) {
    Serial.printf("[BLE] unknown len=%d %02X %02X %02X %02X\n",
      (int)length, pData[0], length>1?pData[1]:0,
      length>2?pData[2]:0, length>3?pData[3]:0);
    return;
  }

  portENTER_CRITICAL(&g_mux);
    uint8_t prev = g_buttons;
    g_accX += dx; g_accY += dy; g_accW += dw;
    g_buttons = btns; g_dirty = true;
  portEXIT_CRITICAL(&g_mux);

  if (btns != prev)
    Serial.printf("[BTN] L=%d R=%d M=%d\n", btns&1, (btns>>1)&1, (btns>>2)&1);
}

// =============================================================================
//  RTS / DTR ISR
// =============================================================================
//
//  MAX232 invertuje: RS-232 +12V (asserted) -> GPIO LOW (FALLING = trigger)
//
//  ISR se spustí na CHANGE (obě hrany). Reagujeme jen na FALLING (LOW).
//  Pokud obě linky přejdou do idle před uplynutím 14 ms v loop(),
//  jde o bounce/rušení a ident se nepošle.

static void IRAM_ATTR rtsISR() {
  if (digitalRead(RTS_PIN) == LOW) {
    g_rtsIdentify = true;
    g_rtsTime = (uint32_t)millis();
  }
}

#if DTR_PIN > 0
static void IRAM_ATTR dtrISR() {
  if (digitalRead(DTR_PIN) == LOW && !g_rtsIdentify) {
    g_rtsIdentify = true;
    g_rtsTime = (uint32_t)millis();
  }
}
#endif

// =============================================================================
//  BLE SCAN
// =============================================================================

static const char* appName(uint16_t a) {
  switch (a) {
    case 0x03C1: return "Keyboard";
    case 0x03C2: return "Mouse";
    case 0x03C3: return "Joystick";
    case 0x03C4: return "Gamepad";
    default:     return "HID";
  }
}

class MyScanCallbacks : public NimBLEScanCallbacks {
  void onDiscovered(const NimBLEAdvertisedDevice* dev) override {
    if (!reconnectScanActive || !strlen(savedMAC)) return;
    if (dev->getAddress().toString() == std::string(savedMAC)) {
      Serial.println("[SCAN] Found — connecting...");
      NimBLEDevice::getScan()->stop();
      reconnectScanActive = false;
      reconnectAt = millis() + 50;
    }
  }
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (reconnectScanActive) return;
    if (!dev->haveServiceUUID() || !dev->isAdvertisingService(HID_SVC_UUID)) return;
    const char* app  = dev->haveAppearance() ? appName(dev->getAppearance()) : "HID";
    const char* name = (dev->haveName() && dev->getName().length()) ? dev->getName().c_str() : "-";
    Serial.printf("  #%-2d  %-17s  %-10s  %4d dBm  %s\n",
      scanCount+1, dev->getAddress().toString().c_str(), app, dev->getRSSI(), name);
    scanCount++;
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    if (reconnectScanActive) { reconnectScanActive = false; reconnectAt = millis()+500; }
  }
};

// =============================================================================
//  BLE CONNECT
// =============================================================================

static bool tryConnect(NimBLEAddress addr)
{
  if (pClient) {
    if (pClient->isConnected()) pClient->disconnect();
    NimBLEDevice::deleteClient(pClient); pClient = nullptr;
    delay(200);
  }
  pClient = NimBLEDevice::createClient();
  pClient->setConnectionParams(6, 12, 0, 3200);
  pClient->setConnectTimeout(12);

  Serial.printf("[BLE] Connecting %s ...\n", addr.toString().c_str());
  if (!pClient->connect(addr)) {
    NimBLEDevice::deleteClient(pClient); pClient = nullptr;
    return false;
  }
  if (pClient->secureConnection()) Serial.println("[BLE] Bonding OK");

  NimBLERemoteService* svc = pClient->getService(HID_SVC_UUID);
  if (!svc) {
    pClient->disconnect(); NimBLEDevice::deleteClient(pClient); pClient = nullptr;
    return false;
  }

  // Subscribe Input reports (descriptor 0x2908); skip ReportID=0 (boot proto)
  g_mouseHandleCount = 0; int subs = 0;
  const std::vector<NimBLERemoteCharacteristic*>& chars = svc->getCharacteristics(&HID_RPT_UUID);

  for (NimBLERemoteCharacteristic* c : chars) {
    if (!c->canNotify()) continue;
    uint8_t id = 0xFF, type = 1;
    NimBLERemoteDescriptor* ref = c->getDescriptor(RPT_REF_UUID);
    if (ref) {
      std::string v = ref->readValue();
      if (v.size() >= 2) { id = v[0]; type = v[1]; }
    }
    bool skip = (type != 1) || (id == 0) || (g_filterReportId && id != g_filterReportId);
    Serial.printf("[BLE] handle=0x%04X  ID=%-3d  Type=%d  %s\n",
      c->getHandle(), id, type, skip ? "skip" : "SUBSCRIBE");
    if (skip) continue;
    c->subscribe(true, notifyCallback); subs++;
    if (g_mouseHandleCount < MAX_MOUSE_HANDLES)
      g_mouseHandles[g_mouseHandleCount++] = c->getHandle();
  }

  if (!subs) {  // fallback
    Serial.println("[BLE] Fallback: all notifiable chars");
    for (NimBLERemoteCharacteristic* c : chars) {
      if (!c->canNotify()) continue;
      c->subscribe(true, notifyCallback); subs++;
      if (g_mouseHandleCount < MAX_MOUSE_HANDLES)
        g_mouseHandles[g_mouseHandleCount++] = c->getHandle();
    }
  }
  if (!subs) {
    pClient->disconnect(); NimBLEDevice::deleteClient(pClient); pClient = nullptr;
    return false;
  }

  // Battery
  batteryLevel = -1; pBatChar = nullptr;
  NimBLERemoteService* bs = pClient->getService(BAT_SVC_UUID);
  if (bs) {
    NimBLERemoteCharacteristic* bc = bs->getCharacteristic(BAT_LVL_UUID);
    if (bc) {
      // Initial read
      if (bc->canRead()) {
        std::string v = bc->readValue();
        if (!v.empty()) batteryLevel = (int)(uint8_t)v[0];
      }
      // Subscribe to notifications for live updates
      if (bc->canNotify())
        bc->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
          if (l > 0) batteryLevel = (int)d[0];
        });
      // Save for keepalive reads (only if device supports read)
      if (bc->canRead()) pBatChar = bc;
    }
  }
  keepaliveAt = millis() + KEEPALIVE_MS;
  Serial.printf("[BLE] Connected — %d char(s), battery %d%%\n", subs, batteryLevel);

  portENTER_CRITICAL(&g_mux);
    g_accX = g_accY = g_accW = 0; g_buttons = 0; g_dirty = false;
  portEXIT_CRITICAL(&g_mux);
  g_prevButtons = 0;
  return true;
}

// =============================================================================
//  KONZOLE
// =============================================================================

// =============================================================================
//  NVS — save / load settings
// =============================================================================

static void saveSettings() {
  prefs.begin("ble_mouse", false);
  prefs.putUChar("proto",    (uint8_t)g_proto);
  prefs.putInt  ("scale",    g_scaleDivisor);
  prefs.putBool ("flipy",    g_flipY);
  prefs.putBool ("flipw",    g_flipW);
  prefs.putUChar("reportid", g_filterReportId);
  prefs.end();
}

static void loadSettings() {
  prefs.begin("ble_mouse", true);
  g_proto          = (Protocol)prefs.getUChar("proto",    PROTO_WHEEL);
  g_scaleDivisor   = prefs.getInt  ("scale",    4);
  g_flipY          = prefs.getBool ("flipy",    false);
  g_flipW          = prefs.getBool ("flipw",    false);
  g_filterReportId = prefs.getUChar("reportid", 0);
  prefs.end();
  if (g_proto > PROTO_WHEEL) g_proto = PROTO_WHEEL;
  if (g_scaleDivisor < 1 || g_scaleDivisor > 64) g_scaleDivisor = 4;
}

static void printHelp() {
  Serial.println("\n============================================");
  Serial.println("  BLE -> RS-232 Serial Mouse Bridge  v2.0");
  Serial.printf ("  TX=GPIO%d  RTS=GPIO%d  DTR=GPIO%d\n", TX_PIN, RTS_PIN, DTR_PIN);
  Serial.println("============================================");
  Serial.println();
  Serial.println("  scan");
  Serial.println("    Scans for BLE HID devices for 10 seconds.");
  Serial.println("    Shows MAC address, type, RSSI and name.");
  Serial.println();
  Serial.println("  connect <mac>");
  Serial.println("    Connects and saves a BLE mouse by MAC address.");
  Serial.println("    Example: connect aa:bb:cc:dd:ee:ff");
  Serial.println("    MAC is saved to NVS — ESP32 reconnects");
  Serial.println("    automatically after restart.");
  Serial.println();
  Serial.println("  forget");
  Serial.println("    Erases saved mouse and ALL settings from NVS.");
  Serial.println("    Resets everything to defaults:");
  Serial.println("    proto=MZ  scale=4  flipy=off  flipw=off  reportid=0");
  Serial.println();
  Serial.println("  proto <M|M3|MZ>");
  Serial.println("    Sets the serial mouse protocol:");
  Serial.println("    M  = Microsoft 2-button, 3 bytes/packet");
  Serial.println("         Ident: 'M'  — widest compatibility");
  Serial.println("    M3 = Logitech 3-button, 4 bytes/packet");
  Serial.println("         Ident: 'M3' — adds middle button");
  Serial.println("    MZ = IntelliMouse wheel, 4 bytes/packet");
  Serial.println("         Ident: 'MZ' — wheel + middle button");
  Serial.println("         (default, requires ctmouse >= 3.4)");
  Serial.println("    After change: reload driver on PC (CTMOUSE /U).");
  Serial.println("    Saved to NVS.");
  Serial.println();
  Serial.println("  scale <1-64>");
  Serial.println("    Movement divisor. Raw BLE delta is divided by");
  Serial.println("    this value before sending over RS-232.");
  Serial.println("    Low value = fast cursor, high = slow.");
  Serial.println("    Recommended by mouse DPI:");
  Serial.println("      400 DPI  -> scale 1-2");
  Serial.println("      800 DPI  -> scale 2-3  (default: 4)");
  Serial.println("     1600 DPI  -> scale 4-6");
  Serial.println("     3200 DPI  -> scale 8-12");
  Serial.println("    Saved to NVS.");
  Serial.println();
  Serial.println("  flipy");
  Serial.println("    Toggles Y-axis inversion (up/down).");
  Serial.println("    Default: off. Saved to NVS.");
  Serial.println();
  Serial.println("  flipw");
  Serial.println("    Toggles scroll wheel inversion.");
  Serial.println("    Useful if scroll direction is reversed.");
  Serial.println("    Default: off. Saved to NVS.");
  Serial.println();
  Serial.println("  reportid <0-255>");
  Serial.println("    BLE HID Report ID filter. Value 0 subscribes");
  Serial.println("    to all Input reports (except boot proto ID=0).");
  Serial.println("    If mouse does not respond, set a specific ID:");
  Serial.println("    MX Master 2/3: reportid 17");
  Serial.println("    Standard BT mice: reportid 1 or 3");
  Serial.println("    After change: reconnect (forget + connect).");
  Serial.println("    Saved to NVS.");
  Serial.println();
  Serial.println("  testm");
  Serial.println("    Manually sends ident sequence ('M', 'M3' or");
  Serial.println("    'MZ') without waiting for RTS/DTR edge.");
  Serial.println("    Useful for testing while driver is waiting.");
  Serial.println();
  Serial.println("  status");
  Serial.println("    Shows current state: BLE connection, MAC,");
  Serial.println("    battery level, protocol and all settings.");
  Serial.println();
  Serial.println("  help / ?");
  Serial.println("    This help.");
  Serial.println("============================================\n");
}

static void cmdScan() {
  if (scanEndAt) { Serial.println("[SCAN] Already running..."); return; }
  NimBLEDevice::getScan()->stop(); delay(200);
  scanCount = 0;
  Serial.println("[SCAN] 10 s — HID devices only...");
  Serial.println("  #    MAC                Type        RSSI    Name");
  Serial.println("  ─────────────────────────────────────────────────");
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setActiveScan(true); sc->setInterval(50); sc->setWindow(45);
  sc->start(0, false);
  scanEndAt = millis() + 10000;
}

static bool connectWithRetry(NimBLEAddress addr, int tries) {
  for (int i = 1; i <= tries; i++) {
    Serial.printf("[BLE] Attempt %d/%d\n", i, tries);
    if (tryConnect(addr)) return true;
    Serial.println("[BLE] Failed.");
    if (i < tries) delay(1500);
  }
  return false;
}

static void cmdStatus() {
  const char* ps = (g_proto==PROTO_WHEEL) ? "MZ (wheel)" :
                   (g_proto==PROTO_LOGITECH) ? "M3 (3-button)" : "M (Microsoft)";
  Serial.println("── Status ─────────────────────────────");
  Serial.printf("  BLE:      %s\n", (pClient&&pClient->isConnected())?"CONNECTED":"disconnected");
  Serial.printf("  MAC:      %s\n", strlen(savedMAC)?savedMAC:"(none)");
  Serial.printf("  Battery:  %s\n", batteryLevel >= 0 ? (String(batteryLevel) + "%").c_str() : "unknown");
  Serial.printf("  Proto:    %s\n", ps);
  Serial.printf("  Scale:    1/%d\n", (int)g_scaleDivisor);
  Serial.printf("  FlipY:    %s\n", g_flipY?"yes":"no");
  Serial.printf("  FlipW:    %s\n", g_flipW?"yes":"no");
  Serial.printf("  ReportID: %d%s\n", (int)g_filterReportId, g_filterReportId?"":" (auto)");
  Serial.println("───────────────────────────────────────");
}

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;

  if      (line.equalsIgnoreCase("scan"))   { cmdScan(); }
  else if (line.startsWith("connect"))      {
    String mac = line.substring(8); mac.trim();
    if (mac.length() < 11) { Serial.println("Usage: connect xx:xx:xx:xx:xx:xx"); return; }
    if (scanEndAt) { NimBLEDevice::getScan()->stop(); scanEndAt = 0; }
    reconnectAt = 0; reconnectFailures = 0;
    NimBLEDevice::deleteAllBonds();
    if (connectWithRetry(NimBLEAddress(mac.c_str(), 1), 3)) {
      strncpy(savedMAC, pClient->getPeerAddress().toString().c_str(), sizeof(savedMAC)-1);
      savedType = pClient->getPeerAddress().getType();
      prefs.begin("ble_mouse", false);
      prefs.putString("mac", savedMAC);
      prefs.putUChar("type", savedType);
      prefs.end();
      saveSettings();
      Serial.printf("[NVS] Saved: %s\n", savedMAC);
    } else Serial.println("[BLE] All attempts failed.");
  }
  else if (line.equalsIgnoreCase("forget")) {
    // Erase mouse and all settings — reset to defaults
    prefs.begin("ble_mouse", false); prefs.clear(); prefs.end();
    savedMAC[0] = 0;
    g_proto          = PROTO_WHEEL;
    g_scaleDivisor   = 4;
    g_flipY          = false;
    g_flipW          = false;
    g_filterReportId = 0;
    if (pClient && pClient->isConnected()) pClient->disconnect();
    NimBLEDevice::deleteAllBonds();
    Serial.println("[NVS] Cleared — all settings reset to defaults.");
    Serial.println("      proto=MZ  scale=4  flipy=off  flipw=off  reportid=0");
  }
  else if (line.startsWith("proto")) {
    String p = line.substring(6); p.trim(); p.toUpperCase();
    if      (p=="M")  { g_proto=PROTO_MS;       Serial.println("[CFG] Proto: M (Microsoft 3-byte)"); }
    else if (p=="M3") { g_proto=PROTO_LOGITECH;  Serial.println("[CFG] Proto: M3 (Logitech 4-byte)"); }
    else if (p=="MZ") { g_proto=PROTO_WHEEL;     Serial.println("[CFG] Proto: MZ (wheel 4-byte)"); }
    else { Serial.println("[CFG] Usage: proto M | M3 | MZ"); return; }
    saveSettings();
    Serial.println("       Saved. Reload mouse driver on PC to apply.");
  }
  else if (line.startsWith("scale")) {
    int n = line.substring(6).toInt();
    if (n>=1 && n<=64) {
      g_scaleDivisor = n;
      saveSettings();
      Serial.printf("[CFG] Scale 1/%d — saved.\n", n);
    } else Serial.println("[CFG] scale must be 1-64");
  }
  else if (line.startsWith("reportid")) {
    int n = line.substring(9).toInt();
    if (n>=0 && n<=255) {
      g_filterReportId = n;
      saveSettings();
      Serial.printf("[CFG] ReportID: %d — saved. Reconnect to apply.\n", n);
    } else Serial.println("[CFG] reportid must be 0-255");
  }
  else if (line.equalsIgnoreCase("flipy")) {
    g_flipY = !g_flipY;
    saveSettings();
    Serial.printf("[CFG] FlipY: %s — saved.\n", g_flipY?"ON":"OFF");
  }
  else if (line.equalsIgnoreCase("flipw")) {
    g_flipW = !g_flipW;
    saveSettings();
    Serial.printf("[CFG] FlipW: %s — saved.\n", g_flipW?"ON":"OFF");
  }
  else if (line.equalsIgnoreCase("testm")) {
    Serial.println("[TEST] Sending ident manually..."); sendMouseIdent();
  }
  else if (line.equalsIgnoreCase("status"))         { cmdStatus(); }
  else if (line.equalsIgnoreCase("help")||line=="?") { printHelp(); }
  else Serial.printf("[CMD] Unknown: '%s'\n", line.c_str());
}

// =============================================================================
//  BLE DAEMON
// =============================================================================

static void bleDaemonTask(void* arg) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (!strlen(savedMAC)||(pClient&&pClient->isConnected())||reconnectAt) continue;
    Serial.println("[DAEMON] Connection lost — scheduling reconnect...");
    portENTER_CRITICAL(&g_mux);
      g_accX=g_accY=g_accW=0; g_buttons=0; g_dirty=false;
    portEXIT_CRITICAL(&g_mux);
    g_prevButtons = 0;
    if (!reconnectScanActive) {
      reconnectScanActive = true;
      NimBLEDevice::getScan()->start(5000, false);
    }
  }
}

// =============================================================================
//  SETUP
// =============================================================================

void setup()
{
  Serial.begin(115200);
  delay(200);
  esp_wifi_stop();
  esp_wifi_deinit();

  // TX bit-bang, idle = HIGH (mark)
  gpio_set_direction((gpio_num_t)TX_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)TX_PIN, 1);

  // RTS — FALLING = asserted = ident trigger
  pinMode(RTS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(RTS_PIN), rtsISR, CHANGE);

#if DTR_PIN > 0
  pinMode(DTR_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(DTR_PIN), dtrISR, CHANGE);
#endif

  // Boot ident — in case PC was powered on before ESP32
  delay(14);
  sendMouseIdent();

  if (digitalRead(RTS_PIN) == LOW)
    { g_rtsIdentify=true; g_rtsTime=millis(); }
#if DTR_PIN > 0
  if (digitalRead(DTR_PIN) == LOW)
    { g_rtsIdentify=true; g_rtsTime=millis(); }
#endif

  NimBLEDevice::init("BLE-Serial-Mouse");
  NimBLEDevice::setPower(9);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::getScan()->setScanCallbacks(new MyScanCallbacks(), false);
  xTaskCreatePinnedToCore(bleDaemonTask, "ble_daemon", 4096, nullptr, 1, nullptr, 0);

  prefs.begin("ble_mouse", true);
  String mac = prefs.getString("mac", "");
  savedType   = prefs.getUChar("type", 1);
  prefs.end();
  loadSettings();
  if (mac.length()) {
    strncpy(savedMAC, mac.c_str(), sizeof(savedMAC)-1);
    reconnectAt = millis() + 1000;
  }
  Serial.printf("[NVS] Saved mouse: %s\n", strlen(savedMAC) ? savedMAC : "(none)");
  Serial.printf("[NVS] proto=%s  scale=1/%d  flipy=%s  flipw=%s  reportid=%d\n",
    g_proto==PROTO_WHEEL?"MZ":g_proto==PROTO_LOGITECH?"M3":"M",
    (int)g_scaleDivisor, g_flipY?"on":"off", g_flipW?"on":"off", (int)g_filterReportId);

  printHelp();
}

// =============================================================================
//  LOOP
// =============================================================================



void loop()
{
  handleSerial();

  // RTS/DTR ident — wait 14 ms after edge, then verify at least one line is asserted
  if (g_rtsIdentify && (millis() - g_rtsTime >= 14)) {
    g_rtsIdentify = false;

    bool rts = (digitalRead(RTS_PIN) == LOW);
#if DTR_PIN > 0
    bool dtr = (digitalRead(DTR_PIN) == LOW);
#else
    bool dtr = false;
#endif

    if (millis() - g_lastIdentTime < IDENT_MIN_INTERVAL_MS) {
      // Too soon after last ident — suppress
      Serial.println("[IDENT] Suppressed — too soon after last ident");
    } else {
      Serial.printf("[IDENT] RTS=%s DTR=%s\n",
        rts ? "ASSERT" : "idle", dtr ? "ASSERT" : "idle");
      portENTER_CRITICAL(&g_mux);
        g_accX=g_accY=g_accW=0; g_buttons=0; g_dirty=false;
      portEXIT_CRITICAL(&g_mux);
      g_prevButtons = 0;
      g_lastIdentTime = millis();
      sendMouseIdent();
    }
  }

  if (scanEndAt && millis() >= scanEndAt) {
    scanEndAt = 0;
    NimBLEDevice::getScan()->stop();
    Serial.printf("[SCAN] Done — %d device(s).\n", scanCount);
  }

  if (pClient && !pClient->isConnected() && strlen(savedMAC) && !reconnectAt) {
    Serial.println("[BLE] Disconnected.");
    NimBLEDevice::deleteClient(pClient); pClient = nullptr;
    portENTER_CRITICAL(&g_mux);
      g_accX=g_accY=g_accW=0; g_buttons=0; g_dirty=false;
    portEXIT_CRITICAL(&g_mux);
    g_prevButtons=0; reconnectFailures=0;
    reconnectScanActive=true;
    NimBLEDevice::getScan()->start(5000, false);
  }

  if (reconnectAt && millis() >= reconnectAt && strlen(savedMAC)) {
    reconnectAt = 0;
    if (!pClient || !pClient->isConnected()) {
      if (scanEndAt) { NimBLEDevice::getScan()->stop(); scanEndAt=0; }
      if (tryConnect(NimBLEAddress(savedMAC, savedType))) {
        reconnectFailures = 0;
      } else {
        Serial.printf("[BLE] Connect failed (%dx)\n", ++reconnectFailures);
        reconnectScanActive = true;
        NimBLEDevice::getScan()->start(5000, false);
      }
    }
  }

  if (keepaliveAt && millis()>=keepaliveAt && pBatChar && pClient && pClient->isConnected()) {
    keepaliveAt = millis() + KEEPALIVE_MS;
    std::string v = pBatChar->readValue();
    if (!v.empty()) batteryLevel = (int)(uint8_t)v[0];
  }

  processMouseMovement();
  delay(1);
}
