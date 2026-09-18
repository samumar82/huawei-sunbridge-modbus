#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <time.h>
#include <vector>

#ifndef SUNBRIDGE_VERSION
#define SUNBRIDGE_VERSION "dev"
#endif

static const IPAddress LOCAL_IP(192, 168, 10, 27);
static const IPAddress GATEWAY(192, 168, 10, 1);
static const IPAddress SUBNET(255, 255, 255, 0);
static const IPAddress DNS1(192, 168, 10, 1);
static const IPAddress SDONGLE_IP(192, 168, 10, 2);

static constexpr uint16_t SDONGLE_PORT = 502;
static constexpr uint16_t SUNBRIDGE_PORT = 5502;
static constexpr uint8_t DEVICE_ID = 1;

// WT32-ETH01 v1.4 / LAN8720 wiring.
// Names intentionally avoid Arduino-ESP32 ETH_* macros.
static constexpr uint8_t SB_ETH_PHY_ADDR = 1;
static constexpr int SB_ETH_PHY_POWER = 16;
static constexpr int SB_ETH_MDC_PIN = 23;
static constexpr int SB_ETH_MDIO_PIN = 18;
static constexpr eth_phy_type_t SB_ETH_PHY_TYPE = ETH_PHY_LAN8720;
static constexpr eth_clock_mode_t SB_ETH_CLK_MODE = ETH_CLOCK_GPIO0_IN;

static constexpr uint32_t POLL_INTERVAL_MS = 10000;
static constexpr uint32_t CONNECT_WAIT_MS = 2000;
static constexpr uint32_t BATCH_DELAY_MS = 500;
static constexpr uint32_t UPSTREAM_TIMEOUT_MS = 5000;
static constexpr size_t MAX_CLIENTS = 4;
static constexpr size_t MAX_HISTORY = 10;

struct RegisterBatch {
  uint16_t start;
  uint16_t count;
};

static const RegisterBatch REGISTER_BATCHES[] = {
  {30000, 15}, {30015, 50}, {30071, 1}, {30075, 2},
  {32000, 20}, {32064, 52}, {37100, 38}, {37200, 2},
  {37760, 28}, {40126, 2}, {42900, 10}, {43006, 2},
  {47000, 1}, {47089, 1}
};

static constexpr size_t BATCH_COUNT = sizeof(REGISTER_BATCHES) / sizeof(REGISTER_BATCHES[0]);
static constexpr uint16_t SETUP_PASSTHROUGH_START = 30000;
static constexpr uint16_t SETUP_PASSTHROUGH_END = 30071;

struct CacheEntry {
  uint16_t reg;
  uint16_t value;
};

struct ClientSlot {
  WiFiClient client;
  bool used = false;
  IPAddress ip;
  uint16_t port = 0;
  time_t connectedAt = 0;
  uint32_t connectedMillis = 0;
  time_t lastRequestAt = 0;
  uint32_t requestCount = 0;
  std::vector<uint8_t> rx;
};

struct ClientHistory {
  String ip;
  uint16_t port = 0;
  time_t connectedAt = 0;
  time_t disconnectedAt = 0;
  uint32_t requestCount = 0;
};

std::vector<CacheEntry> cacheEntries;
SemaphoreHandle_t cacheMutex = nullptr;
SemaphoreHandle_t upstreamMutex = nullptr;
ClientSlot clients[MAX_CLIENTS];
ClientHistory history[MAX_HISTORY];
size_t historyCount = 0;
size_t historyHead = 0;

WiFiServer modbusServer(SUNBRIDGE_PORT);
WebServer webServer(80);
HTTPUpdateServer httpUpdater;

volatile bool ethConnected = false;
volatile bool cacheValid = false;
volatile uint32_t lastSuccessfulPollMillis = 0;
volatile uint32_t pollOkBatches = 0;
volatile uint32_t upstreamErrors = 0;

static String formatTime(time_t t) {
  if (t < 1700000000) return "NTP non sincronizzato";
  struct tm x;
  localtime_r(&t, &x);
  char b[32];
  strftime(b, sizeof(b), "%d/%m/%Y %H:%M:%S", &x);
  return String(b);
}

static String formatDuration(uint32_t s) {
  uint32_t d = s / 86400;
  s %= 86400;
  uint32_t h = s / 3600;
  s %= 3600;
  uint32_t m = s / 60;
  s %= 60;
  char b[40];
  if (d) {
    snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu",
             (unsigned long)d, (unsigned long)h,
             (unsigned long)m, (unsigned long)s);
  } else {
    snprintf(b, sizeof(b), "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
  }
  return String(b);
}

