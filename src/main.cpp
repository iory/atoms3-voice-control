// iPhone Safari (Web Speech API) -> wss:// -> AtomS3.
//
// The AtomS3 serves the page itself over HTTPS as <dashed-ip>.local-ip.sh, so
// Safari treats it as a secure context and the WebSocket needs no mixed-content
// exception.
//
// The QR code points at plain http://<ip>/. While the certificate is valid that
// redirects to the HTTPS page; otherwise it explains why the page is down, since
// Safari cannot show anything served with an expired certificate.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <PsychicHttp.h>
#include <PsychicHttpsServer.h>
#include <WiFi.h>

#include <atomic>

#include "cert_store.h"
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing: copy include/secrets.example.h and set WIFI_SSID / WIFI_PASSWORD"
#endif

extern const char index_html[] asm("_binary_src_web_index_html_start");
extern const char unavailable_html[] asm("_binary_src_web_unavailable_html_start");

namespace {

enum class CertState { Missing, Expired, Warning, Ok };

struct CertStatus {
  bool hasCert = false;
  time_t notAfter = 0;
  bool fromCache = false;
  time_t lastAttempt = 0;
  time_t lastSuccess = 0;
  String lastError;
};

struct Speech {
  String text;
  bool isFinal = false;
  uint32_t count = 0;
};

enum class View { Qr, Speech };

struct ScopedLock {
  explicit ScopedLock(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~ScopedLock() { xSemaphoreGive(mutex); }
  SemaphoreHandle_t mutex;
};

PsychicHttpsServer httpsServer;
PsychicHttpServer httpServer;
PsychicWebSocketHandler speechSocket;

// httpsServer keeps pointers into these PEM buffers: replace only while stopped.
TlsCredentials tls;
std::atomic<bool> httpsRunning{false};
String httpUrl;
String httpsUrl;

// Guards certStatus, latestSpeech and speechDirty (shared with httpd tasks).
SemaphoreHandle_t stateMutex = nullptr;
CertStatus certStatus;
Speech latestSpeech;
bool speechDirty = false;

View view = View::Qr;
String drawnQrFooter;
uint32_t lastRefreshAttemptMs = 0;
uint32_t refreshIntervalMs = config::kCertRefreshIntervalMs;
uint32_t lastStatusCheckMs = 0;

CertStatus certSnapshot() {
  ScopedLock lock(stateMutex);
  return certStatus;
}

CertState certStateOf(const CertStatus& s, time_t now) {
  if (!s.hasCert) {
    return CertState::Missing;
  }
  if (now >= s.notAfter) {
    return CertState::Expired;
  }
  if (s.notAfter - now < config::kCertWarnSeconds) {
    return CertState::Warning;
  }
  return CertState::Ok;
}

String formatLocalTime(time_t t) {
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", localtime(&t));
  return buf;
}

String formatRemaining(time_t seconds) {
  if (seconds < config::kCertShowHoursBelowSeconds) {
    return String(static_cast<long>(seconds / 3600)) + "h";
  }
  return String(static_cast<long>(seconds / 86400)) + "d";
}

String escapeHtml(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += s[i];
    }
  }
  return out;
}

String tailCodepoints(const String& s, size_t maxCodepoints) {
  size_t total = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      ++total;
    }
  }
  if (total <= maxCodepoints) {
    return s;
  }
  size_t skip = total - maxCodepoints;
  for (size_t i = 0; i < s.length(); ++i) {
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      if (skip == 0) {
        return s.substring(i);
      }
      --skip;
    }
  }
  return "";
}

// ---- LCD ----

void showStatus(const char* title, const String& detail, uint16_t color) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextWrap(true, true);
  d.setTextColor(color, TFT_BLACK);
  d.setCursor(0, 0);
  d.println(title);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.println(detail);
}

[[noreturn]] void haltWithError(const String& message) {
  showStatus("ERROR", message, TFT_RED);
  while (true) {
    Serial.printf("[error] %s\n", message.c_str());
    delay(5000);
  }
}

