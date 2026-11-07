#pragma once

// Available system memory (MemAvailable in /proc/meminfo), for code that must leave headroom on
// smaller machines. Returns -1 when unknown (not Linux, or /proc/meminfo unreadable): callers then
// keep their default behaviour.

#if defined(__linux__)

#include <cstdio>
#include <cstdlib>
#include <cstring>

static inline long long availableMemoryBytes()
{
    FILE* meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo)
    {
        return -1;
    }
    char line[128];
    long long availableKb = -1;
    while (fgets(line, sizeof(line), meminfo))
    {
        if (strncmp(line, "MemAvailable:", 13) == 0)
        {
            availableKb = strtoll(line + 13, nullptr, 10);
            break;
        }
    }
    fclose(meminfo);
    return availableKb < 0 ? -1 : availableKb * 1024;
}

#else

static inline long long availableMemoryBytes()
{
    return -1;
}

#endif
