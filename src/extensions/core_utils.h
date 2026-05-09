#pragma once
#include "network_messages/common_def.h"
#include "network_messages/entity.h"
#include "platform/m256.h"
#include "spectrum/special_entities.h"
#include "spectrum/spectrum.h"

#include <functional>

static bool isComputorId(const m256i& id)
{
    for (int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        if (broadcastedComputors.computors.publicKeys[i] == id)
        {
            return true;
        }
    }

    return false;
}

static void forEachComputorId(const std::function<void(const m256i &)> & callback)
{
    for (int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        callback(broadcastedComputors.computors.publicKeys[i]);
    }
}

static EntityRecord* getEntityRecordFromComputorIndex(int index)
{
    m256i &publicKey = broadcastedComputors.computors.publicKeys[index];
    auto comSpectrumIndex = ::spectrumIndex(publicKey);
    if (comSpectrumIndex >= 0)
    {
        return &spectrum[comSpectrumIndex];
    }

    return nullptr;
}