// e.g. "192.168.1.50 43d", "... EXP", "... NO", with " C" when running on the cache.
String qrFooter(const CertStatus& s, time_t now) {
  String footer = WiFi.localIP().toString() + " ";
  switch (certStateOf(s, now)) {
    case CertState::Missing: footer += "NO"; break;
    case CertState::Expired: footer += "EXP"; break;
    default: footer += formatRemaining(s.notAfter - now);
  }
  if (s.hasCert && s.fromCache) {
    footer += " C";
  }
  return footer;
}

uint16_t certColor(CertState state) {
  switch (state) {
    case CertState::Ok: return TFT_DARKGREEN;
    case CertState::Warning: return TFT_ORANGE;
    default: return TFT_RED;
  }
}

void drawQr() {
  const time_t now = time(nullptr);
  const CertStatus s = certSnapshot();
  drawnQrFooter = qrFooter(s, now);

  auto& d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.qrcode(httpUrl, 10, 0, 108, 2);
  d.setTextWrap(false);
  d.setCursor(2, 112);
  d.setTextColor(certColor(certStateOf(s, now)), TFT_WHITE);
  d.print(drawnQrFooter);
}

void drawSpeech(const Speech& s) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextWrap(true, true);
  d.setCursor(0, 0);
  d.setTextColor(s.isFinal ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  d.printf("#%u %s\n", s.count, s.isFinal ? "final" : "...");
  d.setTextColor(s.isFinal ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
  d.print(tailCodepoints(s.text, config::kMaxDisplayCodepoints));
}

// ---- Speech ----

// Hook for robot control: called for every interim and final transcript.
void handleSpeech(const char* text, bool isFinal) {
  JsonDocument line;
  line["text"] = text;
  line["final"] = isFinal;
  serializeJson(line, Serial);
  Serial.println();

  ScopedLock lock(stateMutex);
  latestSpeech.text = text;
  latestSpeech.isFinal = isFinal;
  ++latestSpeech.count;
  speechDirty = true;
}

// ---- Certificate status for clients ----

String certStatusJson() {
  const time_t now = time(nullptr);
  const CertStatus s = certSnapshot();
  JsonDocument doc;
  doc["type"] = "cert";
  doc["secondsLeft"] = static_cast<long>(s.notAfter - now);
  doc["notAfter"] = formatLocalTime(s.notAfter);
  doc["fromCache"] = s.fromCache;
  doc["warnSeconds"] = static_cast<long>(config::kCertWarnSeconds);
  if (s.lastSuccess != 0) {
    doc["lastSuccess"] = formatLocalTime(s.lastSuccess);
  }
  if (!s.lastError.isEmpty()) {
    doc["lastError"] = s.lastError;
    doc["lastAttempt"] = formatLocalTime(s.lastAttempt);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String unavailablePage() {
  const time_t now = time(nullptr);
  const CertStatus s = certSnapshot();
  const CertState state = certStateOf(s, now);

  String details;
  const auto row = [&details](const char* label, const String& value) {
    details += "<dt>";
    details += label;
    details += "</dt><dd>";
    details += escapeHtml(value);
    details += "</dd>";
  };
  row("状態", state == CertState::Expired ? "期限切れ" : "証明書なし");
  if (s.hasCert) {
    row("期限", formatLocalTime(s.notAfter));
  }
  row("最後の取得成功", s.lastSuccess != 0 ? formatLocalTime(s.lastSuccess) : String("なし (起動後)"));
  if (!s.lastError.isEmpty()) {
    row("最後のエラー", s.lastError);
    row("最後の試行", formatLocalTime(s.lastAttempt));
  }
  row("現在時刻", formatLocalTime(now));

  String page = unavailable_html;
  page.replace("%TITLE%", state == CertState::Expired ? "証明書の期限が切れています" : "有効な証明書がありません");
  page.replace("%DETAILS%", details);
  page.replace("%RETRY_MINUTES%", String(config::kCertRetryIntervalMs / 60000));
  page.replace("%RELOAD_SECONDS%", String(config::kUnavailablePageReloadSeconds));
  return page;
}

// ---- Servers ----

void setupRoutes() {
  speechSocket.onOpen([](PsychicWebSocketClient* client) {
    Serial.printf("[ws] #%u open from %s, free heap %u\n", client->socket(),
                  client->remoteIP().toString().c_str(), ESP.getFreeHeap());
    client->sendMessage(certStatusJson().c_str());
  });
  speechSocket.onClose([](PsychicWebSocketClient* client) {
    Serial.printf("[ws] #%u closed\n", client->socket());
  });
  speechSocket.onFrame([](PsychicWebSocketRequest* request, httpd_ws_frame* frame) {
    if (frame->type != HTTPD_WS_TYPE_TEXT) {
      return ESP_OK;
    }
    JsonDocument msg;
    const DeserializationError err = deserializeJson(msg, static_cast<const char*>(static_cast<void*>(frame->payload)), frame->len);
    if (err) {
      return request->reply(R"({"type":"error","message":"invalid json"})");
    }
    handleSpeech(msg["text"] | "", msg["final"] | false);

    // Echo seq/t so the page can show the round-trip time.
    JsonDocument ack;
    ack["type"] = "ack";
    ack["seq"] = msg["seq"];
    ack["t"] = msg["t"];
    String out;
    serializeJson(ack, out);
    return request->reply(out.c_str());
  });

  httpsServer.config.stack_size = config::kHttpsStackSize;
  // Only two TLS sessions fit in RAM; let the WebSocket evict an idle page connection.
  httpsServer.ssl_config.httpd.lru_purge_enable = true;
  httpsServer.on("/", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html; charset=utf-8", index_html);
  });
  httpsServer.on("/ws", &speechSocket);

  httpServer.config.ctrl_port = config::kRedirectCtrlPort;
  httpServer.config.stack_size = config::kRedirectStackSize;
  httpServer.onNotFound([](PsychicRequest* request, PsychicResponse* response) {
    if (httpsRunning) {
      return response->redirect(httpsUrl.c_str());
    }
    return response->send(503, "text/html; charset=utf-8", unavailablePage().c_str());
  });
}

void startHttps() {
  httpsServer.setCertificate(tls.certPem.c_str(), tls.keyPem.c_str());
  if (httpsServer.begin() != ESP_OK) {
    haltWithError("HTTPS server failed to start");
  }
  httpsRunning = true;
  Serial.printf("[https] started, certificate valid until %s, free heap %u\n",
                formatLocalTime(tls.notAfter).c_str(), ESP.getFreeHeap());
}

void stopHttps() {
  if (!httpsRunning) {
    return;
  }
  httpsRunning = false;
  httpsServer.stop();
  Serial.println("[https] stopped");
}

// ---- Certificate lifecycle ----

void loadInitialCertificate(time_t now) {
  String downloadError;
  String cacheError;
  bool loaded = downloadTlsCredentials(now, tls, downloadError);
  if (loaded) {
    if (!saveTlsCache(tls)) {
      Serial.println("[tls] warning: could not write certificate cache");
    }
  } else {
    Serial.printf("[tls] download failed: %s\n", downloadError.c_str());
    loaded = loadCachedTlsCredentials(now, tls, cacheError);
  }

  {
    ScopedLock lock(stateMutex);
    certStatus.lastAttempt = now;
    certStatus.hasCert = loaded;
    certStatus.notAfter = tls.notAfter;
    certStatus.fromCache = tls.fromCache;
    if (loaded && !tls.fromCache) {
      certStatus.lastSuccess = now;
    } else {
      certStatus.lastError = loaded ? downloadError : downloadError + " / " + cacheError;
    }
  }
  refreshIntervalMs = (loaded && !tls.fromCache) ? config::kCertRefreshIntervalMs : config::kCertRetryIntervalMs;
  lastRefreshAttemptMs = millis();

  if (loaded) {
    startHttps();
  } else {
    Serial.printf("[tls] no usable certificate (%s); serving the explanation page only\n", cacheError.c_str());
  }
}

void refreshCertificate() {
  const time_t now = time(nullptr);
  TlsCredentials fresh;
  String error;
  const bool ok = downloadTlsCredentials(now, fresh, error);
  lastRefreshAttemptMs = millis();
  refreshIntervalMs = ok ? config::kCertRefreshIntervalMs : config::kCertRetryIntervalMs;

  if (ok && (!httpsRunning || fresh.certPem != tls.certPem)) {
    Serial.printf("[tls] new certificate valid until %s, restarting HTTPS\n", formatLocalTime(fresh.notAfter).c_str());
    stopHttps();
    tls = std::move(fresh);
    if (!saveTlsCache(tls)) {
      Serial.println("[tls] warning: could not write certificate cache");
    }
    startHttps();
  } else if (ok) {
    Serial.println("[tls] certificate unchanged");
  } else {
    Serial.printf("[tls] refresh failed: %s\n", error.c_str());
  }

  {
    ScopedLock lock(stateMutex);
    certStatus.lastAttempt = now;
    if (ok) {
      certStatus.hasCert = true;
      certStatus.notAfter = tls.notAfter;
      certStatus.fromCache = false;
      certStatus.lastSuccess = now;
      certStatus.lastError = "";
    } else {
      certStatus.lastError = error;
    }
  }
  if (httpsRunning) {
    speechSocket.sendAll(certStatusJson().c_str());
  }
}

// ---- Boot ----

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  // Modem sleep adds 100ms+ of jitter to every packet; not worth it for teleop.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > config::kWifiTimeoutMs) {
      return false;
    }
    delay(200);
  }
  return true;
}

