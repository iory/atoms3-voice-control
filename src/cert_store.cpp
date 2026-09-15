#include "cert_store.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include "config.h"

#if MBEDTLS_VERSION_MAJOR >= 3
#error "cert_store.cpp uses the mbedTLS 2.x API (arduino-esp32 2.x). Port mbedtls_pk_parse_key / check_pair before moving to 3.x."
#endif

// ISRG Root YR + ISRG Root X1, embedded via board_build.embed_txtfiles.
extern const char letsencrypt_roots_pem[] asm("_binary_certs_letsencrypt_roots_pem_start");

namespace {

bool httpsGet(const char* url, String& body, String& error) {
  WiFiClientSecure client;
  client.setCACert(letsencrypt_roots_pem);
  HTTPClient http;
  if (!http.begin(client, url)) {
    error = String("cannot start request: ") + url;
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = String("GET ") + url + " -> " + (code < 0 ? HTTPClient::errorToString(code) : String(code));
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return true;
}

// Howard Hinnant's days_from_civil; avoids depending on timegm() in newlib.
time_t toEpochUtc(const mbedtls_x509_time& t) {
  const int y = t.year - (t.mon <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (t.mon + (t.mon > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return static_cast<time_t>(days * 86400 + t.hour * 3600 + t.min * 60 + t.sec);
}

String mbedtlsError(const char* what, int ret) {
  return String(what) + " -0x" + String(-ret, HEX);
}

// Checks that both PEMs parse and belong together; returns the leaf expiry.
bool inspect(const String& certPem, const String& keyPem, time_t& notAfter, String& error) {
  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);

  bool ok = false;
  int ret = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(certPem.c_str()), certPem.length() + 1);
  if (ret != 0) {
    error = mbedtlsError("certificate parse failed", ret);
  } else {
    ret = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(keyPem.c_str()), keyPem.length() + 1, nullptr, 0);
    if (ret != 0) {
      error = mbedtlsError("private key parse failed", ret);
    } else {
      ret = mbedtls_pk_check_pair(&crt.pk, &key);
      if (ret != 0) {
        error = mbedtlsError("key does not match certificate", ret);
      } else {
        notAfter = toEpochUtc(crt.valid_to);
        ok = true;
      }
    }
  }

  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

bool readFile(const char* path, String& out) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    return false;
  }
  out = f.readString();
  f.close();
  return !out.isEmpty();
}

bool writeFile(const char* path, const String& content) {
  File f = LittleFS.open(path, "w", true);
  if (!f) {
    return false;
  }
  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool validate(const String& cert, const String& key, time_t now, time_t& notAfter, String& error) {
  if (!inspect(cert, key, notAfter, error)) {
    return false;
  }
  if (notAfter <= now) {
    error = "certificate expired";
    return false;
  }
  return true;
}

}  // namespace

bool downloadTlsCredentials(time_t now, TlsCredentials& out, String& error) {
  String cert;
  String key;
  if (!httpsGet(config::kCertUrl, cert, error) || !httpsGet(config::kKeyUrl, key, error)) {
    return false;
  }
  time_t notAfter = 0;
  if (!validate(cert, key, now, notAfter, error)) {
    error = "downloaded " + error;
    return false;
  }
  out.certPem = std::move(cert);
  out.keyPem = std::move(key);
  out.notAfter = notAfter;
  out.fromCache = false;
  return true;
}

bool loadCachedTlsCredentials(time_t now, TlsCredentials& out, String& error) {
  String cert;
  String key;
  if (!readFile(config::kCertCachePath, cert) || !readFile(config::kKeyCachePath, key)) {
    error = "no cached certificate";
    return false;
  }
  time_t notAfter = 0;
  if (!validate(cert, key, now, notAfter, error)) {
    error = "cached " + error;
    return false;
  }
  out.certPem = std::move(cert);
  out.keyPem = std::move(key);
  out.notAfter = notAfter;
  out.fromCache = true;
  return true;
}

bool saveTlsCache(const TlsCredentials& credentials) {
  return writeFile(config::kCertCachePath, credentials.certPem) && writeFile(config::kKeyCachePath, credentials.keyPem);
}
