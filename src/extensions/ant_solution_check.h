#pragma once

// Guardian solution check: the request a guardian signs to have this node score one ant solution.

#include <string>

// Identity allowed to ask for a score. Empty turns the compute endpoint off.
#ifndef ANT_CHECK_GUARDIAN_IDENTITY
#define ANT_CHECK_GUARDIAN_IDENTITY "MKPYRHUKEHUTBBTNUTPVQOUDNAODJZQTFRWAEUBNRGSGEUCRFKBNKUVGJLOL"
#endif

// Signed bytes: publicKey[32] | nonce[32] | anchorTick | parentTick | parentIndex, the numbers 32-bit little-endian.
static constexpr unsigned int ANT_SCORE_CHALLENGE_BYTES = 76;

static void antChallengeDigest(const m256i& publicKey, const m256i& nonce, unsigned int anchorTick, unsigned int parentTick,
    unsigned int parentIndex, unsigned char* digest)
{
    unsigned char message[ANT_SCORE_CHALLENGE_BYTES];
    copyMem(message, publicKey.m256i_u8, 32);
    copyMem(message + 32, nonce.m256i_u8, 32);
    copyMem(message + 64, &anchorTick, 4);
    copyMem(message + 68, &parentTick, 4);
    copyMem(message + 72, &parentIndex, 4);
    KangarooTwelve(message, sizeof(message), digest, 32);
}

static bool antChallengeSignatureValid(const m256i& guardianPublicKey, const m256i& publicKey, const m256i& nonce, unsigned int anchorTick,
    unsigned int parentTick, unsigned int parentIndex, const unsigned char* signature)
{
    unsigned char digest[32];
    antChallengeDigest(publicKey, nonce, anchorTick, parentTick, parentIndex, digest);
    return verify(guardianPublicKey.m256i_u8, digest, signature);
}

static int antHexDigitValue(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F')
    {
        return digit - 'A' + 10;
    }
    return -1;
}

// Exactly byteCount bytes written as hex digits, nothing else.
static bool antParseHex(const std::string& hex, unsigned char* out, unsigned int byteCount)
{
    if (hex.size() != 2ULL * byteCount)
    {
        return false;
    }
    for (unsigned int i = 0; i < byteCount; i++)
    {
        const int high = antHexDigitValue(hex[2 * i]);
        const int low = antHexDigitValue(hex[2 * i + 1]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out[i] = (unsigned char)((high << 4) | low);
    }
    return true;
}

// Decimal digits only, no larger than a 32-bit value.
static bool antParseUnsigned(const std::string& text, unsigned int& out)
{
    if (text.empty() || text.size() > 10)
    {
        return false;
    }
    unsigned long long value = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        value = value * 10 + (unsigned long long)(digit - '0');
    }
    if (value > 0xFFFFFFFFULL)
    {
        return false;
    }
    out = (unsigned int)value;
    return true;
}

// A 60-letter identity whose checksum matches.
static bool antParseIdentity(const std::string& identity, m256i& publicKey)
{
    if (identity.size() != 60 || !getPublicKeyFromIdentity((const unsigned char*)identity.c_str(), publicKey.m256i_u8))
    {
        return false;
    }
    CHAR16 canonical[61] = {};
    getIdentity(publicKey.m256i_u8, canonical, false);
    for (unsigned int i = 0; i < 60; i++)
    {
        if ((char)canonical[i] != identity[i])
        {
            return false;
        }
    }
    return true;
}