bool syncTime(time_t& now) {
  configTzTime(config::kTimeZone, config::kNtpServer1, config::kNtpServer2);
  const uint32_t start = millis();
  while ((now = time(nullptr)) < config::kMinValidEpoch) {
    if (millis() - start > config::kNtpTimeoutMs) {
      return false;
    }
    delay(200);
  }
  return true;
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setFont(&fonts::efontJA_12);
  stateMutex = xSemaphoreCreateMutex();

  if (!LittleFS.begin(true)) {
    haltWithError("LittleFS mount failed");
  }

  showStatus("WiFi", WIFI_SSID, TFT_CYAN);
  if (!connectWifi()) {
    haltWithError(String("WiFi connect failed: ") + WIFI_SSID);
  }

  showStatus("NTP", config::kNtpServer1, TFT_CYAN);
  time_t now = 0;
  if (!syncTime(now)) {
    haltWithError("NTP sync failed (needed to check certificate expiry)");
  }

  const IPAddress ip = WiFi.localIP();
  String dashed = ip.toString();
  dashed.replace('.', '-');
  httpUrl = "http://" + ip.toString() + "/";
  httpsUrl = "https://" + dashed + "." + config::kLocalIpDomain + "/";

  setupRoutes();
  if (httpServer.begin() != ESP_OK) {
    haltWithError("HTTP server failed to start");
  }

  showStatus("TLS cert", config::kCertUrl, TFT_CYAN);
  loadInitialCertificate(now);

  Serial.printf("[ready] scan QR or open %s -> %s\n", httpUrl.c_str(), httpsUrl.c_str());
  Serial.printf("[ready] free heap %u\n", ESP.getFreeHeap());
  drawQr();
}

