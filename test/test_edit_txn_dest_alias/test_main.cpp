// Edit-transaction destination alias: keeps a phone session bound to the pre-rekey self
// when an in-RAM PKI rotation moves nodeDB->getNodeNum() mid-session. Alias lives on
// AdminModule; MeshService::handleToRadio() rewrites p.to from the alias to the live self
// on every phone-originated admin packet. See test_handleToRadio_* for the rewrite guard.

#include "MeshTypes.h" // include BEFORE TestUtil.h
#include "TestUtil.h"
#include "mesh/Router.h"
#include <cstring>
#include <unity.h>

#include "SyntheticFixtures.h"
#include "mesh/NodeDB.h"
#include "mesh/PhoneAPI.h"
#include "modules/RoutingModule.h"
#include "support/AdminModuleTestShim.h"
#include "support/MockMeshService.h"

namespace
{
// A radio interface that just releases the packet back to packetPool - keeps the Router's `iface`
// non-null so sendLocal()'s !iface branch does not turn remote-bound packets into NO_INTERFACE NAKs.
class CaptureRadio : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t, bool = false) override { return 0; }
};

// A NAK-absorbing stand-in for the global routingModule (production installs one in Modules.cpp).
// A decoded admin DM whose destination has no key in the fresh NodeDB - the post-commit, remote
// and non-admin-port cases - is refused by Router::send's PKI path with PKI_SEND_FAIL_PUBLIC_KEY
// and NAKs through this seam instead of crashing on a null global.
class LocalSessionPhoneAPITestShim : public PhoneAPI
{
  public:
    uint32_t localSessionId() { return getLocalAdminSessionId(); }

  protected:
    bool checkIsConnected() override { return true; }
};

class MockRoutingModule : public RoutingModule
{
  public:
    void sendAckNak(meshtastic_Routing_Error, NodeNum, PacketId, ChannelIndex, uint8_t = 0, bool = false,
                    const meshtastic_MeshPacket * = nullptr) override
    {
    }
};

// A router that delivers local-bound packets by releasing them (so dispatchReceived
// runs the AdminModule once and any reply it allocates is the test's responsibility
// to drain) and has a real radio interface so remote-bound packets don't take the
// !iface path.
class MockRouter : public Router
{
  public:
    MockRouter() { addInterface(std::make_unique<CaptureRadio>()); }
    ~MockRouter() override
    {
        delete cryptLock;
        cryptLock = nullptr;
    }

    void enqueueReceivedMessage(meshtastic_MeshPacket *p) override { packetPool.release(p); }
};
} // namespace

static MockMeshService *mockService = nullptr;
static AdminModuleTestShim *admin = nullptr;
static MockRouter *mockRouter = nullptr;
static MockRoutingModule *mockRoutingModule = nullptr;
static NodeDB *testNodeDB = nullptr;
static AdminModule *savedAdminModule = nullptr;
static NodeDB *savedNodeDB = nullptr;
static Router *savedRouter = nullptr;
static RoutingModule *savedRoutingModule = nullptr;
static MeshService *savedService = nullptr;
static meshtastic_Config_SecurityConfig_admin_key_t savedAdminKey0;
static pb_size_t savedAdminKeyCount = 0;

// One-shot setup: install service/admin/router/routing-module/nodeDB pair, snapshot admin_key[0], reset my_node_num.
void setUp(void)
{
    savedAdminModule = adminModule;
    savedNodeDB = nodeDB;
    savedRouter = router;
    savedRoutingModule = routingModule;
    savedService = service;
    // Snapshot admin_key[0] so sendRemoteBegin's write is reverted per-test. The auth gate consults
    // slot 0; other slots and the count are not touched.
    savedAdminKey0 = config.security.admin_key[0];
    savedAdminKeyCount = config.security.admin_key_count;

    mockService = new MockMeshService();
    service = mockService;
    admin = new AdminModuleTestShim();
    adminModule = admin; // wire the global so MeshService::handleToRadio's guard fires
    admin->setLocalSession(1);
    testNodeDB = new NodeDB();
    nodeDB = testNodeDB;
    mockRouter = new MockRouter();
    router = mockRouter;
    mockRoutingModule = new MockRoutingModule();
    routingModule = mockRoutingModule;
    myNodeInfo.my_node_num = ORIGINAL_SELF;
}

