// Focused parity check for incremental K12 digest caching.

#define NO_UEFI

#include <gtest/gtest.h>

#include "platform/msvc_polyfill.h"
#include "platform/file_io.h"
#include "platform/m256.h"
#include <extensions/k12_digest_cache.h>

#include <array>
#include <vector>

namespace
{

std::array<unsigned char, 32> canonicalHash(const unsigned char* state, size_t size)
{
    std::array<unsigned char, 32> hash{};
    KangarooTwelve(state, (unsigned int)size, hash.data(), (unsigned int)hash.size());
    return hash;
}

} // namespace

TEST(K12DigestCacheTest, IncrementalDigestEqualsCanonicalK12)
{
    constexpr size_t size = 2 * 1024 * 1024 + 37;
    std::vector<unsigned char> state(size);
    K12DigestCache digestCache(state.data(), state.size());

    for (size_t i = 0; i < state.size(); i++)
    {
        state[i] = (unsigned char)((i * 1315423911ULL) >> 17);
    }

    std::array<unsigned char, 32> hash{};
    EXPECT_EQ(digestCache.getHash(hash.data(), hash.size()), 0);
    EXPECT_EQ(hash, canonicalHash(state.data(), state.size()));

    for (unsigned int chunk = 1; chunk < digestCache.getMaxChunks(); chunk += 17)
    {
        state[(size_t)chunk * K12_chunkSize] ^= 0x5A;
        digestCache.markChunkChanged(chunk);
    }

    EXPECT_EQ(digestCache.getHash(hash.data(), hash.size()), 0);
    EXPECT_EQ(hash, canonicalHash(state.data(), state.size()));
}
