#include "syncengine/HybridLogicalClock.h"

#include <algorithm>
#include <chrono>

namespace syncengine {

static constexpr int kLogicalBits = 16;
static constexpr uint64_t kLogicalMask = (1ULL << kLogicalBits) - 1;

HybridLogicalClock::HybridLogicalClock()
{
    m_hlc = pack(wallClockMs(), 0);
}

uint64_t HybridLogicalClock::now()
{
    std::lock_guard lock(m_mutex);

    uint64_t physNow = wallClockMs();
    uint64_t oldPhys = physicalComponent(m_hlc);
    uint16_t oldLogical = logicalComponent(m_hlc);

    if (physNow > oldPhys) {
        m_hlc = pack(physNow, 0);
    } else {
        m_hlc = pack(oldPhys, oldLogical + 1);
    }
    return m_hlc;
}

uint64_t HybridLogicalClock::receive(uint64_t remoteHlc)
{
    std::lock_guard lock(m_mutex);

    uint64_t physNow = wallClockMs();
    uint64_t localPhys = physicalComponent(m_hlc);
    uint16_t localLogical = logicalComponent(m_hlc);
    uint64_t remotePhys = physicalComponent(remoteHlc);
    uint16_t remoteLogical = logicalComponent(remoteHlc);

    uint64_t maxPhys = std::max({physNow, localPhys, remotePhys});

    if (maxPhys == localPhys && maxPhys == remotePhys) {
        // All three are equal -- take max logical + 1
        m_hlc = pack(maxPhys, std::max(localLogical, remoteLogical) + 1);
    } else if (maxPhys == localPhys) {
        m_hlc = pack(maxPhys, localLogical + 1);
    } else if (maxPhys == remotePhys) {
        m_hlc = pack(maxPhys, remoteLogical + 1);
    } else {
        // Wall clock is ahead of both
        m_hlc = pack(maxPhys, 0);
    }
    return m_hlc;
}

uint64_t HybridLogicalClock::physicalComponent(uint64_t hlc)
{
    return hlc >> kLogicalBits;
}

uint16_t HybridLogicalClock::logicalComponent(uint64_t hlc)
{
    return static_cast<uint16_t>(hlc & kLogicalMask);
}

uint64_t HybridLogicalClock::pack(uint64_t physicalMs, uint16_t logical)
{
    return (physicalMs << kLogicalBits) | logical;
}

uint64_t HybridLogicalClock::current() const
{
    std::lock_guard lock(m_mutex);
    return m_hlc;
}

uint64_t HybridLogicalClock::wallClockMs() const
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace syncengine