void tearDown(void)
{
    admin->drainReply(); // any reply allocated by the AdminModule during handleToRadio lands back in the pool
    delete admin;
    admin = nullptr;
    adminModule = savedAdminModule;
    service = savedService;
    delete mockService;
    mockService = nullptr;
    router = savedRouter;
    delete mockRouter;
    mockRouter = nullptr;
    routingModule = savedRoutingModule;
    delete mockRoutingModule;
    mockRoutingModule = nullptr;
    nodeDB = savedNodeDB;
    delete testNodeDB;
    testNodeDB = nullptr;
    // Restore admin_key[0] and the slot count, undoing any write sendRemoteBegin made this test.
    config.security.admin_key[0] = savedAdminKey0;
    config.security.admin_key_count = savedAdminKeyCount;
}

// Local begin: from==0, addressed to current self (mirrors what PhoneAPI sends).
static void sendBegin(uint32_t sessionId = 1)
{
    admin->setLocalSession(sessionId);
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
    m.begin_edit_settings = true;
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = 0;
    mp.to = nodeDB->getNodeNum();
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    admin->handleReceivedProtobuf(mp, &m);
}

static void sendBeginTo(NodeNum dest, uint32_t sessionId = 1)
{
    admin->setLocalSession(sessionId);
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
    m.begin_edit_settings = true;
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = 0;
    mp.to = dest;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    admin->handleReceivedProtobuf(mp, &m);
}

static void sendCommit(uint32_t sessionId = 1)
{
    admin->setLocalSession(sessionId);
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
    m.commit_edit_settings = true;
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = 0;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    admin->handleReceivedProtobuf(mp, &m);
}

// Remote begin: PKC auth (admin_key[0] + matching public_key) + session_passkey minted via setPassKey.
// setUp/tearDown snapshot and restore admin_key[0] so this helper is safe to call across the suite.
static void sendRemoteBegin()
{
    config.security.admin_key[0].size = 32;
    memcpy(config.security.admin_key[0].bytes, REMOTE_KEY, 32);

    meshtastic_AdminMessage probe = meshtastic_AdminMessage_init_zero;
    admin->setPassKey(&probe);
    TEST_ASSERT_EQUAL(8, probe.session_passkey.size);

    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
    m.begin_edit_settings = true;
    m.session_passkey = probe.session_passkey;

    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = STRANGER_REMOTE;
    mp.to = ORIGINAL_SELF;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.pki_encrypted = true;
    mp.public_key.size = 32;
    memcpy(mp.public_key.bytes, REMOTE_KEY, 32);

    admin->handleReceivedProtobuf(mp, &m);
}

static void sendRemoteCommit()
{
    config.security.admin_key[0].size = 32;
    memcpy(config.security.admin_key[0].bytes, REMOTE_KEY, 32);

    meshtastic_AdminMessage probe = meshtastic_AdminMessage_init_zero;
    admin->setPassKey(&probe);

    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
    m.commit_edit_settings = true;
    m.session_passkey = probe.session_passkey;

    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = STRANGER_REMOTE;
    mp.to = nodeDB->getNodeNum();
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.pki_encrypted = true;
    mp.public_key.size = 32;
    memcpy(mp.public_key.bytes, REMOTE_KEY, 32);

    admin->handleReceivedProtobuf(mp, &m);
}

static bool decodeRoutingError(meshtastic_MeshPacket *reply, meshtastic_Routing_Error &out)
{
    if (!reply || reply->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        reply->decoded.portnum != meshtastic_PortNum_ROUTING_APP)
        return false;
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    if (!pb_decode_from_bytes(reply->decoded.payload.bytes, reply->decoded.payload.size, &meshtastic_Routing_msg, &routing) ||
        routing.which_variant != meshtastic_Routing_error_reason_tag)
        return false;
    out = routing.error_reason;
    return true;
}

// Allocates a reply so the pool stays clean; used to drive expireStaleEditTransaction.
static void sendGetDeviceMetadata(uint32_t sessionId = 1)
{
    admin->setLocalSession(sessionId);
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_request_tag;
    m.get_device_metadata_request = true;
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
    mp.from = 0;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    admin->handleReceivedProtobuf(mp, &m);
    admin->drainReply();
}

