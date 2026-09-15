// iPhone Safari (Web Speech API) -> wss:// -> AtomS3.
//
// The AtomS3 serves the page itself over HTTPS as <dashed-ip>.local-ip.sh, so
// Safari treats it as a secure context and the WebSocket needs no mixed-content
// exception. Scan the QR code on the LCD to open the page.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <PsychicHttp.h>
#include <PsychicHttpsServer.h>
#include <WiFi.h>

#include "cert_store.h"
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing: copy include/secrets.example.h and set WIFI_SSID / WIFI_PASSWORD"
#endif

extern const char index_html[] asm("_binary_src_web_index_html_start");

namespace {

PsychicHttpsServer httpsServer;
PsychicHttpServer redirectServer;
PsychicWebSocketHandler speechSocket;
TlsCredentials tls;
String pageUrl;

struct Speech {
  String text;
  bool isFinal = false;
  uint32_t count = 0;
};

// Written from the httpd task, drawn from loop().
SemaphoreHandle_t speechMutex = nullptr;
Speech latestSpeech;
bool speechDirty = false;

enum class View { Qr, Speech };
View view = View::Qr;

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

void drawQr() {
  auto& d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.qrcode(pageUrl, 10, 0, 108, 3);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextWrap(false);
  d.setCursor(4, 112);
  d.print(WiFi.localIP().toString());
  if (tls.fromCache) {
    d.setTextColor(TFT_RED, TFT_WHITE);
    d.print(" C");
  }
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

// Hook for robot control: called for every interim and final transcript.
void handleSpeech(const char* text, bool isFinal) {
  JsonDocument line;
  line["text"] = text;
  line["final"] = isFinal;
  serializeJson(line, Serial);
  Serial.println();

  xSemaphoreTake(speechMutex, portMAX_DELAY);
  latestSpeech.text = text;
  latestSpeech.isFinal = isFinal;
  ++latestSpeech.count;
  speechDirty = true;
  xSemaphoreGive(speechMutex);
}

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

String buildPageUrl(const IPAddress& ip) {
  String host = ip.toString();
  host.replace('.', '-');
  return "https://" + host + "." + config::kLocalIpDomain + "/";
}

void startServers() {
  speechSocket.onOpen([](PsychicWebSocketClient* client) {
    Serial.printf("[ws] #%u open from %s, free heap %u\n", client->socket(),
                  client->remoteIP().toString().c_str(), ESP.getFreeHeap());
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
  httpsServer.setCertificate(tls.certPem.c_str(), tls.keyPem.c_str());
  httpsServer.on("/", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html; charset=utf-8", index_html);
  });
  httpsServer.on("/ws", &speechSocket);
  if (httpsServer.begin() != ESP_OK) {
    haltWithError("HTTPS server failed to start");
  }

  // Typing the bare IP into Safari lands on the certificate-matching hostname.
  redirectServer.config.ctrl_port = config::kRedirectCtrlPort;
  redirectServer.config.stack_size = config::kRedirectStackSize;
  redirectServer.onNotFound([](PsychicRequest* request, PsychicResponse* response) {
    return response->redirect(pageUrl.c_str());
  });
  if (redirectServer.begin() != ESP_OK) {
    haltWithError("HTTP redirect server failed to start");
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setFont(&fonts::efontJA_12);
  speechMutex = xSemaphoreCreateMutex();

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

  showStatus("TLS cert", config::kCertUrl, TFT_CYAN);
  const String tlsError = loadTlsCredentials(now, tls);
  if (!tlsError.isEmpty()) {
    haltWithError(tlsError);
  }

  pageUrl = buildPageUrl(WiFi.localIP());
  startServers();

  char expiry[32];
  strftime(expiry, sizeof(expiry), "%Y-%m-%d %H:%M", localtime(&tls.notAfter));
  Serial.printf("[ready] %s\n", pageUrl.c_str());
  Serial.printf("[ready] certificate valid until %s%s\n", expiry, tls.fromCache ? " (from cache: download failed)" : "");
  Serial.printf("[ready] free heap %u\n", ESP.getFreeHeap());
  drawQr();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    view = (view == View::Qr) ? View::Speech : View::Qr;
    if (view == View::Qr) {
      drawQr();
    } else {
      xSemaphoreTake(speechMutex, portMAX_DELAY);
      const Speech snapshot = latestSpeech;
      xSemaphoreGive(speechMutex);
      drawSpeech(snapshot);
    }
  }

  xSemaphoreTake(speechMutex, portMAX_DELAY);
  const bool dirty = speechDirty;
  const Speech snapshot = dirty ? latestSpeech : Speech{};
  speechDirty = false;
  xSemaphoreGive(speechMutex);
  if (dirty) {
    view = View::Speech;
    drawSpeech(snapshot);
  }

  delay(10);
}