static void initCache() {
  for (const auto &b : REGISTER_BATCHES) {
    for (uint16_t i = 0; i < b.count; i++) {
      cacheEntries.push_back({(uint16_t)(b.start + i), 0});
    }
  }
}

static bool cacheGetRange(uint16_t start, uint16_t count, std::vector<uint16_t> &values) {
  values.clear();
  if (!cacheValid) return false;
  if (xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  for (uint16_t i = 0; i < count; i++) {
    bool found = false;
    for (const auto &e : cacheEntries) {
      if (e.reg == (uint16_t)(start + i)) {
        values.push_back(e.value);
        found = true;
        break;
      }
    }
    if (!found) {
      xSemaphoreGive(cacheMutex);
      values.clear();
      return false;
    }
  }

  xSemaphoreGive(cacheMutex);
  return true;
}

static void cacheSetRange(uint16_t start, const std::vector<uint16_t> &values) {
  if (xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  for (size_t i = 0; i < values.size(); i++) {
    for (auto &e : cacheEntries) {
      if (e.reg == (uint16_t)(start + i)) {
        e.value = values[i];
        break;
      }
    }
  }
  xSemaphoreGive(cacheMutex);
}

static bool readExact(WiFiClient &client, uint8_t *buffer, size_t length, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t started = millis();

  while (got < length) {
    while (client.available() && got < length) {
      int r = client.read(buffer + got, length - got);
      if (r > 0) got += (size_t)r;
    }
    if (got >= length) return true;
    if (millis() - started > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

static bool readResponse(WiFiClient &client, uint16_t &tx, uint8_t &uid, std::vector<uint8_t> &pdu) {
  uint8_t header[7];
  if (!readExact(client, header, sizeof(header), UPSTREAM_TIMEOUT_MS)) return false;

  tx = ((uint16_t)header[0] << 8) | header[1];
  uint16_t proto = ((uint16_t)header[2] << 8) | header[3];
  uint16_t len = ((uint16_t)header[4] << 8) | header[5];
  uid = header[6];

  if (proto != 0 || len < 2 || len > 260) return false;
  pdu.resize(len - 1);
  return readExact(client, pdu.data(), pdu.size(), UPSTREAM_TIMEOUT_MS);
}

static bool openUpstream(WiFiClient &client) {
  client.setTimeout(UPSTREAM_TIMEOUT_MS);
  if (!client.connect(SDONGLE_IP, SDONGLE_PORT, 10000)) return false;
  vTaskDelay(pdMS_TO_TICKS(CONNECT_WAIT_MS));
  return true;
}

static bool readOnConnection(WiFiClient &client, uint8_t uid, uint16_t start,
                             uint16_t count, std::vector<uint16_t> &values) {
  uint8_t request[12] = {
    0, 0, 0, 0, 0, 6, uid, 3,
    (uint8_t)(start >> 8), (uint8_t)start,
    (uint8_t)(count >> 8), (uint8_t)count
  };

  if (client.write(request, sizeof(request)) != sizeof(request)) return false;

  uint16_t tx;
  uint8_t responseUid;
  std::vector<uint8_t> pdu;
  if (!readResponse(client, tx, responseUid, pdu)) return false;
  if (pdu.size() < 2 || pdu[0] != 3 || pdu[1] < count * 2) return false;
  if (pdu.size() < (size_t)(2 + pdu[1])) return false;

  values.resize(count);
  for (uint16_t i = 0; i < count; i++) {
    values[i] = ((uint16_t)pdu[2 + i * 2] << 8) | pdu[3 + i * 2];
  }
  return true;
}

static bool directRead(uint8_t uid, uint16_t start, uint16_t count,
                       std::vector<uint16_t> &values) {
  if (xSemaphoreTake(upstreamMutex, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
  WiFiClient client;
  bool ok = openUpstream(client) && readOnConnection(client, uid, start, count, values);
  client.stop();
  xSemaphoreGive(upstreamMutex);
  if (!ok) upstreamErrors++;
  return ok;
}

static bool forwardWrite(uint8_t uid, uint16_t reg, uint16_t value) {
  if (xSemaphoreTake(upstreamMutex, pdMS_TO_TICKS(15000)) != pdTRUE) return false;

  WiFiClient client;
  bool ok = false;
  if (openUpstream(client)) {
    uint8_t request[12] = {
      0, 0, 0, 0, 0, 6, uid, 6,
      (uint8_t)(reg >> 8), (uint8_t)reg,
      (uint8_t)(value >> 8), (uint8_t)value
    };

    if (client.write(request, sizeof(request)) == sizeof(request)) {
      uint16_t tx;
      uint8_t responseUid;
      std::vector<uint8_t> pdu;
      ok = readResponse(client, tx, responseUid, pdu) && pdu.size() >= 5 && pdu[0] == 6;
    }
  }

  client.stop();
  xSemaphoreGive(upstreamMutex);
  if (!ok) upstreamErrors++;
  return ok;
}

static std::vector<uint8_t> makeResponse(uint16_t tx, uint8_t uid,
                                         const std::vector<uint8_t> &pdu) {
  std::vector<uint8_t> out(7 + pdu.size());
  uint16_t len = pdu.size() + 1;
  out[0] = tx >> 8;
  out[1] = tx;
  out[2] = 0;
  out[3] = 0;
  out[4] = len >> 8;
  out[5] = len;
  out[6] = uid;
  memcpy(out.data() + 7, pdu.data(), pdu.size());
  return out;
}

static void recordDisconnect(const ClientSlot &slot) {
  ClientHistory &entry = history[historyHead];
  entry.ip = slot.ip.toString();
  entry.port = slot.port;
  entry.connectedAt = slot.connectedAt;
  entry.disconnectedAt = time(nullptr);
  entry.requestCount = slot.requestCount;

  historyHead = (historyHead + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) historyCount++;
}

static void closeClient(size_t index) {
  if (!clients[index].used) return;
  recordDisconnect(clients[index]);
  clients[index].client.stop();
  clients[index].used = false;
  clients[index].rx.clear();
}

static void acceptClients() {
  if (!modbusServer.hasClient()) return;
  WiFiClient incoming = modbusServer.available();
  if (!incoming) return;

  for (size_t i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].used) {
      clients[i].client = incoming;
      clients[i].used = true;
      clients[i].ip = incoming.remoteIP();
      clients[i].port = incoming.remotePort();
      clients[i].connectedAt = time(nullptr);
      clients[i].connectedMillis = millis();
      clients[i].lastRequestAt = 0;
      clients[i].requestCount = 0;
      clients[i].rx.clear();
      clients[i].rx.reserve(300);
      Serial.printf("Client connected: %s:%u\n",
                    clients[i].ip.toString().c_str(), clients[i].port);
      return;
    }
  }

  incoming.stop();
}

static void processFrame(ClientSlot &slot, const uint8_t *frame, size_t frameSize) {
  if (frameSize < 8) return;

  uint16_t tx = ((uint16_t)frame[0] << 8) | frame[1];
  uint16_t proto = ((uint16_t)frame[2] << 8) | frame[3];
  uint16_t len = ((uint16_t)frame[4] << 8) | frame[5];
  uint8_t uid = frame[6];

  if (proto != 0 || len < 2 || frameSize != (size_t)(6 + len)) return;

  const uint8_t *pdu = frame + 7;
  size_t pduSize = len - 1;
  uint8_t fc = pdu[0];
  std::vector<uint8_t> responsePdu;

  slot.requestCount++;
  slot.lastRequestAt = time(nullptr);

  if (fc == 3 && pduSize >= 5) {
    uint16_t reg = ((uint16_t)pdu[1] << 8) | pdu[2];
    uint16_t count = ((uint16_t)pdu[3] << 8) | pdu[4];
    std::vector<uint16_t> values;

    bool ok = cacheGetRange(reg, count, values);
    uint32_t endReg = (uint32_t)reg + count - 1;
    bool setupRange = count > 0 && reg >= SETUP_PASSTHROUGH_START && endReg <= SETUP_PASSTHROUGH_END;
    if (!ok && setupRange) ok = directRead(uid, reg, count, values);

    if (!ok) {
      responsePdu = {(uint8_t)0x83, 2};
    } else {
      responsePdu.resize(2 + count * 2);
      responsePdu[0] = 3;
      responsePdu[1] = count * 2;
      for (uint16_t i = 0; i < count; i++) {
        responsePdu[2 + i * 2] = values[i] >> 8;
        responsePdu[3 + i * 2] = values[i];
      }
    }
  } else if (fc == 6 && pduSize >= 5) {
    uint16_t reg = ((uint16_t)pdu[1] << 8) | pdu[2];
    uint16_t value = ((uint16_t)pdu[3] << 8) | pdu[4];
    if (forwardWrite(uid, reg, value)) {
      responsePdu.assign(pdu, pdu + 5);
    } else {
      responsePdu = {(uint8_t)0x86, 4};
    }
  } else {
    responsePdu = {(uint8_t)(fc | 0x80), 1};
  }

  auto out = makeResponse(tx, uid, responsePdu);
  slot.client.write(out.data(), out.size());
}

static void serviceClients() {
  for (size_t i = 0; i < MAX_CLIENTS; i++) {
    ClientSlot &slot = clients[i];
    if (!slot.used) continue;

    if (!slot.client.connected()) {
      closeClient(i);
      continue;
    }

    while (slot.client.available()) {
      int b = slot.client.read();
      if (b < 0) break;
      slot.rx.push_back((uint8_t)b);
      if (slot.rx.size() > 1024) {
        closeClient(i);
        break;
      }
    }

    while (slot.used && slot.rx.size() >= 7) {
      uint16_t len = ((uint16_t)slot.rx[4] << 8) | slot.rx[5];
      if (len < 2 || len > 260) {
        closeClient(i);
        break;
      }

      size_t total = 6 + len;
      if (slot.rx.size() < total) break;
      processFrame(slot, slot.rx.data(), total);
      slot.rx.erase(slot.rx.begin(), slot.rx.begin() + total);
    }
  }
}

static void pollTask(void *) {
  for (;;) {
    if (!ethConnected) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (xSemaphoreTake(upstreamMutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    WiFiClient client;
    uint32_t okBatches = 0;

    if (openUpstream(client)) {
      for (const auto &batch : REGISTER_BATCHES) {
        std::vector<uint16_t> values;
        if (readOnConnection(client, DEVICE_ID, batch.start, batch.count, values)) {
          Serial.printf("Batch OK: %u/%u\\n", batch.start, batch.count);
          cacheSetRange(batch.start, values);
          okBatches++;
        } else {
          upstreamErrors++;
          Serial.printf("Batch FAIL: %u/%u\\n", batch.start, batch.count);
        }
        vTaskDelay(pdMS_TO_TICKS(BATCH_DELAY_MS));
      }
    } else {
      upstreamErrors++;
    }

    client.stop();
    xSemaphoreGive(upstreamMutex);

    pollOkBatches = okBatches;
    if (okBatches == BATCH_COUNT) {
      cacheValid = true;
      lastSuccessfulPollMillis = millis();
    }

    Serial.printf("Poll: %lu/%u batches OK\n",
                  (unsigned long)okBatches, (unsigned)BATCH_COUNT);
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }
}

static String htmlPage() {
  String h;
  h.reserve(12000);
  h = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='5'><title>Huawei SunBridge ESP32</title><style>body{font-family:system-ui;background:#111827;color:#e5e7eb;margin:0;padding:20px}.wrap{max-width:1100px;margin:auto}.card{background:#1f2937;border-radius:12px;padding:18px;margin:14px 0}table{width:100%;border-collapse:collapse}td,th{padding:8px;border-bottom:1px solid #374151;text-align:left}.ok{color:#34d399}.bad{color:#f87171}.muted{color:#9ca3af}code,a{color:#93c5fd}</style></head><body><div class='wrap'>");

  h += "<h1>Huawei SunBridge ESP32</h1><div class='muted'>Firmware " + String(SUNBRIDGE_VERSION) + " · WT32-ETH01 v1.4</div>";
  h += "<div class='card'><h2>Ethernet</h2><table>";
  h += "<tr><td>Status</td><td class='" + String(ethConnected ? "ok'>UP" : "bad'>DOWN") + "</td></tr>";
  h += "<tr><td>IP</td><td><code>" + ETH.localIP().toString() + "</code></td></tr>";
  h += "<tr><td>Gateway</td><td>" + ETH.gatewayIP().toString() + "</td></tr>";
  h += "<tr><td>Link</td><td>" + String(ETH.linkSpeed()) + " Mbps " + String(ETH.fullDuplex() ? "Full Duplex" : "Half Duplex") + "</td></tr></table></div>";

  h += "<div class='card'><h2>SDongle / proxy</h2><table>";
  h += "<tr><td>SDongle</td><td><code>" + SDONGLE_IP.toString() + ":502</code></td></tr>";
  h += "<tr><td>SunBridge</td><td><code>" + LOCAL_IP.toString() + ":5502</code></td></tr>";
  h += "<tr><td>Cache valida</td><td>" + String(cacheValid ? "sì" : "no") + "</td></tr>";
  h += "<tr><td>Ultimo poll</td><td>" + String(pollOkBatches) + "/" + String(BATCH_COUNT) + " batch OK</td></tr>";
  h += "<tr><td>Età cache</td><td>" + (lastSuccessfulPollMillis ? String((millis() - lastSuccessfulPollMillis) / 1000) + " s" : String("mai aggiornata")) + "</td></tr>";
  h += "<tr><td>Errori upstream</td><td>" + String(upstreamErrors) + "</td></tr>";
  h += "<tr><td>Uptime</td><td>" + formatDuration(millis() / 1000) + "</td></tr></table></div>";

  h += F("<div class='card'><h2>Client Modbus connessi</h2><table><tr><th>IP</th><th>Porta</th><th>Connesso dal</th><th>Durata</th><th>Ultima richiesta</th><th>Richieste</th></tr>");
  size_t active = 0;
  for (size_t i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].used) continue;
    active++;
    h += "<tr><td><code>" + clients[i].ip.toString() + "</code></td><td>" + String(clients[i].port) + "</td><td>" + formatTime(clients[i].connectedAt) + "</td><td>" + formatDuration((millis() - clients[i].connectedMillis) / 1000) + "</td><td>" + (clients[i].lastRequestAt ? formatTime(clients[i].lastRequestAt) : String("—")) + "</td><td>" + String(clients[i].requestCount) + "</td></tr>";
  }
  if (!active) h += F("<tr><td colspan='6' class='muted'>Nessun client connesso</td></tr>");
  h += "</table><p><b>Client attivi:</b> " + String(active) + " / " + String(MAX_CLIENTS) + "</p></div>";

  h += F("<div class='card'><h2>Ultime disconnessioni</h2><table><tr><th>IP</th><th>Porta</th><th>Connessione</th><th>Disconnessione</th><th>Richieste</th></tr>");
  if (!historyCount) h += F("<tr><td colspan='5' class='muted'>Nessuna disconnessione registrata</td></tr>");
  for (size_t n = 0; n < historyCount; n++) {
    size_t i = (historyHead + MAX_HISTORY - 1 - n) % MAX_HISTORY;
    const auto &x = history[i];
    h += "<tr><td><code>" + x.ip + "</code></td><td>" + String(x.port) + "</td><td>" + formatTime(x.connectedAt) + "</td><td>" + formatTime(x.disconnectedAt) + "</td><td>" + String(x.requestCount) + "</td></tr>";
  }
  h += F("</table></div><div class='card'><h2>Aggiornamento firmware OTA</h2><p>Carica il normale <code>firmware.bin</code> via Ethernet, senza USB-TTL.</p><p><a href='/update'>Apri pagina aggiornamento OTA</a></p></div>");
  h += "<div class='muted'>Ora locale: " + formatTime(time(nullptr)) + " · aggiornamento ogni 5 s</div></div></body></html>";
  return h;
}

static void setupWeb() {
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html; charset=utf-8", htmlPage());
  });

  webServer.on("/health", HTTP_GET, []() {
    String json = String("{\"ethernet\":") + (ethConnected ? "true" : "false") +
                  ",\"cache_valid\":" + (cacheValid ? String("true") : String("false")) +
                  ",\"poll_ok\":" + String(pollOkBatches) +
                  ",\"poll_total\":" + String(BATCH_COUNT) +
                  ",\"upstream_errors\":" + String(upstreamErrors) + "}";
    webServer.send(200, "application/json", json);
  });

  httpUpdater.setup(&webServer, "/update");
  webServer.begin();
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname("huawei-sunbridge");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethConnected = true;
      Serial.printf("Ethernet UP: %s\n", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      ethConnected = false;
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  cacheMutex = xSemaphoreCreateMutex();
  upstreamMutex = xSemaphoreCreateMutex();
  initCache();

  WiFi.onEvent(WiFiEvent);

  // Arduino-ESP32 2.0.17 signature:
  // begin(phy_addr, power, mdc, mdio, type, clk_mode, use_mac_from_efuse)
  bool ethStarted = ETH.begin(
    SB_ETH_PHY_ADDR,
    SB_ETH_PHY_POWER,
    SB_ETH_MDC_PIN,
    SB_ETH_MDIO_PIN,
    SB_ETH_PHY_TYPE,
    SB_ETH_CLK_MODE
  );

  if (!ethStarted) {
    Serial.println("ETH.begin failed");
  }

  ETH.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);

  uint32_t started = millis();
  while (!ethConnected && millis() - started < 15000) {
    delay(100);
  }

  configTzTime(
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  modbusServer.begin();
  modbusServer.setNoDelay(true);
  setupWeb();

  xTaskCreatePinnedToCore(
    pollTask,
    "sdongle-poll",
    8192,
    nullptr,
    1,
    nullptr,
    0
  );

  Serial.printf(
    "SunBridge %s:%u -> SDongle %s:%u\n",
    LOCAL_IP.toString().c_str(),
    SUNBRIDGE_PORT,
    SDONGLE_IP.toString().c_str(),
    SDONGLE_PORT
  );
}

void loop() {
  acceptClients();
  serviceClients();
  webServer.handleClient();
  delay(1);
}
