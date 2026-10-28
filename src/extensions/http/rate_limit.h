#pragma once

// Shared per-IP HTTP rate limiter (token bucket) + client-IP extraction. Used by both the in-process
// HTTP server (http.h) and the RPC sidecar proxy (rpc_proxy.h) so both throttle identically.

#if defined(__linux__) || defined(LITE_WASM_SC)

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace rl
{
inline bool envBool(const char* name, bool defaultVal)
{
    const char* s = std::getenv(name);
    if (!s || !*s) return defaultVal;
    std::string v(s);
    return !(v == "0" || v == "false" || v == "FALSE" || v == "no");
}

// Loopback / RFC1918 (incl. Docker bridge) pass unmetered and may set proxy headers.
inline bool isAllowlistedIp(const std::string& ip)
{
    if (ip.empty()) return true;
    if (ip == "127.0.0.1" || ip == "::1") return true;
    if (ip.rfind("10.", 0) == 0) return true;
    if (ip.rfind("192.168.", 0) == 0) return true;
    if (ip.rfind("172.", 0) == 0)
    {
        size_t dot = ip.find('.', 4);
        if (dot != std::string::npos)
        {
            int n = std::atoi(ip.substr(4, dot - 4).c_str());
            if (n >= 16 && n <= 31) return true;
        }
    }
    return false;
}

// Originating client IP for diagnostics/metering, never auth. Proxy headers (CF-Connecting-IP,
// X-Real-IP, first X-Forwarded-For) are forgeable, so only a local reverse proxy (allowlisted peer)
// may set them; QUBIC_HTTP_TRUST_PROXY_HEADERS=1 honors them from any peer for nodes behind a
// public-IP proxy or CDN whose ingress is firewalled to that proxy.
inline std::string extractClientIp(const drogon::HttpRequestPtr& req)
{
    static const bool trustAnyPeer = envBool("QUBIC_HTTP_TRUST_PROXY_HEADERS", false);
    std::string peerIp = req->peerAddr().toIp();
    if (!trustAnyPeer && !isAllowlistedIp(peerIp)) return peerIp;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    auto cf = req->getHeader("cf-connecting-ip");
    if (!cf.empty()) return trim(cf);
    auto xri = req->getHeader("x-real-ip");
    if (!xri.empty()) return trim(xri);
    auto xff = req->getHeader("x-forwarded-for");
    if (!xff.empty())
    {
        auto comma = xff.find(',');
        return trim(comma == std::string::npos ? xff : xff.substr(0, comma));
    }
    return peerIp;
}

#ifndef NO_HTTP_RATE_LIMIT
struct RateLimitBucket
{
    double tokens = 0.0;
    std::chrono::steady_clock::time_point lastRefill{};
    uint64_t blockedCount = 0;
    std::chrono::steady_clock::time_point lastSeen{};
};

inline std::mutex& rateLimitMutex()
{
    static std::mutex mutex; // SMARTMUTEX-EXEMPT: non-consensus sidecar HTTP rate-limit map
    return mutex;
}
inline std::unordered_map<std::string, RateLimitBucket>& rateLimitBuckets()
{
    static std::unordered_map<std::string, RateLimitBucket> b;
    return b;
}

inline double envDouble(const char* name, double defaultVal)
{
    const char* s = std::getenv(name);
    if (!s || !*s) return defaultVal;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    return (end == s) ? defaultVal : v;
}
inline bool rateLimitEnabled()
{
    static const bool v = envBool("QUBIC_HTTP_RATE_LIMIT_ENABLED", true);
    return v;
}
inline double rateLimitRefillPerSec()
{
    static const double v = std::max(1.0, envDouble("QUBIC_HTTP_RATE_LIMIT_RPS", 20.0));
    return v;
}
inline double rateLimitBurst()
{
    static const double v = std::max(1.0, envDouble("QUBIC_HTTP_RATE_LIMIT_BURST", 40.0));
    return v;
}

// True if allowed, false if it must be 429'd.
inline bool checkRateLimit(const std::string& ip)
{
    if (!rateLimitEnabled()) return true;
    if (isAllowlistedIp(ip)) return true;

    auto now = std::chrono::steady_clock::now();
    const double burst = rateLimitBurst();
    const double rps = rateLimitRefillPerSec();

    std::lock_guard<std::mutex> lk(rateLimitMutex());
    auto& b = rateLimitBuckets()[ip];
    if (b.lastRefill.time_since_epoch().count() == 0)
    {
        b.tokens = burst;
    }
    else
    {
        double secs = std::chrono::duration<double>(now - b.lastRefill).count();
        b.tokens = std::min(burst, b.tokens + secs * rps);
    }
    b.lastRefill = now;
    b.lastSeen = now;
    if (b.tokens >= 1.0)
    {
        b.tokens -= 1.0;
        return true;
    }
    b.blockedCount++;
    return false;
}

// Log the top blocked IPs and evict buckets idle > 5 min (keeps memory bounded). Call periodically.
inline void logRateLimitReport(int intervalSec, size_t topN)
{
    if (!rateLimitEnabled()) return;
    auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, uint64_t>> blocked;
    size_t bucketCount = 0;
    {
        std::lock_guard<std::mutex> lk(rateLimitMutex());
        auto& m = rateLimitBuckets();
        for (auto it = m.begin(); it != m.end();)
        {
            if (it->second.blockedCount > 0)
                blocked.emplace_back(it->first, it->second.blockedCount);
            it->second.blockedCount = 0;
            double idleSec = std::chrono::duration<double>(now - it->second.lastSeen).count();
            if (idleSec > 300.0) it = m.erase(it);
            else ++it;
        }
        bucketCount = m.size();
    }
    if (blocked.empty()) return;
    std::sort(blocked.begin(), blocked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    size_t bn = std::min(blocked.size(), topN);
    std::ostringstream oss;
    oss << "HTTP rate-limit drops (last " << intervalSec << "s, top " << bn << " of " << blocked.size() << " IPs, " << bucketCount << " buckets tracked):";
    for (size_t i = 0; i < bn; ++i)
        oss << " " << blocked[i].first << "=" << blocked[i].second;
    LOG_INFO << oss.str();
}
#endif // NO_HTTP_RATE_LIMIT

} // namespace rl

#endif // __linux__
