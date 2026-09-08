#pragma once

#include <cstdint>
#include <mutex>

// Outcome of submitting a connection-parameter intent or processing its completion.
enum class BleConnParamSendResult {
    Sent,    // host accepted this intent; its completion is outstanding
    Busy,    // intent is retained until the outstanding procedure completes
    Refused, // host rejected the intent and no completion will follow
    Drained, // completion processed with no retained intent
    Ignored  // completion did not match scheduler-owned state
};

// Connection parameters in BLE units: intervals in 1.25 ms, supervision timeout in 10 ms.
struct BleConnParams {
    uint16_t minInterval;
    uint16_t maxInterval;
    uint16_t latency;
    uint16_t timeout;
};

constexpr bool operator==(const BleConnParams &a, const BleConnParams &b)
{
    return a.minInterval == b.minInterval && a.maxInterval == b.maxInterval && a.latency == b.latency && a.timeout == b.timeout;
}

// Keeps at most one connection-parameter procedure outstanding and coalesces
// concurrent requests to the latest desired parameters.
template <typename Sender> class BleConnParamSchedulerT
{
  public:
    explicit BleConnParamSchedulerT(Sender &sender) : sender(sender) {}

    BleConnParamSendResult request(uint16_t connHandle, BleConnParams params)
    {
        if (connHandle == kNoConnection)
            return BleConnParamSendResult::Refused;

        std::lock_guard<std::mutex> guard(mutex);
        bindLocked(connHandle);
        desired = params;
        hasDesired = true;
        if (inFlight)
            return BleConnParamSendResult::Busy;
        return dispatchLocked(connHandle);
    }

    BleConnParamSendResult onConnUpdateComplete(uint16_t connHandle, int status)
    {
        (void)status;
        std::lock_guard<std::mutex> guard(mutex);
        if (connHandle != boundHandle || !inFlight)
            return BleConnParamSendResult::Ignored;

        inFlight = false;
        if (!hasDesired)
            return BleConnParamSendResult::Drained;
        return dispatchLocked(connHandle);
    }

    void onDisconnect(uint16_t connHandle)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (connHandle == boundHandle)
            clearLocked();
    }

    void reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        clearLocked();
    }

  private:
    static constexpr uint16_t kNoConnection = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE

    void bindLocked(uint16_t connHandle)
    {
        if (connHandle != boundHandle) {
            boundHandle = connHandle;
            hasDesired = false;
            inFlight = false;
        }
    }

    BleConnParamSendResult dispatchLocked(uint16_t connHandle)
    {
        const BleConnParamSendResult result = sender.send(connHandle, desired);
        if (result == BleConnParamSendResult::Sent) {
            hasDesired = false;
            inFlight = true;
        } else if (result == BleConnParamSendResult::Busy) {
            // EALREADY means another procedure owns the next completion; retain the intent for retry.
            inFlight = true;
        } else {
            hasDesired = false;
            inFlight = false;
        }
        return result;
    }

    void clearLocked()
    {
        boundHandle = kNoConnection;
        hasDesired = false;
        inFlight = false;
    }

    Sender &sender;
    std::mutex mutex;
    uint16_t boundHandle = kNoConnection;
    BleConnParams desired{};
    bool hasDesired = false;
    bool inFlight = false;
};