// Decoded admin packet for handleToRadio. Any admin payload works - canonicalization runs before
// the per-portnum handler.
static meshtastic_MeshPacket makeAdminPacket(NodeNum to)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.to = to;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p.id = 1;
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_request_tag;
    m.get_device_metadata_request = true;
    p.decoded.payload.size =
        pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_AdminMessage_msg, &m);
    return p;
}

// Decoded state-changing admin packet used to exercise session ownership through MeshService.
// Carries want_response like a real committing client: it must learn the commit verdict.
static meshtastic_MeshPacket makeCommitAdminPacket(NodeNum to)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.to = to;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p.decoded.want_response = true;
    p.id = 2;
    meshtastic_AdminMessage m = meshtastic_AdminMessage_init_zero;
    m.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
    m.commit_edit_settings = true;
    p.decoded.payload.size =
        pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_AdminMessage_msg, &m);
    return p;
}

// Decoded non-admin packet - exercises the portnum guard.
static meshtastic_MeshPacket makeNonAdminPacket(NodeNum to)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.to = to;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    return p;
}

// -----------------------------------------------------------------------
// Alias lifecycle on AdminModule
// -----------------------------------------------------------------------

static void test_alias_isCapturedAtBegin(void)
{
    // begin_edit_settings captures the begin packet's addressed self (mp.to).
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
}

// After commit the alias is cleared; the next packet addressed to the old self lands as-is.
static void test_alias_isClearedOnCommit(void)
{
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    sendCommit();
    TEST_ASSERT_EQUAL_UINT32(0, admin->getEditTransactionOriginalDest());
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
}

// Idle-window expiry clears the alias alongside the rest of the transaction state.
static void test_alias_isClearedOnExpiry(void)
{
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    admin->ageEditTransaction();
    sendGetDeviceMetadata(); // any later admin message triggers expireStaleEditTransaction()

    TEST_ASSERT_EQUAL_UINT32(0, admin->getEditTransactionOriginalDest());
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
}
// Alias survives in-RAM rekeys inside an open transaction.
static void test_alias_isRetainedAcrossMultipleRekeys(void)
{
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    myNodeInfo.my_node_num = POST_REKEY_SELF;
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    myNodeInfo.my_node_num = SECOND_REKEY_SELF;
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    myNodeInfo.my_node_num = THIRD_REKEY_SELF;
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());
}

// Repeating begin after a rekey must not replace the transaction's original destination.
static void test_alias_repeatedBeginAfterRekey_preservesOriginalDest(void)
{
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());

    myNodeInfo.my_node_num = POST_REKEY_SELF;
    sendBegin();

    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());
}

static void test_alias_remoteCommit_doesNotTerminateLocalTransaction(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    sendRemoteCommit();

    meshtastic_Routing_Error error = meshtastic_Routing_Error_NONE;
    TEST_ASSERT_TRUE_MESSAGE(decodeRoutingError(admin->reply(), error), "non-owner commit must return a routing error");
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_BAD_REQUEST, error);
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());
    admin->drainReply();

    sendCommit();
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
}

// A remote PKC begin must NOT plant a local alias; a follow-up local begin still captures its own self.
static void test_alias_remoteBegin_doesNotCapture(void)
{
    sendRemoteBegin();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, admin->getEditTransactionOriginalDest(),
                                     "a remote PKC begin must not plant a local alias");

    admin->ageEditTransaction();
    sendGetDeviceMetadata();

    // A later local transaction captures its own self once the remote-owned transaction is gone.
    sendBegin();
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, admin->getEditTransactionOriginalDest());
}

static void test_alias_broadcastBegin_doesNotCapture()
{
    sendBeginTo(NODENUM_BROADCAST);
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    TEST_ASSERT_EQUAL_UINT32(0, admin->getEditTransactionOriginalDest());
    sendCommit();
}

static void test_phoneApi_localSessions_areDistinctAndRenewOnClose()
{
    LocalSessionPhoneAPITestShim first;
    LocalSessionPhoneAPITestShim second;
    const uint32_t firstId = first.localSessionId();
    const uint32_t secondId = second.localSessionId();
    TEST_ASSERT_NOT_EQUAL(0, firstId);
    TEST_ASSERT_NOT_EQUAL(0, secondId);
    TEST_ASSERT_NOT_EQUAL(firstId, secondId);

    first.close();
    const uint32_t renewedId = first.localSessionId();
    TEST_ASSERT_NOT_EQUAL(0, renewedId);
    TEST_ASSERT_NOT_EQUAL(firstId, renewedId);
}

