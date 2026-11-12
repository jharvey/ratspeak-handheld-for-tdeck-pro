// Simulator only: the process's real memory use, for the firmware's "hb" heartbeat line.
// The simulated heap_caps_* numbers are fixed (plausible ESP32-S3 values), so a leak in
// the firmware shows up here instead.
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <cstddef>

namespace sim {
size_t processBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return 0;
    return pmc.PrivateUsage;
}
}  // namespace sim
