#pragma once

#include <kangaroo_twelve.h>

#include <cstddef>
#include <cstring>
#include <vector>

// Chaining-value cache for one contract state: KangarooTwelve hashes every full K12_chunkSize chunk
// after the first as an independent leaf, so only the leaves of changed chunks are rehashed. Chunk 0
// and the partial tail are always re-read; their change flags are never cleared.
class K12DigestCache
{
protected:
    struct Intermediate
    {
        unsigned char intermediate[K12_capacityInBytes];
    };

    // Per-chunk chaining values and dirty flags.
    std::vector<Intermediate> intermediateMap;
    std::vector<unsigned char> isChunkChangedMap;
    unsigned int maxChunks;
    unsigned char *_state;
    size_t _stateSize;

    unsigned char *_lastOutput;
    size_t _lastOutputSize;

public:
    K12DigestCache(unsigned char *state, size_t stateSize)
    {
        _state = state;
        _stateSize = stateSize;
        maxChunks = (stateSize + K12_chunkSize - 1) / K12_chunkSize;

        isChunkChangedMap.resize(maxChunks);
        intermediateMap.resize(maxChunks);
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            isChunkChangedMap[i] = true;
        }
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            std::memset(intermediateMap[i].intermediate, 0, K12_capacityInBytes);
        }

        _lastOutput = new unsigned char[1024];
        _lastOutputSize = 0;
    }

    ~K12DigestCache()
    {
        delete[] _lastOutput;
    }

    // Digest of the state, bit-identical to KangarooTwelve(state, stateSize, output, 32). Returns 0 on
    // success. With useCache == false every leaf is rehashed but still cached.
    int getHash(unsigned char* output, size_t outputByteLen, bool useCache = true)
    {
        if (outputByteLen != K12_capacityInBytes)
            return 1;
        if (_lastOutput && outputByteLen == _lastOutputSize && isAllChunksUnchanged())
        {
            // Reuse the complete digest when no chunk changed.
            std::memcpy(output, _lastOutput, outputByteLen);
            return 0;
        }

        KangarooTwelveStream stream;
        stream.init();
        if (_stateSize <= K12_chunkSize)
        {
            stream.update(_state, _stateSize);
        }
        else
        {
            stream.update(_state, K12_chunkSize);
            size_t offset = K12_chunkSize;
            for (unsigned int chunkIndex = 1; offset + K12_chunkSize <= _stateSize; chunkIndex++, offset += K12_chunkSize)
            {
                Intermediate &inter = intermediateMap[chunkIndex];
                if (isChunkChangedMap[chunkIndex] || !useCache)
                {
                    KangarooTwelveLeaf(_state + offset, inter.intermediate);
                    isChunkChangedMap[chunkIndex] = false;
                }
                stream.absorbChainingValue(inter.intermediate);
            }
            stream.update(_state + offset, _stateSize - offset);
        }
        stream.finalize(output);

        // Cache the complete digest.
        std::memcpy(_lastOutput, output, outputByteLen);
        _lastOutputSize = outputByteLen;

        return 0;
    }

    void markChunkChanged(unsigned int chunkIndex)
    {
        if (chunkIndex < maxChunks)
        {
            isChunkChangedMap[chunkIndex] = true;
        }
    }

    bool isAllChunksUnchanged() const
    {
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            if (isChunkChangedMap[i])
            {
                return false;
            }
        }
        return true;
    }

    unsigned int getMaxChunks() const
    {
        return maxChunks;
    }
};
