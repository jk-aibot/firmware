#pragma once
// The one class AdminModule.h befriends (test seam): exposes the protected/private handlers to the
// native suites and defers persistence so setters exercise in-RAM logic without disk/reboot effects.
#include "MeshTypes.h" // packetPool
#include "modules/AdminModule.h"

class AdminModuleTestShim : public AdminModule
{
  public:
    using AdminModule::checkPassKey; // session-key gate seam (see test_admin_session_repro)
    using AdminModule::handleGetConfig;
    using AdminModule::handleGetModuleConfig;
    using AdminModule::handleReceivedProtobuf;
    using AdminModule::handleSetConfig;
    using AdminModule::handleSetModuleConfig;
    using AdminModule::handleSetOwner;
    using AdminModule::responseIsSolicited; // request/response pairing gate
    using AdminModule::setPassKey;

    // Peek at the reply a handler queued, before drainReply() releases it.
    meshtastic_MeshPacket *reply() { return myReply; }

    // With an "open edit transaction" saveChanges() is a pure no-op: no reloadConfig/saveToDisk/reboot.
    // Stamps the clock like begin_edit_settings and claims local ownership the way a real phone
    // client's begin does, so the owner-mismatch gate admits this suite's from==0 setters.
    void deferSaves()
    {
        if (!hasOpenEditTransaction) {
            editTransactionOwner = 0;
            editTransactionLocalOwner = 1;
        }
        setLocalAdminSessionForDispatch(1);
        hasOpenEditTransaction = true;
        editTransactionActivityMs = millis();
        deferredEditReboot = false;
    }
    int savedSegments() const { return lastSaveWhatForTest; }
    bool editTransactionNeedsReboot() const { return deferredEditReboot; }

    bool editTransactionOpen() const { return hasOpenEditTransaction; }
    void setLocalSession(uint32_t id) { setLocalAdminSessionForDispatch(id); }
    // Backdate past the idle window so a test sees an abandoned transaction without waiting it out.
    void ageEditTransaction() { editTransactionActivityMs = millis() - EDIT_TRANSACTION_IDLE_MS - 1; }

    // Setters may allocate an error reply from packetPool; drain it each iteration or the pool leaks.
    void drainReply()
    {
        if (myReply) {
            packetPool.release(myReply);
            myReply = nullptr;
        }
    }
};
