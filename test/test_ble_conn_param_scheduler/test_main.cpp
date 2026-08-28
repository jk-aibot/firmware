// Pure state-machine suite for BleConnParamSchedulerT. Deliberately does NOT include TestUtil.h:
// like test_geocoord_distance, nothing here touches NodeDB, OSThread, RTC or sockets, so the
// harness-environment setup would only add globals this suite never uses. configuration.h is
// still required: it pulls Arduino.h, whose Common.h declares setup/loop extern "C" - without
// that these definitions get C++ linkage and portduino's main() cannot link to them. The
// scheduler is host-free by design - the Sender seam stands in for ble_gap_update_params - so
// every LL serialization invariant is encoded without hardware. Not verified on-device: the
// actual classic-ESP32 controller behavior still needs the hardware soak.

#include "configuration.h"
#include "nimble/BleConnParamScheduler.h"
#include <unity.h>
#include <vector>

namespace
{
constexpr uint16_t kHandle = 7;
constexpr uint16_t kOtherHandle = 9;

constexpr BleConnParams kHigh{24, 40, 0, 200};
constexpr BleConnParams kFast{6, 12, 0, 600};
constexpr BleConnParams kLow{24, 40, 2, 600};

// Scripted host: records every send and replays a programmed outcome per call (Sent once the
// script is exhausted, mirroring the common accept path).
struct ScriptedSender {
    struct Call {
        uint16_t connHandle;
        BleConnParams params;
    };

    std::vector<Call> calls;
    std::vector<BleConnParamSendResult> script;

    BleConnParamSendResult send(uint16_t connHandle, BleConnParams params)
    {
        calls.push_back({connHandle, params});
        if (script.empty())
            return BleConnParamSendResult::Sent;
        BleConnParamSendResult result = script.front();
        script.erase(script.begin());
        return result;
    }
};

BleConnParams sentParams(const ScriptedSender &sender, size_t index)
{
    return sender.calls[index].params;
}

void assertResult(BleConnParamSendResult actual, BleConnParamSendResult expected)
{
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

// Invariant 1: an idle scheduler submits immediately and reports Sent.
void test_idle_request_sends_immediately(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);

    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size());
    TEST_ASSERT_EQUAL_UINT16(kHandle, sender.calls[0].connHandle);
    TEST_ASSERT_TRUE(sentParams(sender, 0) == kHigh);
}

// Invariant 2: requests during an in-flight procedure never reach the host; only the latest
// desired parameters survive, and the completion submits exactly that one queued intent.
void test_requests_coalesce_to_latest_and_drain_once(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    assertResult(scheduler.request(kHandle, kFast), BleConnParamSendResult::Busy);
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size()); // still only the first procedure

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kLow); // latest, not the first queued desire

    // The drained submission is itself the one in-flight procedure: another completion submits
    // nothing, so the LL procedures can never overlap.
    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Drained);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
}

// Invariant 3: a completion with a failure status still ends the procedure and still drains the
// queued intent - the LL procedure is over either way.
void test_failed_completion_still_drains_queued_intent(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0x3e), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kLow);
}

// Invariant 4: a non-busy refusal starts no procedure and produces no completion, so the scheduler
// drops the intent and goes idle instead of wedging - the next request submits immediately.
void test_refused_request_drops_intent_and_stays_usable(void)
{
    ScriptedSender sender;
    sender.script.push_back(BleConnParamSendResult::Refused);
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Refused);
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size());

    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kLow);
}

// Invariant 5: a drained submission the host answers with EALREADY leaves the procedure claimed by
// the host's own in-flight work; its completion drains an empty queue and submits nothing.
void test_busy_drained_submission_waits_for_host_completion(void)
{
    ScriptedSender sender;
    sender.script.push_back(BleConnParamSendResult::Sent); // connect-time request goes out
    sender.script.push_back(BleConnParamSendResult::Busy); // drained intent hits EALREADY
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size());
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Busy);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size()); // the queued intent went out on the drain
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kLow);

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Drained);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size()); // and only once
}

// Invariant 6: a disconnect drops the in-flight procedure and any queued intent, so a reconnect on
// the same handle starts from a clean slate and submits immediately.
void test_disconnect_clears_queued_and_inflight(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size());
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);
    scheduler.onDisconnect(kHandle);

    assertResult(scheduler.request(kHandle, kFast), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kFast); // kLow was dropped with the connection
}

// Invariant 7: completions and disconnects for other connections are ignored - a peer-initiated
// update or a stale event must not drain or reset this connection's state.
void test_foreign_handle_events_are_ignored(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Ignored);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);
    assertResult(scheduler.onConnUpdateComplete(kOtherHandle, 0), BleConnParamSendResult::Ignored);
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size()); // still in flight, nothing drained

    scheduler.onDisconnect(kOtherHandle);
    assertResult(scheduler.request(kHandle, kFast), BleConnParamSendResult::Busy); // still queued
    TEST_ASSERT_EQUAL_UINT(1, sender.calls.size());
}

// Invariant 8: session teardown (BLE re-enable) resets even without a disconnect event.
void test_reset_clears_all_state(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);
    scheduler.reset();

    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Ignored);
    assertResult(scheduler.request(kHandle, kFast), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kFast);
}

// Invariant 9: a request naming a different connection abandons the old scope; state never
// carries across links even if the old disconnect event has not arrived yet.
void test_new_handle_rebinds_scope(void)
{
    ScriptedSender sender;
    BleConnParamSchedulerT<ScriptedSender> scheduler(sender);

    assertResult(scheduler.request(kHandle, kHigh), BleConnParamSendResult::Sent);
    assertResult(scheduler.request(kHandle, kLow), BleConnParamSendResult::Busy);
    assertResult(scheduler.request(kOtherHandle, kFast), BleConnParamSendResult::Sent);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
    TEST_ASSERT_EQUAL_UINT16(kOtherHandle, sender.calls[1].connHandle);
    TEST_ASSERT_TRUE(sentParams(sender, 1) == kFast);

    // The old connection's events no longer match: nothing drains, nothing resets the new scope.
    assertResult(scheduler.onConnUpdateComplete(kHandle, 0), BleConnParamSendResult::Ignored);
    assertResult(scheduler.onConnUpdateComplete(kOtherHandle, 0), BleConnParamSendResult::Drained);
    TEST_ASSERT_EQUAL_UINT(2, sender.calls.size());
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_request_sends_immediately);
    RUN_TEST(test_requests_coalesce_to_latest_and_drain_once);
    RUN_TEST(test_failed_completion_still_drains_queued_intent);
    RUN_TEST(test_refused_request_drops_intent_and_stays_usable);
    RUN_TEST(test_busy_drained_submission_waits_for_host_completion);
    RUN_TEST(test_disconnect_clears_queued_and_inflight);
    RUN_TEST(test_foreign_handle_events_are_ignored);
    RUN_TEST(test_reset_clears_all_state);
    RUN_TEST(test_new_handle_rebinds_scope);
    exit(UNITY_END());
}

void loop() {}