void loop() {
  M5.update();

  if (millis() - lastStatusCheckMs >= config::kStatusCheckIntervalMs) {
    lastStatusCheckMs = millis();
    const time_t now = time(nullptr);
    if (httpsRunning && now >= tls.notAfter) {
      Serial.println("[tls] certificate expired");
      stopHttps();
      refreshIntervalMs = 0;  // try a download right away
    }
    if (view == View::Qr && qrFooter(certSnapshot(), now) != drawnQrFooter) {
      drawQr();
    }
  }

  if (millis() - lastRefreshAttemptMs >= refreshIntervalMs) {
    refreshCertificate();
  }

  if (M5.BtnA.wasPressed()) {
    view = (view == View::Qr) ? View::Speech : View::Qr;
    if (view == View::Qr) {
      drawQr();
    } else {
      Speech snapshot;
      {
        ScopedLock lock(stateMutex);
        snapshot = latestSpeech;
      }
      drawSpeech(snapshot);
    }
  }

  Speech snapshot;
  bool dirty = false;
  {
    ScopedLock lock(stateMutex);
    dirty = speechDirty;
    if (dirty) {
      snapshot = latestSpeech;
      speechDirty = false;
    }
  }
  if (dirty) {
    view = View::Speech;
    drawSpeech(snapshot);
  }

  delay(10);
}
