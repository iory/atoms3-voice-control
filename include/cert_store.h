#pragma once

#include <Arduino.h>

#include <ctime>

struct TlsCredentials {
  String certPem;
  String keyPem;
  time_t notAfter = 0;
  bool fromCache = false;
};

// Each loader validates what it returns: both PEMs parse, the key matches the
// certificate, and the certificate has not expired at `now`. On failure `error`
// holds a human-readable reason and `out` is left untouched.

// Fetches the local-ip.sh wildcard certificate and key.
bool downloadTlsCredentials(time_t now, TlsCredentials& out, String& error);

// Reads the copy last stored by saveTlsCache(). Sets `fromCache`.
bool loadCachedTlsCredentials(time_t now, TlsCredentials& out, String& error);

bool saveTlsCache(const TlsCredentials& credentials);
