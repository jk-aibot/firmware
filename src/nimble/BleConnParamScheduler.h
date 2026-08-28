#pragma once

#include <cstdint>
#include <mutex>

// Outcome of handing a connection-parameter intent to the BLE host or of processing a completion.
enum class BleConnParamSendResult {
    Sent,    // host accepted the request; one LL update procedure is now in flight
    Busy,    // a procedure is already outstanding (BLE_HS_EALREADY); its completion will still arrive
    Refused, // no procedure was started and none will be (link gone, stack disabled, invalid params)
    Drained, // completion processed, nothing was queued; scheduler is idle
    Ignored  // completion matched no in-flight procedure (stale handle or peer-initiated update)
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

// Serializes BLE LL connection-parameter updates: at most one procedure in flight per connection,
// concurrent requests coalesce to the latest parameters and are submitted when the in-flight
// procedure completes (success or failure), and disconnect/reset clears all connection-scoped
// state. Sender translates the host return code so this stays host-free and natively testable.
// Sends happen under the mutex on purpose: the NimBLE host enqueues a procedure without
// dispatching GAP events synchronously, so no re-entry can observe the half-updated state.
template <typename Sender> class BleConnParamSchedulerT
{
  public:
    explicit BleConnParamSchedulerT(Sender &sender) : sender(sender) {}

    // Record the latest desired parameters; submit them now unless a procedure is in flight.
    BleConnParamSendResult request(uint16_t connHandle, BleConnParams params)
    {
        if (connHandle == kNoConnection)
            return BleConnParamSendResult::Refused;
        std::lock_guard<std::mutex> guard(mutex);
        bindLocked(connHandle);
        desired = params;
        hasDesired = true;
        if (inFlight)
            return BleConnParamSendResult::Busy; // queued; the in-flight completion submits it
        hasDesired = false;
        inFlight = true;
        return dispatchLocked(connHandle, params);
    }

    // BLE_GAP_EVENT_CONN_UPDATE: the procedure ended (status 0 or not); submit the latest intent.
    BleConnParamSendResult onConnUpdateComplete(uint16_t connHandle, int status)
    {
        (void)status;
        std::lock_guard<std::mutex> guard(mutex);
        if (connHandle != boundHandle || !inFlight)
            return BleConnParamSendResult::Ignored; // peer-initiated update, or a stale event
        inFlight = false;
        if (!hasDesired)
            return BleConnParamSendResult::Drained;
        BleConnParams params = desired;
        hasDesired = false;
        inFlight = true;
        return dispatchLocked(connHandle, params);
    }

    // BLE_GAP_EVENT_DISCONNECT for this connection: drop every connection-scoped intent.
    void onDisconnect(uint16_t connHandle)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (connHandle == boundHandle)
            clearLocked();
    }

    // Session teardown (BLE re-enable, deinit): drop every connection-scoped intent.
    void reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        clearLocked();
    }

  private:
    static constexpr uint16_t kNoConnection = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE

    // A request on a new handle means the previous connection is gone regardless of its disconnect
    // event; connection-scoped state never carries across links.
    void bindLocked(uint16_t connHandle)
    {
        if (connHandle != boundHandle) {
            boundHandle = connHandle;
            hasDesired = false;
            inFlight = false;
        }
    }

    BleConnParamSendResult dispatchLocked(uint16_t connHandle, BleConnParams params)
    {
        BleConnParamSendResult result = sender.send(connHandle, params);
        if (result == BleConnParamSendResult::Refused) {
            // No completion event follows this failure: drop the intent instead of wedging the queue.
            inFlight = false;
        }
        // Sent: our completion event clears inFlight. Busy: the host's own procedure completion does.
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
