#define NO_UEFI

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <string>

#include "platform/host_file.h"

// NO_UEFI stubs for platform/memory.h.
void setMem(void* buffer, unsigned long long size, unsigned char value)
{
    memset(buffer, (int)value, (size_t)size);
}

void copyMem(void* destination, const void* source, unsigned long long length)
{
    memcpy(destination, source, (size_t)length);
}

bool allocatePool(unsigned long long size, void** buffer)
{
    *buffer = malloc((size_t)size);
    return *buffer != nullptr;
}

void freePool(void* buffer)
{
    free(buffer);
}

#include "platform/m256.h"
#include "platform/memory.h"
#include "platform/common_types.h"
#include "four_q.h"
#include "network_messages/entity.h"
#include "network_messages/common_def.h"
#include "public_settings.h"

// Wrapper: real getIdentity writes CHAR16 (wchar_t), convert to narrow char.
static void getIdentityStr(const unsigned char* publicKey, char* out, bool isLowerCase)
{
    CHAR16 wid[61];
    getIdentity(publicKey, wid, isLowerCase);
    for (int i = 0; i < 60; i++) out[i] = (char)wid[i];
    out[60] = '\0';
}

// ---------------------------------------------------------------------------
// Hex utilities
// ---------------------------------------------------------------------------

