#pragma once

// Replay of one bpp9000 network at 32 neurons, returning the Rating and leaving the shift that
// ScoreBpp9000::advanceShift() would. The state fits one register, so a tick never touches memory, and
// the network has no input, so once a state repeats the remaining emits are copied instead of ticked.

#ifndef LITE_BPP9000_NO_FAST_REPLAY

#include <lib/platform_common/qintrin.h>

#include "mining/rating.h"

namespace LiteBpp9000
{

// Off routes every replay back to the engine's own kernel.
inline bool fastReplayEnabled = true;

template<typename Engine>
struct FastReplay
{
    static constexpr unsigned long long NEURONS = 32;
    static constexpr unsigned long long SLOTS = 3;
    static constexpr unsigned long long LUT_ENTRIES = 27;
    // A dword lane holds four neurons, one per byte.
    static constexpr unsigned long long NEURONS_PER_LANE = 4;

    static constexpr bool supported = (Engine::populationThreshold == NEURONS)
        && (Engine::numberOfNeighbors == SLOTS)
        && (Engine::lutSize == LUT_ENTRIES)
        && (Engine::numberOfOutputNeurons == 1);

    // The current network in the form the tick consumes.
    struct Network
    {
#ifdef __AVX512F__
        __m256i gatherIndex[SLOTS];
#else
        // A byte shuffle stays inside its 128-bit half, so each slot selects from both halves.
        __m256i gatherFromLowHalf[SLOTS];
        __m256i gatherFromHighHalf[SLOTS];
#endif
        // Bit e of a lane is the neuron's LUT entry e. The high trit bit is stored one position up, so
        // the shift that selects the entry also puts it at bit 1.
        __m256i lutLowBit[NEURONS_PER_LANE];
        __m256i lutHighBit[NEURONS_PER_LANE];
    };

    // One entry per emit; a replay never needs more emits than the task has targets.
    struct EmitLog
    {
        unsigned char outputTrit[Engine::sequenceLength];
        unsigned int tick[Engine::sequenceLength];
    };

    struct Run
    {
        __m256i state;
        unsigned long long ticks;
        unsigned long long emits;
        // Brent cycle search: the state is compared to one kept from a power-of-two tick.
        unsigned long long keptStateKey;
        unsigned long long ticksSinceKept;
        unsigned long long keepInterval;
        // Zero until the state has repeated.
        unsigned long long periodTicks;
        unsigned long long periodEmits;
    };

    static void buildNetwork(const Engine& engine, Network& network)
    {
        alignas(32) unsigned char control[2][NEURONS];
        for (unsigned long long slot = 0; slot < SLOTS; ++slot)
        {
            for (unsigned long long n = 0; n < NEURONS; ++n)
            {
                const unsigned int source = engine.neighborIndices[n * SLOTS + slot];
#ifdef __AVX512F__
                control[0][n] = (unsigned char)source;
#else
                // 0x80 makes the shuffle yield zero, so the two halves can be OR-ed.
                control[0][n] = (unsigned char)((source < 16) ? source : 0x80);
                control[1][n] = (unsigned char)((source >= 16) ? (source - 16) : 0x80);
#endif
            }
#ifdef __AVX512F__
            network.gatherIndex[slot] = _mm256_load_si256((const __m256i*)(const void*)control[0]);
#else
            network.gatherFromLowHalf[slot] = _mm256_load_si256((const __m256i*)(const void*)control[0]);
            network.gatherFromHighHalf[slot] = _mm256_load_si256((const __m256i*)(const void*)control[1]);
#endif
        }

        alignas(32) unsigned int lowBit[NEURONS_PER_LANE][NEURONS / NEURONS_PER_LANE];
        alignas(32) unsigned int highBit[NEURONS_PER_LANE][NEURONS / NEURONS_PER_LANE];
        for (unsigned long long n = 0; n < NEURONS; ++n)
        {
            unsigned int low = 0;
            unsigned int high = 0;
            for (unsigned long long entry = 0; entry < LUT_ENTRIES; ++entry)
            {
                const unsigned int trit = engine.curLut[n * LUT_ENTRIES + entry];
                low |= (trit & 1u) << entry;
                high |= ((trit >> 1) & 1u) << (entry + 1);
            }
            lowBit[n % NEURONS_PER_LANE][n / NEURONS_PER_LANE] = low;
            highBit[n % NEURONS_PER_LANE][n / NEURONS_PER_LANE] = high;
        }
        for (unsigned long long k = 0; k < NEURONS_PER_LANE; ++k)
        {
            network.lutLowBit[k] = _mm256_load_si256((const __m256i*)(const void*)lowBit[k]);
            network.lutHighBit[k] = _mm256_load_si256((const __m256i*)(const void*)highBit[k]);
        }
    }

