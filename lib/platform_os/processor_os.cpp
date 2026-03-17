#include <lib/platform_common/processor.h>

unsigned long long mainProcessorID = -1;

unsigned long long getRunningProcessorID()
{
    return mainProcessorID;
}