static int hexCharVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hexToBytes(const char* hex, unsigned char* out, int outLen)
{
    int hexLen = (int)strlen(hex);
    if (hexLen != outLen * 2) return false;
    for (int i = 0; i < outLen; i++) {
        int hi = hexCharVal(hex[i * 2]);
        int lo = hexCharVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static void bytesToHex(const unsigned char* bytes, int len, char* out)
{
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", bytes[i]);
    out[len * 2] = '\0';
}

// ---------------------------------------------------------------------------
// Score cache entry (mirrors ScoreCache::CacheEntry layout — 104 bytes with
// compiler padding on the trailing int).
// ---------------------------------------------------------------------------

struct ScoreCacheEntry {
    m256i publicKey;
    m256i miningSeed;
    m256i nonce;
    int score;
};
static_assert(sizeof(ScoreCacheEntry) == 32 * 3 + 8, "ScoreCacheEntry size mismatch");

// ---------------------------------------------------------------------------
// read-entity
// ---------------------------------------------------------------------------

static int cmdReadEntity(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: qubic-cli --read-entity <spectrum-file> <identity-or-index>\n");
        return 1;
    }
    const char* filePath = argv[2];
    const char* idStr = argv[3];

    bool isNumeric = true;
    for (size_t i = 0; idStr[i]; i++) {
        if (idStr[i] < '0' || idStr[i] > '9') { isNumeric = false; break; }
    }

    m256i targetKey;
    unsigned long long targetIndex = 0;
    bool useIndex = false;

    if (isNumeric) {
        targetIndex = strtoull(idStr, nullptr, 10);
        if (targetIndex >= SPECTRUM_CAPACITY) {
            fprintf(stderr, "ERROR: index %llu exceeds spectrum capacity %llu\n",
                    targetIndex, (unsigned long long)SPECTRUM_CAPACITY);
            return 1;
        }
        useIndex = true;
    } else {
        if (strlen(idStr) != 60) {
            fprintf(stderr, "ERROR: identity must be 60 characters, got %zu\n", strlen(idStr));
            return 1;
        }
        if (!getPublicKeyFromIdentity((const unsigned char*)idStr, targetKey.m256i_u8)) {
            fprintf(stderr, "ERROR: invalid identity string (must be A-Z only)\n");
            return 1;
        }
    }

    FILE* f = nullptr;
    const int openError = openHostFile(&f, std::filesystem::path(filePath), HostFileMode::ReadBinary);
    if (openError != 0) {
        fprintf(stderr, "ERROR: cannot open spectrum file '%s': %s\n", filePath, strerror(openError));
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long long fileSize = ftell(f);
    rewind(f);

    const long long expectedSize = SPECTRUM_CAPACITY * (long long)sizeof(EntityRecord);
    if (fileSize != expectedSize) {
        fprintf(stderr, "ERROR: spectrum file size %lld does not match expected %lld\n",
                fileSize, expectedSize);
        fclose(f);
        return 1;
    }

    EntityRecord* buf = (EntityRecord*)malloc(expectedSize);
    if (!buf) {
        fprintf(stderr, "ERROR: failed to allocate %lld bytes\n", expectedSize);
        fclose(f);
        return 1;
    }

    if ((long long)fread(buf, 1, expectedSize, f) != expectedSize) {
        fprintf(stderr, "ERROR: short read\n");
        free(buf); fclose(f);
        return 1;
    }
    fclose(f);

    int foundIndex = -1;
    if (useIndex) {
        if (!isZero(buf[targetIndex].publicKey))
            foundIndex = (int)targetIndex;
    } else if (!isZero(targetKey)) {
        unsigned int idx = targetKey.m256i_u32[0] & (SPECTRUM_CAPACITY - 1);
        for (;;) {
            if (buf[idx].publicKey == targetKey) { foundIndex = (int)idx; break; }
            if (isZero(buf[idx].publicKey)) break;
            idx = (idx + 1) & (SPECTRUM_CAPACITY - 1);
        }
    }

    if (foundIndex < 0) {
        fprintf(stderr, "ERROR: entity '%s' not found\n", idStr);
        free(buf);
        return 1;
    }

    const EntityRecord& e = buf[foundIndex];
    char idBuf[61];
    getIdentityStr(e.publicKey.m256i_u8, idBuf, false);

    printf("Spectrum index:        %d\n", foundIndex);
    printf("Identity:              %s\n", idBuf);
    printf("Incoming amount:       %lld\n", e.incomingAmount);
    printf("Outgoing amount:       %lld\n", e.outgoingAmount);
    printf("Balance:               %lld\n", e.incomingAmount - e.outgoingAmount);
    printf("Incoming transfers:    %u\n", e.numberOfIncomingTransfers);
    printf("Outgoing transfers:    %u\n", e.numberOfOutgoingTransfers);
    printf("Last incoming tick:    %u\n", e.latestIncomingTransferTick);
    printf("Last outgoing tick:    %u\n", e.latestOutgoingTransferTick);

    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------
// read-score-cache
// ---------------------------------------------------------------------------

static int cmdReadScoreCache(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: qubic-cli --read-score-cache <score-file> <identity|seed|nonce> <value>\n");
        return 1;
    }
    const char* filePath = argv[2];
    const char* filterType = argv[3];
    const char* filterValue = argv[4];

    enum { FILTER_IDENTITY, FILTER_SEED, FILTER_NONCE } filter;
    if (strcmp(filterType, "identity") == 0)
        filter = FILTER_IDENTITY;
    else if (strcmp(filterType, "seed") == 0)
        filter = FILTER_SEED;
    else if (strcmp(filterType, "nonce") == 0)
        filter = FILTER_NONCE;
    else {
        fprintf(stderr, "ERROR: filter type must be 'identity', 'seed', or 'nonce'\n");
        return 1;
    }

    m256i targetKey, targetSeed, targetNonce;

    if (filter == FILTER_IDENTITY) {
        if (strlen(filterValue) != 60) {
            fprintf(stderr, "ERROR: identity must be 60 characters, got %zu\n", strlen(filterValue));
            return 1;
        }
        if (!getPublicKeyFromIdentity((const unsigned char*)filterValue, targetKey.m256i_u8)) {
            fprintf(stderr, "ERROR: invalid identity string (must be A-Z only)\n");
            return 1;
        }
    } else {
        if (!hexToBytes(filterValue, (filter == FILTER_SEED) ? targetSeed.m256i_u8 : targetNonce.m256i_u8, 32)) {
            fprintf(stderr, "ERROR: must be 64 hex characters, got %zu\n", strlen(filterValue));
            return 1;
        }
    }

    FILE* f = nullptr;
    const int openError = openHostFile(&f, std::filesystem::path(filePath), HostFileMode::ReadBinary);
    if (openError != 0) {
        fprintf(stderr, "ERROR: cannot open score cache file '%s': %s\n", filePath, strerror(openError));
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long long fileSize = ftell(f);
    rewind(f);

    const long long expectedSize = SCORE_CACHE_SIZE * (long long)sizeof(ScoreCacheEntry);
    if (fileSize != expectedSize) {
        fprintf(stderr, "ERROR: file size %lld does not match expected %lld (SCORE_CACHE_SIZE=%d, entry=%zu)\n",
                fileSize, expectedSize, SCORE_CACHE_SIZE, sizeof(ScoreCacheEntry));
        fclose(f);
        return 1;
    }

    ScoreCacheEntry* buf = (ScoreCacheEntry*)malloc(expectedSize);
    if (!buf) {
        fprintf(stderr, "ERROR: failed to allocate %lld bytes\n", expectedSize);
        fclose(f);
        return 1;
    }

    if ((long long)fread(buf, 1, expectedSize, f) != expectedSize) {
        fprintf(stderr, "ERROR: short read\n");
        free(buf); fclose(f);
        return 1;
    }
    fclose(f);

    int found = 0;
    for (int i = 0; i < SCORE_CACHE_SIZE; i++) {
        const ScoreCacheEntry& e = buf[i];
        if (isZero(e.publicKey)) continue;

        bool match = false;
        if (filter == FILTER_IDENTITY)
            match = (e.publicKey == targetKey);
        else if (filter == FILTER_SEED)
            match = (e.miningSeed == targetSeed);
        else
            match = (e.nonce == targetNonce);

        if (!match) continue;

        char idBuf[61];
        getIdentityStr(e.publicKey.m256i_u8, idBuf, false);

        char seedHex[65], nonceHex[65];
        bytesToHex(e.miningSeed.m256i_u8, 32, seedHex);
        bytesToHex(e.nonce.m256i_u8, 32, nonceHex);

        const unsigned char algoId = e.nonce.m256i_u8[0];
        const bool isBpp9000 = algoId == 1;
        const char* algo = isBpp9000 ? "BPP9000" : algoId == 0 ? "Neuraxon (reserved)" : "Unknown";
        const unsigned int score = (unsigned int)e.score;
        const bool valid = isBpp9000
            && score != std::numeric_limits<unsigned int>::max()
            && score <= BPP9000_WINDOW_WIDTH;
        const bool good = valid && score <= BPP9000_SOLUTION_THRESHOLD_DEFAULT;
        const char* scoreStatus = good ? "(good)" : valid ? "(above threshold)" : "(invalid)";

        printf("Cache index:    %d\n", i);
        printf("Identity:       %s\n", idBuf);
        printf("Mining seed:    %s\n", seedHex);
        printf("Nonce:          %s\n", nonceHex);
        printf("Algo:           %s\n", algo);
        printf("Score:          %u %s\n", score, scoreStatus);
        printf("---\n");
        found++;
    }

    printf("Total matches: %d\n", found);
    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: qubic-cli [--read-entity|--read-score-cache] ...\n");
        fprintf(stderr, "  --read-entity       <spectrum-file> <identity-or-index>\n");
        fprintf(stderr, "  --read-score-cache  <score-file> <identity|seed|nonce> <value>\n");
        return 1;
    }

    const char* cmd = argv[1];
    if (strcmp(cmd, "--read-entity") == 0)
        return cmdReadEntity(argc, argv);
    if (strcmp(cmd, "--read-score-cache") == 0)
        return cmdReadScoreCache(argc, argv);

    fprintf(stderr, "ERROR: unknown command '%s'\n", cmd);
    return 1;
}
