// See UptimeClock.h for the full contract.
#include "UptimeClock.h"
#include <Arduino.h>
#include <atomic>
#ifdef ARCH_ESP32
#include "esp_timer.h"
#endif

uint32_t Time::getMillis()
{
#ifdef PIO_UNIT_TESTING
    if (Time::useTestClock.load(std::memory_order_relaxed))
        return Time::testNowMs.load(std::memory_order_relaxed);
#endif
    return millis();
}

namespace
{
struct PublishedSnapshot {
    std::atomic<uint32_t> high{0};
    std::atomic<uint32_t> low{0};
};

// The constexpr atomic initializers make both snapshots available before firmware startup.
PublishedSnapshot published[2];
std::atomic<uint32_t> publishedGeneration{0};
#ifdef PIO_UNIT_TESTING
std::atomic<Time::MonotonicPublishHook> monotonicPublishHook{nullptr};
#endif

// ESP32 uses its native 64-bit timer; other platforms retain the 32-bit
// getMillis() sample and wrap-carry composition.
uint64_t monotonicSampleMs()
{
#ifdef PIO_UNIT_TESTING
    if (Time::useTestNative64.load(std::memory_order_relaxed))
        return Time::testNowMs64.load(std::memory_order_relaxed);
    if (Time::useTestClock.load(std::memory_order_relaxed))
        return Time::testNowMs.load(std::memory_order_relaxed);
#endif
#if defined(ARCH_ESP32)
    return (uint64_t)esp_timer_get_time() / 1000;
#else
    return Time::getMillis();
#endif
}

#if defined(ARCH_ESP32) || defined(PIO_UNIT_TESTING)
// Native 64-bit samples are absolute, but a pure read can observe a forward sample before
// serviceMonotonic() publishes it; this shared high-water stops later lower samples retreating it.
std::atomic<uint64_t> native64HighWater{0};

uint64_t extendNative64(uint64_t candidate)
{
    uint64_t prev = native64HighWater.load(std::memory_order_relaxed);
    while (prev < candidate && !native64HighWater.compare_exchange_weak(prev, candidate, std::memory_order_relaxed))
        ;
    return prev >= candidate ? prev : candidate;
}
#endif

// Absolute samples never retreat; 32-bit samples extend the published low word
// with unsigned wrap-correct elapsed time.
uint64_t extendPublished(uint64_t base, uint64_t now)
{
#ifdef PIO_UNIT_TESTING
    if (Time::useTestNative64.load(std::memory_order_relaxed))
        return extendNative64(now > base ? now : base);
    if (Time::useTestClock.load(std::memory_order_relaxed))
        return base + (uint32_t)((uint32_t)now - (uint32_t)base);
#endif
#if defined(ARCH_ESP32)
    return extendNative64(now > base ? now : base);
#else
    return base + (uint32_t)((uint32_t)now - (uint32_t)base);
#endif
}

// A generation change means the writer completed a publish while this copy was being read. A
// paused publish leaves the generation unchanged and writes only the inactive snapshot.
void readPublished(uint32_t &high, uint32_t &low)
{
    for (;;) {
        const uint32_t before = publishedGeneration.load(std::memory_order_acquire);
        PublishedSnapshot &snapshot = published[before & 1u];
        high = snapshot.high.load(std::memory_order_relaxed);
        low = snapshot.low.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (publishedGeneration.load(std::memory_order_relaxed) == before)
            return;
    }
}
} // namespace

uint64_t Time::getMillisMonotonic()
{
    uint32_t high, low;
    readPublished(high, low);
    // The reader only advances the native-64 high-water; otherwise it composes the last
    // published snapshot with now.
    return extendPublished(((uint64_t)high << 32) | low, monotonicSampleMs());
}

uint32_t Time::getUptimeSecs()
{
    return (uint32_t)(getMillisMonotonic() / 1000);
}

void Time::serviceMonotonic()
{
    const uint32_t generation = publishedGeneration.load(std::memory_order_relaxed);
    PublishedSnapshot &active = published[generation & 1u];
    const uint32_t low = active.low.load(std::memory_order_relaxed);
    const uint32_t high = active.high.load(std::memory_order_relaxed);
    const uint64_t next = extendPublished(((uint64_t)high << 32) | low, monotonicSampleMs());

    PublishedSnapshot &inactive = published[(generation + 1u) & 1u];
    inactive.high.store((uint32_t)(next >> 32), std::memory_order_relaxed);
    inactive.low.store((uint32_t)next, std::memory_order_relaxed);
#ifdef PIO_UNIT_TESTING
    if (const auto hook = monotonicPublishHook.load(std::memory_order_relaxed))
        hook();
#endif
    publishedGeneration.store(generation + 1u, std::memory_order_release);
}

#ifdef PIO_UNIT_TESTING
void Time::resetMonotonicForTests()
{
    publishedGeneration.store(0, std::memory_order_relaxed);
    for (auto &snapshot : published) {
        snapshot.high.store(0, std::memory_order_relaxed);
        snapshot.low.store(0, std::memory_order_relaxed);
    }
    monotonicPublishHook.store(nullptr, std::memory_order_relaxed);
    Time::useTestNative64.store(false, std::memory_order_relaxed);
    Time::testNowMs64.store(0, std::memory_order_relaxed);
    native64HighWater.store(0, std::memory_order_relaxed);
}

void Time::setMonotonicPublishHookForTests(MonotonicPublishHook hook)
{
    monotonicPublishHook.store(hook, std::memory_order_relaxed);
}
#endif