static void test_alias_secondLocalSession_cannotCommitFirstSession()
{
    sendBegin(1);
    sendCommit(2);

    meshtastic_Routing_Error error = meshtastic_Routing_Error_NONE;
    TEST_ASSERT_TRUE(decodeRoutingError(admin->reply(), error));
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_BAD_REQUEST, error);
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    admin->drainReply();

    sendCommit(1);
    TEST_ASSERT_FALSE(admin->editTransactionOpen());
}

// -----------------------------------------------------------------------
// MeshService::handleToRadio() integration - drives the production rewrite seam.
// -----------------------------------------------------------------------

// No rekey: handleToRadio must not rewrite a packet whose destination already matches current self.
static void test_handleToRadio_unchangedIdentity_noRewrite(void)
{
    sendBegin(); // alias = ORIGINAL_SELF, currentSelf = ORIGINAL_SELF
    meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, p.to);
    sendCommit();
}

// One rekey: a packet addressed to the pre-rotation self is rewritten to the current self.
static void test_handleToRadio_oneRekey_rewritesToCurrentSelf(void)
{
    sendBegin(); // alias = ORIGINAL_SELF
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(POST_REKEY_SELF, p.to, "pre-rotation-self packet must be rewritten");
    sendCommit();
}

// Multiple rekeys: handleToRadio always rewrites to the *current* self.
static void test_handleToRadio_multipleRekeys_eachRewrittenToCurrentSelf(void)
{
    sendBegin(); // alias = ORIGINAL_SELF
    myNodeInfo.my_node_num = POST_REKEY_SELF;
    {
        meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
        service->handleToRadio(p, 1);
        TEST_ASSERT_EQUAL_UINT32(POST_REKEY_SELF, p.to);
    }

    myNodeInfo.my_node_num = SECOND_REKEY_SELF;
    {
        meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
        service->handleToRadio(p, 1);
        TEST_ASSERT_EQUAL_UINT32(SECOND_REKEY_SELF, p.to);
    }
}

// Post-commit: alias is cleared, so no further rewrites even on packets addressed to the old self.
static void test_handleToRadio_afterCommit_noRewrite(void)
{
    sendBegin();                              // alias = ORIGINAL_SELF
    myNodeInfo.my_node_num = POST_REKEY_SELF; // (the rekey happened during the session)
    sendCommit();                             // alias cleared

    meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, p.to);
}

static void test_handleToRadio_afterIdleExpiry_noRewrite(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;
    admin->ageEditTransaction();

    meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, p.to);
}

// Encrypted payload: variant guard short-circuits before the rewrite.
static void test_handleToRadio_encryptedPayload_notRewritten(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.to = ORIGINAL_SELF;
    p.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    p.encrypted.size = 4;
    p.id = 1;
    service->handleToRadio(p);
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, p.to);
    sendCommit();
}

// Non-admin port: portnum guard short-circuits before the rewrite.
static void test_handleToRadio_nonAdminPort_notRewritten(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    meshtastic_MeshPacket p = makeNonAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p);
    TEST_ASSERT_EQUAL_UINT32(ORIGINAL_SELF, p.to);
    sendCommit();
}

// Remote destination: the packetDest==origDest guard misses, so the destination is left as-is.
static void test_handleToRadio_remoteDestination_notRewritten(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    meshtastic_MeshPacket p = makeAdminPacket(STRANGER_REMOTE);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(STRANGER_REMOTE, p.to, "a packet addressed to a remote must keep that destination");
    sendCommit();
}

