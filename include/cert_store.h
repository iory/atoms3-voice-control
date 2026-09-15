#pragma once

#include <Arduino.h>

#include <ctime>

struct TlsCredentials {
  String certPem;
  String keyPem;
  time_t notAfter = 0;
  bool fromCache = false;
};

// Downloads the local-ip.sh wildcard certificate and key, validates them
// (parseable, key matches certificate, not expired at `now`) and caches them in
// LittleFS. If the download fails, a still-valid cached copy is used and
// `fromCache` is set so the caller can report it.
//
// Returns an empty string on success, otherwise a human-readable error.
String loadTlsCredentials(time_t now, TlsCredentials& out);