    static inline __m256i lookUp(__m256i lowBit, __m256i highBit, __m256i entry)
    {
        const __m256i low = _mm256_and_si256(_mm256_srlv_epi32(lowBit, entry), _mm256_set1_epi32(1));
        const __m256i high = _mm256_and_si256(_mm256_srlv_epi32(highBit, entry), _mm256_set1_epi32(2));
        return _mm256_or_si256(low, high);
    }

    static inline __m256i tick(const Network& network, __m256i state)
    {
#ifdef __AVX512F__
        const __m256i t0 = _mm256_permutexvar_epi8(network.gatherIndex[0], state);
        const __m256i t1 = _mm256_permutexvar_epi8(network.gatherIndex[1], state);
        const __m256i t2 = _mm256_permutexvar_epi8(network.gatherIndex[2], state);
#else
        const __m256i lowHalf = _mm256_permute2x128_si256(state, state, 0x00);
        const __m256i highHalf = _mm256_permute2x128_si256(state, state, 0x11);
        const __m256i t0 = _mm256_or_si256(_mm256_shuffle_epi8(lowHalf, network.gatherFromLowHalf[0]),
            _mm256_shuffle_epi8(highHalf, network.gatherFromHighHalf[0]));
        const __m256i t1 = _mm256_or_si256(_mm256_shuffle_epi8(lowHalf, network.gatherFromLowHalf[1]),
            _mm256_shuffle_epi8(highHalf, network.gatherFromHighHalf[1]));
        const __m256i t2 = _mm256_or_si256(_mm256_shuffle_epi8(lowHalf, network.gatherFromLowHalf[2]),
            _mm256_shuffle_epi8(highHalf, network.gatherFromHighHalf[2]));
#endif
        // LUT entry t0 + 3 * t1 + 9 * t2, the multiples taken from a three-entry table.
        const __m256i times3 = _mm256_setr_epi8(0, 3, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        const __m256i times9 = _mm256_setr_epi8(0, 9, 18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        const __m256i entry = _mm256_add_epi8(t0, _mm256_add_epi8(_mm256_shuffle_epi8(times3, t1), _mm256_shuffle_epi8(times9, t2)));

        const __m256i byteMask = _mm256_set1_epi32(0xFF);
        const __m256i entry0 = _mm256_and_si256(entry, byteMask);
        const __m256i entry1 = _mm256_and_si256(_mm256_srli_epi32(entry, 8), byteMask);
        const __m256i entry2 = _mm256_and_si256(_mm256_srli_epi32(entry, 16), byteMask);
        const __m256i entry3 = _mm256_srli_epi32(entry, 24);

        const __m256i next0 = lookUp(network.lutLowBit[0], network.lutHighBit[0], entry0);
        const __m256i next1 = lookUp(network.lutLowBit[1], network.lutHighBit[1], entry1);
        const __m256i next2 = lookUp(network.lutLowBit[2], network.lutHighBit[2], entry2);
        const __m256i next3 = lookUp(network.lutLowBit[3], network.lutHighBit[3], entry3);
        return _mm256_or_si256(_mm256_or_si256(next0, _mm256_slli_epi32(next1, 8)),
            _mm256_or_si256(_mm256_slli_epi32(next2, 16), _mm256_slli_epi32(next3, 24)));
    }

    // Bit n of lowBits / highBits is the low / high bit of neuron n's trit.
    static inline void tritBits(__m256i state, unsigned int& lowBits, unsigned int& highBits)
    {
        lowBits = (unsigned int)_mm256_movemask_epi8(_mm256_slli_epi64(state, 7));
        highBits = (unsigned int)_mm256_movemask_epi8(_mm256_slli_epi64(state, 6));
    }

    // Extends the log to `needed` emits. False when the tick budget runs out first.
    static bool ensureEmits(const Engine& engine, const Network& network, EmitLog& log, Run& run, unsigned long long needed)
    {
        if (run.periodTicks == 0)
        {
            const unsigned int control = engine.controlIndex;
            const unsigned int output = engine.outputIndex;
            // Locals keep the tick chain in registers.
            __m256i state = run.state;
            unsigned long long ticks = run.ticks;
            unsigned long long emits = run.emits;
            unsigned long long keptStateKey = run.keptStateKey;
            unsigned long long ticksSinceKept = run.ticksSinceKept;
            unsigned long long keepInterval = run.keepInterval;
            unsigned long long periodTicks = 0;
            bool outOfTicks = false;

            while (emits < needed)
            {
                if (++ticks >= Engine::maxNumberOfTicks)
                {
                    outOfTicks = true;
                    break;
                }
                state = tick(network, state);

                unsigned int lowBits, highBits;
                tritBits(state, lowBits, highBits);
                // Written every tick and kept only when the control neuron emits, so the loop has no
                // data-dependent branch.
                log.outputTrit[emits] = (unsigned char)(((lowBits >> output) & 1u) | (((highBits >> output) & 1u) << 1));
                log.tick[emits] = (unsigned int)ticks;
                emits += ((highBits >> control) & 1u) ^ 1u;

                const unsigned long long stateKey = lowBits | ((unsigned long long)highBits << 32);
                if (stateKey == keptStateKey)
                {
                    periodTicks = ticksSinceKept;
                    break;
                }
                if (ticksSinceKept == keepInterval)
                {
                    keptStateKey = stateKey;
                    keepInterval <<= 1;
                    ticksSinceKept = 1;
                }
                else
                {
                    ticksSinceKept++;
                }
            }

            run.state = state;
            run.ticks = ticks;
            run.emits = emits;
            run.keptStateKey = keptStateKey;
            run.ticksSinceKept = ticksSinceKept;
            run.keepInterval = keepInterval;
            if (outOfTicks)
            {
                return false;
            }
            if (periodTicks != 0)
            {
                unsigned long long emitsInPeriod = 0;
                while (emitsInPeriod < emits && log.tick[emits - 1 - emitsInPeriod] > ticks - periodTicks)
                {
                    emitsInPeriod++;
                }
                run.periodTicks = periodTicks;
                run.periodEmits = emitsInPeriod;
            }
        }

        if (run.emits >= needed)
        {
            return true;
        }
        // A period without an emit never produces another one.
        if (run.periodEmits == 0)
        {
            return false;
        }
        while (run.emits < needed)
        {
            const unsigned long long emitTick = (unsigned long long)log.tick[run.emits - run.periodEmits] + run.periodTicks;
            if (emitTick >= Engine::maxNumberOfTicks)
            {
                return false;
            }
            log.outputTrit[run.emits] = log.outputTrit[run.emits - run.periodEmits];
            log.tick[run.emits] = (unsigned int)emitTick;
            run.emits++;
        }
        return true;
    }

    static inline unsigned int isWrong(const Engine& engine, const EmitLog& log, unsigned long long emit)
    {
        return (log.outputTrit[emit] != engine.targetOutputs[emit]) ? 1u : 0u;
    }

    static score_engine::Rating advanceShift(Engine& engine)
    {
        static_assert(supported, "the replay is built for 32 neurons with 3 neighbours");
        static_assert(Engine::shiftCap + Engine::windowWidth <= Engine::sequenceLength, "the emit log must hold the deepest frame");

        Network network;
        buildNetwork(engine, network);

        // Per thread rather than on the stack: the two arrays are tens of kilobytes.
        static thread_local EmitLog log;

        Run run;
        run.state = _mm256_loadu_si256((const __m256i*)(const void*)engine.curInitial);
        run.ticks = 0;
        run.emits = 0;
        unsigned int lowBits, highBits;
        tritBits(run.state, lowBits, highBits);
        run.keptStateKey = lowBits | ((unsigned long long)highBits << 32);
        run.ticksSinceKept = 1;
        run.keepInterval = 1;
        run.periodTicks = 0;
        run.periodEmits = 0;

        const unsigned long long windowWidth = Engine::windowWidth;
        const unsigned long long firstFrame = engine.shift;
        if (!ensureEmits(engine, network, log, run, firstFrame + windowWidth))
        {
            return score_engine::Rating{ Engine::INFINITE_ERROR, (unsigned int)firstFrame };
        }

        unsigned int wrongInFrame = 0;
        for (unsigned long long emit = firstFrame; emit < firstFrame + windowWidth; ++emit)
        {
            wrongInFrame += isWrong(engine, log, emit);
        }

        unsigned long long frame = firstFrame;
        while (wrongInFrame <= Engine::advanceThreshold && frame < Engine::shiftCap)
        {
            if (!ensureEmits(engine, network, log, run, frame + windowWidth + 1))
            {
#ifdef __AVX512F__
                // The engine's one-pass kernel leaves the shift alone when a later frame times out.
                return score_engine::Rating{ Engine::INFINITE_ERROR, (unsigned int)firstFrame };
#else
                // The engine's per-frame kernel has already moved on to the frame that times out.
                engine.shift = frame + 1;
                return score_engine::Rating{ Engine::INFINITE_ERROR, (unsigned int)(frame + 1) };
#endif
            }
            wrongInFrame += isWrong(engine, log, frame + windowWidth);
            wrongInFrame -= isWrong(engine, log, frame);
            frame++;
        }

        engine.shift = frame;
        return score_engine::Rating{ wrongInFrame, (unsigned int)frame };
    }
};

}

#endif
