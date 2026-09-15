#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace config {

// local-ip.sh resolves "192-168-1-50.local-ip.sh" to 192.168.1.50 and publishes
// a Let's Encrypt wildcard certificate (with its private key) for *.local-ip.sh.
constexpr const char* kLocalIpDomain = "local-ip.sh";
constexpr const char* kCertUrl = "https://local-ip.sh/server.pem";
constexpr const char* kKeyUrl = "https://local-ip.sh/server.key";

constexpr const char* kCertCachePath = "/tls/server.pem";
constexpr const char* kKeyCachePath = "/tls/server.key";

constexpr const char* kTimeZone = "JST-9";
constexpr const char* kNtpServer1 = "ntp.nict.jp";
constexpr const char* kNtpServer2 = "pool.ntp.org";
// Any clock earlier than this has not been set by NTP yet (2025-01-01T00:00Z).
constexpr time_t kMinValidEpoch = 1735689600;

constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kNtpTimeoutMs = 15000;

// local-ip.sh renews about a month before expiry, so a daily re-download keeps
// an always-on device valid. After a failure, retry sooner.
constexpr uint32_t kCertRefreshIntervalMs = 24UL * 60 * 60 * 1000;
constexpr uint32_t kCertRetryIntervalMs = 10UL * 60 * 1000;
// Less than this left means the daily refresh has been failing for weeks.
constexpr time_t kCertWarnSeconds = 14 * 24 * 60 * 60;
// Below this, show hours instead of days.
constexpr time_t kCertShowHoursBelowSeconds = 2 * 24 * 60 * 60;
constexpr uint32_t kStatusCheckIntervalMs = 1000;
// The plain-HTTP "certificate unavailable" page reloads itself at this period
// so it turns into a redirect once the certificate is back.
constexpr uint32_t kUnavailablePageReloadSeconds = 30;

// esp_http_server uses a UDP control socket per instance; the HTTPS server
// keeps the default (32768), so the HTTP->HTTPS redirector needs another one.
constexpr uint16_t kRedirectCtrlPort = 32769;
constexpr uint16_t kHttpsStackSize = 10240;
constexpr uint16_t kRedirectStackSize = 4096;

// The 128x128 LCD fits roughly 10 x 9 full-width glyphs of efontJA_12.
constexpr size_t kMaxDisplayCodepoints = 80;

}  // namespace config