// Broadcast destination: the rewrite must not redirect a broadcast back at self.
static void test_handleToRadio_broadcastDestination_notRewritten(void)
{
    sendBegin();
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    meshtastic_MeshPacket p = makeAdminPacket(NODENUM_BROADCAST);
    service->handleToRadio(p, 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(NODENUM_BROADCAST, p.to, "a broadcast packet must stay a broadcast");
    sendCommit();
}

static void test_handleToRadio_nonOwnerSession_doesNotUseOwnerAlias()
{
    sendBegin(1);
    myNodeInfo.my_node_num = POST_REKEY_SELF;

    meshtastic_MeshPacket p = makeAdminPacket(ORIGINAL_SELF);
    service->handleToRadio(p, 2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ORIGINAL_SELF, p.to, "another local session must not inherit the owner's alias");
    sendCommit(1);
}

static void test_handleToRadio_nonOwnerSession_stateChangeIsRejected()
{
    sendBegin(1);
    meshtastic_MeshPacket p = makeCommitAdminPacket(nodeDB->getNodeNum());
    service->handleToRadio(p, 2);

    // The module loop hands solicited replies to MeshService for the phone, so drain the queue a
    // real client reads from - admin->reply() is always empty once the loop has dispatched.
    meshtastic_MeshPacket *toPhone = service->getForPhone();
    meshtastic_Routing_Error error = meshtastic_Routing_Error_NONE;
    TEST_ASSERT_TRUE_MESSAGE(decodeRoutingError(toPhone, error), "non-owner local commit must return a routing error");
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_BAD_REQUEST, error);
    if (toPhone)
        packetPool.release(toPhone);
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    sendCommit(1);
}

static void test_handleToRadio_zeroSession_cannotBorrowOwnerContext()
{
    sendBegin(1);
    meshtastic_MeshPacket p = makeCommitAdminPacket(nodeDB->getNodeNum());
    service->handleToRadio(p, 0);

    meshtastic_MeshPacket *toPhone = service->getForPhone();
    meshtastic_Routing_Error error = meshtastic_Routing_Error_NONE;
    TEST_ASSERT_TRUE_MESSAGE(decodeRoutingError(toPhone, error), "zero-session commit must be rejected");
    TEST_ASSERT_EQUAL(meshtastic_Routing_Error_BAD_REQUEST, error);
    if (toPhone)
        packetPool.release(toPhone);
    TEST_ASSERT_TRUE(admin->editTransactionOpen());
    sendCommit(1);
}

// -----------------------------------------------------------------------
// Test runner
// -----------------------------------------------------------------------

void setup()
{
    delay(10);
    delay(2000);
    initializeTestEnvironment();

    UNITY_BEGIN();

    // Alias lifecycle
    RUN_TEST(test_alias_isCapturedAtBegin);
    RUN_TEST(test_alias_isClearedOnCommit);
    RUN_TEST(test_alias_isClearedOnExpiry);
    RUN_TEST(test_alias_isRetainedAcrossMultipleRekeys);
    RUN_TEST(test_alias_repeatedBeginAfterRekey_preservesOriginalDest);
    RUN_TEST(test_alias_remoteCommit_doesNotTerminateLocalTransaction);
    RUN_TEST(test_alias_remoteBegin_doesNotCapture);
    RUN_TEST(test_alias_broadcastBegin_doesNotCapture);
    RUN_TEST(test_phoneApi_localSessions_areDistinctAndRenewOnClose);
    RUN_TEST(test_alias_secondLocalSession_cannotCommitFirstSession);

    // MeshService::handleToRadio integration
    RUN_TEST(test_handleToRadio_unchangedIdentity_noRewrite);
    RUN_TEST(test_handleToRadio_oneRekey_rewritesToCurrentSelf);
    RUN_TEST(test_handleToRadio_multipleRekeys_eachRewrittenToCurrentSelf);
    RUN_TEST(test_handleToRadio_afterCommit_noRewrite);
    RUN_TEST(test_handleToRadio_afterIdleExpiry_noRewrite);
    RUN_TEST(test_handleToRadio_encryptedPayload_notRewritten);
    RUN_TEST(test_handleToRadio_nonAdminPort_notRewritten);
    RUN_TEST(test_handleToRadio_remoteDestination_notRewritten);
    RUN_TEST(test_handleToRadio_broadcastDestination_notRewritten);
    RUN_TEST(test_handleToRadio_nonOwnerSession_doesNotUseOwnerAlias);
    RUN_TEST(test_handleToRadio_nonOwnerSession_stateChangeIsRejected);
    RUN_TEST(test_handleToRadio_zeroSession_cannotBorrowOwnerContext);

    exit(UNITY_END());
}

void loop() {}
