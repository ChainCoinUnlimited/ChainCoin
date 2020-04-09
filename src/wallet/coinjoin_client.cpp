// Copyright (c) 2019 The CoinJoin! developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/coinjoin_client.h>

#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <modules/masternode/masternode_payments.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/masternode/masternode_man.h>
#include <script/sign.h>
#include <shutdown.h>
#include <util/moneystr.h>
#include <util/string.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/psbtwallet.h>
#include <wallet/scriptpubkeyman.h>

#include <numeric>
#include <memory>

void CKeyHolderStorage::AddKey(CScript &script, CWallet* pwalletIn)
{
    OutputType output_type = pwalletIn->m_default_change_type != OutputType::CHANGE_AUTO ? pwalletIn->m_default_change_type : pwalletIn->m_default_address_type;
    if (output_type == OutputType::LEGACY) {
        LogPrintf("%s CKeyHolderStorage::%s -- Error: Only SegWit addresses are supported for mixing\n", pwalletIn->GetDisplayName(), __func__);
        return;
    }
    std::shared_ptr<ReserveDestination> reservedest = std::make_shared<ReserveDestination>(pwalletIn);;
    CTxDestination dest;
    if (!reservedest->GetReservedDestination(output_type, dest, true)) {
        LogPrintf("%s CKeyHolderStorage::%s -- Warning: Keypool ran out, trying to top up\n", pwalletIn->GetDisplayName(), __func__);
        pwalletIn->TopUpKeyPool();
        if (!reservedest->GetReservedDestination(output_type, dest, true)) {
            LogPrintf("%s CKeyHolderStorage::%s -- Error: Failed to obtain key from keypool\n", pwalletIn->GetDisplayName(), __func__);
            return;
        }
    }

    script = GetScriptForDestination(dest);

    LOCK(cs_storage);
    storage.emplace_back(std::move(reservedest));
    LogPrintf("%s CKeyHolderStorage::AddKey -- storage size %lld\n", pwalletIn->GetDisplayName(), storage.size());
}

void CKeyHolderStorage::KeepAll()
{
    std::vector<std::shared_ptr<ReserveDestination> > tmp;
    {
        // don't hold cs_storage while calling KeepKey(), which might lock cs_wallet
        LOCK(cs_storage);
        std::swap(storage, tmp);
    }

    if (tmp.size() > 0) {
        for (auto &dest : tmp) {
            dest->KeepDestination();
        }
        LogPrint(BCLog::CJOIN, "CKeyHolderStorage::KeepAll -- %lld keys kept\n", tmp.size());
    }
}

void CKeyHolderStorage::ReturnAll()
{
    std::vector<std::shared_ptr<ReserveDestination> > tmp;
    {
        // don't hold cs_storage while calling ReturnKey(), which might lock cs_wallet
        LOCK(cs_storage);
        std::swap(storage, tmp);
    }

    if (tmp.size() > 0) {
        for (auto &dest : tmp) {
            dest->ReturnDestination();
        }
        LogPrint(BCLog::CJOIN, "CKeyHolderStorage::ReturnAll -- %lld keys returned\n", tmp.size());
    }
}

void CCoinJoinClientManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return; // ignore all CoinJoin related functionality
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (pfrom->GetSendVersion() < MIN_COINJOIN_PEER_PROTO_VERSION) {
        LogPrintf("%s CCoinJoinClientManager::ProcessMessage -- peer=%d using obsolete version %i\n", m_wallet->GetDisplayName(), pfrom->GetId(), pfrom->GetSendVersion());
        return;
    }

    if (!CheckDiskSpace((GetDataDir()))) {
        ResetPool();
        fEnableCoinJoin = false;
        strAutoCoinJoinResult = _("WARNING: Low disk space, disabling CoinJoin.").translated;
        LogPrintf("%s CCoinJoinClientManager::ProcessMessage -- Not enough disk space, disabling CoinJoin.\n", m_wallet->GetDisplayName());
        return;
    }

    if (strCommand == NetMsgType::CJQUEUE) {

        CCoinJoinQueue queue;
        vRecv >> queue;

        if (queue.IsExpired(nCachedBlockHeight)) return;
        if (queue.nHeight > nCachedBlockHeight + 1) return;

        // enable for network analysis only!!!
        // LogPrint(BCLog::CJOIN, "%s CJQUEUE -- %s new from %s\n", m_wallet->GetDisplayName(), queue.ToString(), pfrom->addr.ToStringIPPort());

        masternode_info_t infoMn;
        if (!mnodeman.GetMasternodeInfo(queue.masternodeOutpoint, infoMn) || !queue.CheckSignature(infoMn.pubKeyMasternode)) {
            // we probably have outdated info
            m_wallet->chain().askForMN(pfrom, queue.masternodeOutpoint);
            LogPrintf("%s CJQUEUE -- Masternode for CoinJoin queue (%s) not found, requesting.\n", m_wallet->GetDisplayName(), queue.ToString());
            return;
        }

        {
            LOCK(cs_vecqueue);
            // process every queue only once
            // status has changed, update and remove if closed
            for (std::vector<CCoinJoinQueue>::iterator it = vecCoinJoinQueue.begin(); it!=vecCoinJoinQueue.end(); ++it) {
                if (*it == queue) {
                    LogPrint(BCLog::CJOIN, "%s CJQUEUE -- seen CoinJoin queue (%s) from masternode %s, vecCoinJoinQueue size: %d from %s\n",
                             m_wallet->GetDisplayName(), queue.ToString(), infoMn.addr.ToString(), GetQueueSize(), pfrom->addr.ToStringIPPort());
                    return;
                } else if (*it != queue) {
                    LogPrint(BCLog::CJOIN, "%s CJQUEUE -- updated CoinJoin queue (%s) from masternode %s, vecCoinJoinQueue size: %d from %s\n",
                             m_wallet->GetDisplayName(), queue.ToString(), infoMn.addr.ToString(), GetQueueSize(), pfrom->addr.ToStringIPPort());
                    if (queue.status > it->status) it->status = queue.status;
                } else if (it->masternodeOutpoint == queue.masternodeOutpoint) {
                    // refuse to create another queue this often
                    LogPrint(BCLog::CJOIN, "%s CJQUEUE -- last request is still in queue, return.\n", m_wallet->GetDisplayName());
                    return;
                }
            }
        }


        switch (queue.status) {
        case STATUS_CLOSED:
        case STATUS_OPEN:
        {
            LOCK(cs_vecqueue);
            vecCoinJoinQueue.emplace_back(queue);
            queue.Relay();
            LogPrint(BCLog::CJOIN, "%s CJQUEUE -- %s CoinJoin queue (%s) from masternode %s, vecCoinJoinQueue size: %d from %s\n",
                     m_wallet->GetDisplayName(), queue.status == STATUS_CLOSED ? strprintf("closed") : strprintf("new"), queue.ToString(),
                     infoMn.addr.ToString(), GetQueueSize(), pfrom->addr.ToStringIPPort());
            // see if we can join unless we are a LP
        }
            if (!nLiquidityProvider && fEnableCoinJoin && !fActive) CoinJoin();
            return;
        case STATUS_READY:
        case STATUS_FULL:
        {
            // we might have timed out
            LOCK(cs_deqsessions);
            if (deqSessions.empty()) return;
            for (auto& session : deqSessions) {
                masternode_info_t mnMixing;
                if (session.GetMixingMasternodeInfo(mnMixing) && mnMixing.addr == infoMn.addr && session.GetState() == POOL_STATE_QUEUE) {
                    LogPrint(BCLog::CJOIN, "%s CJQUEUE -- CoinJoin queue (%s) is ready on masternode %s\n", m_wallet->GetDisplayName(), queue.ToString(), infoMn.addr.ToString());
                    session.SendDenominate();
                    return;
                }
            }
        }
            return;
        case STATUS_REJECTED:
        case STATUS_ACCEPTED:
            return;
        }
    } else if (
        strCommand == NetMsgType::CJSTATUSUPDATE ||
        strCommand == NetMsgType::CJFINALTX ||
        strCommand == NetMsgType::CJCOMPLETE) {
        LOCK(cs_deqsessions);
        for (auto& session : deqSessions) {
            session.ProcessMessage(pfrom, strCommand, vRecv);
        }
    }
}

void CCoinJoinClientSession::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return; // ignore all CoinJoin related functionality
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (!infoMixingMasternode.fInfoValid) return;
    if (infoMixingMasternode.addr != pfrom->addr) return;

    if (strCommand == NetMsgType::CJSTATUSUPDATE) {

        int nMsgSessionID;
        int nMsgState;
        int nMsgEntriesCount;
        int nMsgStatusUpdate;
        int nMsgMessageID;
        vRecv >> nMsgSessionID >> nMsgState >> nMsgEntriesCount >> nMsgStatusUpdate >> nMsgMessageID;

        if (nMsgState < POOL_STATE_MIN || nMsgState > POOL_STATE_MAX) {
            LogPrint(BCLog::CJOIN, "%s CJSTATUSUPDATE -- nMsgState is out of bounds: %d\n", m_wallet_session->GetDisplayName(), nMsgState);
            return;
        }

        if (nMsgStatusUpdate < STATUS_REJECTED || nMsgStatusUpdate > STATUS_ACCEPTED) {
            LogPrint(BCLog::CJOIN, "%s CJSTATUSUPDATE -- nMsgStatusUpdate is out of bounds: %d\n", m_wallet_session->GetDisplayName(), nMsgStatusUpdate);
            return;
        }

        if (nMsgMessageID < MSG_POOL_MIN || nMsgMessageID > MSG_POOL_MAX) {
            LogPrint(BCLog::CJOIN, "%s CJSTATUSUPDATE -- nMsgMessageID is out of bounds: %d\n", m_wallet_session->GetDisplayName(), nMsgMessageID);
            return;
        }

        bool updated = CheckPoolStateUpdate(PoolState(nMsgState), nMsgEntriesCount, PoolStatusUpdate(nMsgStatusUpdate), PoolMessage(nMsgMessageID), nMsgSessionID);
        LogPrint(BCLog::CJOIN, "%s CJSTATUSUPDATE -- CheckPoolStateUpdate: %s: nMsgSessionID %d  nMsgState: %d  nEntriesCount: %d  nMsgStatusUpdate: %d  nMsgMessageID %d (%s)\n",
                 m_wallet_session->GetDisplayName(),
                 updated ? strprintf("updated") : strprintf("no action"),
                 nMsgSessionID,
                 nMsgState,
                 nEntriesCount,
                 nMsgStatusUpdate,
                 nMsgMessageID,
                 CCoinJoin::GetMessageByID(PoolMessage(nMsgMessageID)));

    } else if (strCommand == NetMsgType::CJFINALTX) {

        CCoinJoinBroadcastTx psbtxFinal;
        vRecv >> psbtxFinal;

        if (!psbtxFinal.CheckSignature(infoMixingMasternode.pubKeyMasternode)) {
            // we probably have outdated info
            m_wallet_session->chain().askForMN(pfrom, psbtxFinal.masternodeOutpoint);
            return;
        }

        if (nSessionID != psbtxFinal.nSessionID) {
            LogPrint(BCLog::CJOIN, "%s CJFINALTX -- message doesn't match current CoinJoin session: nSessionID: %d  nMsgSessionID: %d\n", m_wallet_session->GetDisplayName(), nSessionID, psbtxFinal.nSessionID);
            return;
        }

        //check to see if input is spent already? (and probably not confirmed)
        SignFinalTransaction(psbtxFinal.psbtx, pfrom);

    } else if (strCommand == NetMsgType::CJCOMPLETE) {

        int nMsgSessionID;
        int nMsgMessageID;
        vRecv >> nMsgSessionID >> nMsgMessageID;

        if (nMsgMessageID < MSG_POOL_MIN || nMsgMessageID > MSG_POOL_MAX) {
            LogPrint(BCLog::CJOIN, "%s CJCOMPLETE -- nMsgMessageID is out of bounds: %d\n", m_wallet_session->GetDisplayName(), nMsgMessageID);
            return;
        }

        if (nSessionID != nMsgSessionID) {
            LogPrint(BCLog::CJOIN, "%s CJCOMPLETE -- message doesn't match current CoinJoin session: nSessionID: %d  nMsgSessionID: %d\n", m_wallet_session->GetDisplayName(), nSessionID, nMsgSessionID);
            return;
        }

        LogPrint(BCLog::CJOIN, "%s CJCOMPLETE -- nMsgSessionID %d  nMsgMessageID %d (%s)\n", m_wallet_session->GetDisplayName(), nMsgSessionID, nMsgMessageID, CCoinJoin::GetMessageByID(PoolMessage(nMsgMessageID)));

        CompletedTransaction(PoolMessage(nMsgMessageID));
    }
}

void CCoinJoinClientManager::ResetPool()
{
    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::ResetPool -- resetting.\n", m_wallet->GetDisplayName());
    LOCK(cs_deqsessions);
    nCachedLastSuccessBlock = 0;
    vecMasternodesUsed.clear();
    UnlockCoins();
    for (auto& session : deqSessions) {
        session.SetNull();
    }
    deqSessions.clear();
    CCoinJoinBaseManager::SetNull();
    fActive = false;
    fStartup = false;
}

void CCoinJoinClientSession::SetNull()
{
    // Client side
    nEntriesCount = 0;
    fLastEntryAccepted = false;
    UnlockCoins();
    keyHolderStorage.ReturnAll();
    infoMixingMasternode = masternode_info_t();
    pendingCJaRequest = CPendingCJaRequest();

    CCoinJoinBaseSession::SetNull();
}

//
// Unlock coins after mixing fails or succeeds
//
void CCoinJoinClientManager::UnlockCoins()
{
    while(m_wallet) {
        TRY_LOCK(m_wallet->cs_wallet, lockWallet);
        if (!lockWallet) {MilliSleep(50); continue;}
        for (const auto& outpoint : vecOutPointLocked)
            m_wallet->UnlockCoin(outpoint);
        break;
    }

    vecOutPointLocked.clear();
}

void CCoinJoinClientSession::UnlockCoins()
{
    while(m_wallet_session) {
        TRY_LOCK(m_wallet_session->cs_wallet, lockWallet);
        if (!lockWallet) {MilliSleep(50); continue;}
        for (const auto& outpoint : vecOutPointLocked)
            m_wallet_session->UnlockCoin(outpoint);
        break;
    }

    vecOutPointLocked.clear();
}

std::string CCoinJoinClientSession::GetStatus(bool fWaitForBlock)
{
    static int nStatusMessageProgress = 0;
    nStatusMessageProgress += 10;
    std::string strSuffix = "";

    if (fWaitForBlock || !masternodeSync.IsBlockchainSynced())
        return strAutoCoinJoinResult;

    switch(nState) {
        case POOL_STATE_IDLE:
            return _("CoinJoin is idle.").translated;
        case POOL_STATE_CONNECTING:
            return strAutoCoinJoinResult;
        case POOL_STATE_QUEUE:
            if (     nStatusMessageProgress % 70 <= 30) strSuffix = ".";
            else if (nStatusMessageProgress % 70 <= 50) strSuffix = "..";
            else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
            return strprintf(_("Submitted to masternode, waiting in queue %s").translated, strSuffix);;
        case POOL_STATE_ACCEPTING_ENTRIES:
            if (nEntriesCount == 0) {
                nStatusMessageProgress = 0;
                return strAutoCoinJoinResult;
            } else if (fLastEntryAccepted) {
                if (nStatusMessageProgress % 10 > 8) {
                    fLastEntryAccepted = false;
                    nStatusMessageProgress = 0;
                }
                return _("CoinJoin request complete:").translated + " " + _("Your transaction was accepted into the pool!").translated;
            } else {
                if (     nStatusMessageProgress % 70 <= 40) return strprintf(_("Submitted following entries to masternode: %u / %d").translated, nEntriesCount, CCoinJoin::GetMaxPoolInputs());
                else if (nStatusMessageProgress % 70 <= 50) strSuffix = ".";
                else if (nStatusMessageProgress % 70 <= 60) strSuffix = "..";
                else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
                return strprintf(_("Submitted to masternode, waiting for more entries ( %u / %d ) %s").translated, nEntriesCount, CCoinJoin::GetMaxPoolInputs(), strSuffix);
            }
        case POOL_STATE_SIGNING:
            if (     nStatusMessageProgress % 70 <= 40) return _("Found enough users, signing ...").translated;
            else if (nStatusMessageProgress % 70 <= 50) strSuffix = ".";
            else if (nStatusMessageProgress % 70 <= 60) strSuffix = "..";
            else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
            return strprintf(_("Found enough users, signing ( waiting %s )").translated, strSuffix);
        case POOL_STATE_ERROR:
            return _("CoinJoin request incomplete:").translated + " " + strLastMessage + " " + _("Will retry...").translated;
        case POOL_STATE_SUCCESS:
            return _("CoinJoin request complete:").translated + " " + strLastMessage;
       default:
            return strprintf(_("Unknown state: id = %u").translated, nState);
    }
}

std::string CCoinJoinClientManager::GetStatuses()
{
    std::string strStatus;

    for (auto& session : deqSessions) {
        strStatus += session.GetStatus(WaitForAnotherBlock()) + "; ";
    }
    return strStatus;
}

std::string CCoinJoinClientManager::GetSessionDenoms()
{
    std::string strSessionDenoms;

    for (auto& session : deqSessions) {
        strSessionDenoms += (session.nSessionDenom ? CCoinJoin::GetDenominationsToString(session.nSessionDenom) : "N/A") + "; ";
    }
    return strSessionDenoms.empty() ? "N/A" : strSessionDenoms;
}

bool CCoinJoinClientSession::GetMixingMasternodeInfo(masternode_info_t& mnInfoRet) const
{
    mnInfoRet = infoMixingMasternode.fInfoValid ? infoMixingMasternode : masternode_info_t();
    return infoMixingMasternode.fInfoValid;
}

bool CCoinJoinClientManager::GetMixingMasternodesInfo(std::vector<masternode_info_t>& vecMnInfoRet) const
{
    LOCK(cs_deqsessions);
    for (const auto& session : deqSessions) {
        masternode_info_t mnInfo;
        if (session.GetMixingMasternodeInfo(mnInfo)) {
            vecMnInfoRet.push_back(mnInfo);
        }
    }
    return !vecMnInfoRet.empty();
}

//
// Check session timeouts
//
bool CCoinJoinClientSession::PoolStateManager()
{
    // catching hanging sessions
    switch(nState) {
    case POOL_STATE_IDLE:
    case POOL_STATE_CONNECTING:
    case POOL_STATE_QUEUE:
    case POOL_STATE_ACCEPTING_ENTRIES:
        return false;
    case POOL_STATE_SIGNING:
        if (GetTime() - nTimeStart > COINJOIN_SIGNING_TIMEOUT + 10) {
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CheckTimeout -- Signing timed out -- resetting\n", m_wallet_session->GetDisplayName());
            SetNull();
            SetState(POOL_STATE_IDLE);
            return true;
        }
        return false;
    case POOL_STATE_ERROR:
        LogPrintf("%s CCoinJoinClientSession::CheckTimeout -- Pool error -- resetting\n", m_wallet_session->GetDisplayName());
        SetNull();
        SetState(POOL_STATE_IDLE);
        return true;
    case POOL_STATE_SUCCESS:
        LogPrintf("%s CCoinJoinClientSession::CheckTimeout -- Pool success -- Reset and keep keys\n", m_wallet_session->GetDisplayName());
        keyHolderStorage.KeepAll();
        SetNull();
        return false;
    }
    return true;
}

//
// Check all queues and sessions for timeouts and result
//
void CCoinJoinClientManager::CheckResult(int nHeight)
{
    CheckQueue(nHeight);

    LOCK2(cs_deqsessions, cs_vecqueue);
    for (auto& session : deqSessions) {
        masternode_info_t mnMixing;
        bool found = false;
        for (const auto& q : vecCoinJoinQueue) {
            if (session.GetMixingMasternodeInfo(mnMixing) && mnMixing.outpoint == q.masternodeOutpoint && q.IsOpen()) {
                found = true;
                break;
            }
        }
        if (!found) session.SetError();
        if (session.PoolStateManager()) {
            strAutoCoinJoinResult = _("Session timed out.").translated;
        }
    }
    // let's see if we can free up some space by popping the first finished session
    while (!deqSessions.empty()) {
        if (deqSessions.front().GetState() == POOL_STATE_IDLE) {
            deqSessions.pop_front();
        } else break;
    }
    if (deqSessions.empty() && !fStartup) fActive = false;
}

//
// Execute a mixing denomination via a Masternode.
// This is only ran from clients
//
bool CCoinJoinClientSession::SendDenominate()
{
    // we should already be connected to a Masternode
    if (!nSessionID) {
        SetNull();
        LogPrintf("%s CCoinJoinClientSession::SendDenominate -- No Masternode has been selected yet.\n", m_wallet_session->GetDisplayName());
        return false;
    }

    if (!CheckDiskSpace(GetDataDir())) {
        SetNull();
        LogPrintf("%s CCoinJoinClientSession::SendDenominate -- Not enough disk space, disabling CoinJoin.\n", m_wallet_session->GetDisplayName());
        return false;
    }

    if (!infoMixingMasternode.fInfoValid)
        return false;

    SetState(POOL_STATE_ACCEPTING_ENTRIES);
    strLastMessage = "";

    // Remove all scriptSigs and scriptWitnesses from inputs
    for (CTxIn& input : mtxSession.vin) {
        input.scriptSig.clear();
        input.scriptWitness.SetNull();
    }

    // Make and fill our psbt
    PartiallySignedTransaction psbtx;
    psbtx.tx = mtxSession;
    for (unsigned int i = 0; i < mtxSession.vin.size(); ++i) {
        psbtx.inputs.push_back(PSBTInput());
    }
    for (unsigned int i = 0; i < mtxSession.vout.size(); ++i) {
        psbtx.outputs.push_back(PSBTOutput());
    }

    bool complete = false;
    const TransactionError err = FillPSBT(m_wallet_session, psbtx, complete, 1, false, false);
    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::SendDenominate -- FillPSBT completed: %b\n", m_wallet_session->GetDisplayName(), complete);

    if (err != TransactionError::OK) {
        LogPrintf("%s CCoinJoinClientManager::SendDenominate -- ERROR: creating transaction failed, psbtx=%s, error=%s\n",
                                  m_wallet_session->GetDisplayName(),
                                  psbtx.tx->GetHash().ToString(), TransactionErrorString(err));
        return false;
    }

    LogPrintf("%s CCoinJoinClientSession::SendDenominate -- Submitting psbt %s\n", m_wallet_session->GetDisplayName(), mtxSession.GetHash().ToString());

    // store our entry for later use
    CCoinJoinEntry entry(nSessionID, psbtx);
    if (!m_wallet_session->chain().pushCJEntry(infoMixingMasternode, NetMsgType::CJTXIN, entry)) {
        LogPrintf("%s CCoinJoinClientSession::SendDenominate -- Failed to submit psbt %s\n", m_wallet_session->GetDisplayName(), mtxSession.GetHash().ToString());
        return false;
    }
    return true;
}

// Incoming message from Masternode updating the progress of mixing
bool CCoinJoinClientSession::CheckPoolStateUpdate(PoolState nStateNew, int nEntriesCountNew, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID, int nSessionIDNew)
{
    // do not update state when mixing client state is one of these
    if (nState == POOL_STATE_IDLE || nState == POOL_STATE_ERROR || nState == POOL_STATE_SUCCESS) return false;

    strAutoCoinJoinResult = _("Masternode:").translated + " " + CCoinJoin::GetMessageByID(nMessageID);

    // if rejected at any state
    if (nStatusUpdate == STATUS_REJECTED) {
        LogPrintf("%s CCoinJoinClientSession::CheckPoolStateUpdate -- entry is rejected by Masternode\n", m_wallet_session->GetDisplayName());
        SetNull();
        SetState(POOL_STATE_ERROR);
        strLastMessage = CCoinJoin::GetMessageByID(nMessageID);
        return true;
    }

    if (nStatusUpdate == STATUS_ACCEPTED && nState == nStateNew) {
        if (nStateNew == POOL_STATE_QUEUE && nSessionID == 0 && nSessionIDNew != 0) {
            // new session id should be set only in POOL_STATE_QUEUE state
            nSessionID = nSessionIDNew;
            LogPrintf("%s CCoinJoinClientSession::CheckPoolStateUpdate -- set nSessionID to %d\n", m_wallet_session->GetDisplayName(), nSessionID);
            return true;
        }
        else if (nStateNew == POOL_STATE_ACCEPTING_ENTRIES && nEntriesCount != nEntriesCountNew) {
            nEntriesCount = nEntriesCountNew;
            fLastEntryAccepted = true;
            LogPrintf("%s CCoinJoinClientSession::CheckPoolStateUpdate -- new entry accepted!\n", m_wallet_session->GetDisplayName());
            return true;
        }
    }

    // only situations above are allowed, fail in any other case
    return false;
}

//
// After we receive the finalized transaction from the Masternode, we must
// check it to make sure it's what we want, then sign it if we agree.
//
bool CCoinJoinClientSession::SignFinalTransaction(PartiallySignedTransaction& finalTransactionNew, CNode* pnode)
{
    if (pnode == nullptr || !m_wallet_session) return false;

    LogPrintf("%s CCoinJoinClientSession::SignFinalTransaction -- finalTransactionNew=%s\n", m_wallet_session->GetDisplayName(), finalTransactionNew.tx->GetHash().ToString());

    CMutableTransaction mtx(*finalTransactionNew.tx);

    //make sure my outputs are present, otherwise refuse to sign
    //don't care about the inputs
    for (const auto& txout1 : mtx.vout) {
        bool found = false;
        for (const auto& txout2 : mtx.vout) {
            if (txout1 == txout2) found = true;
        }
        if (!found) {
            LogPrintf("%s CCoinJoinClientSession::SignFinalTransaction -- received transaction does not contain session outputs!\n", m_wallet_session->GetDisplayName());
            SetState(POOL_STATE_ERROR);
            return false;
        }
    }

    CAmount nFee = 0;
    PoolMessage nMessageID = MSG_NOERR;
    bool complete = false;
    bool fUnsigned = true;

    if (!CheckTransaction(finalTransactionNew, nFee, nMessageID, fUnsigned)) {
        LogPrintf("%s CCoinJoinClientSession::SignFinalTransaction -- CheckTransaction failed!\n", m_wallet_session->GetDisplayName());
        SetState(POOL_STATE_ERROR);
        return false;
    }

    const TransactionError err = FillPSBT(m_wallet_session, finalTransactionNew, complete);

    if (err != TransactionError::OK) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::SignFinalTransaction -- ERROR: finalTransactionNew=%s, error=%s\n",
                 m_wallet_session->GetDisplayName(),
                 finalTransactionNew.tx->GetHash().ToString(), TransactionErrorString(err));
        SetState(POOL_STATE_ERROR);
        return false;
    }

    // push all of our signatures to the Masternode
    LogPrintf("%s CCoinJoinClientSession::SignFinalTransaction -- pushing sigs to the masternode, finalMutableTransaction=%s\n", m_wallet_session->GetDisplayName(), mtx.GetHash().ToString());
    if (!m_wallet_session->chain().pushPSBT(pnode, NetMsgType::CJSIGNFINALTX, finalTransactionNew)) {
        LogPrintf("%s CCoinJoinClientSession::SignFinalTransaction -- failed to push sigs to the masternode, masternode=%s\n", m_wallet_session->GetDisplayName(), pnode->addr.ToString());
        return false;
    }
    SetState(POOL_STATE_SIGNING);
    nTimeStart = GetTime();

    return true;
}

// mixing transaction was completed (failed or successful)
void CCoinJoinClientSession::CompletedTransaction(PoolMessage nMessageID)
{
    if (nMessageID == MSG_SUCCESS) {
        LogPrintf("%s CompletedTransaction -- success\n", m_wallet_session->GetDisplayName());
        m_wallet_session->coinjoinClient->UpdatedSuccessBlock();
    } else {
        LogPrintf("%s CompletedTransaction -- error\n", m_wallet_session->GetDisplayName());
    }
    keyHolderStorage.KeepAll();
    UnlockCoins();
    SetNull();
    strLastMessage = CCoinJoin::GetMessageByID(nMessageID);
}

void CCoinJoinClientManager::UpdatedSuccessBlock()
{
    if (fMasternodeMode) return;
    nCachedLastSuccessBlock = nCachedBlockHeight;
}

// check if we should initiate a mixing process and if so, pass some flags to determine the priorities later
bool CCoinJoinClientManager::IsMixingRequired(std::vector<std::pair<CTxIn, CTxOut> >& portfolio, std::vector<CAmount>& vecAmounts, bool& fMixOnly)
{
    // first check for portfolio denoms unless we are alredy in mix only mode
    CAmount nTotal = std::accumulate(vecAmounts.begin(), vecAmounts.end(), CAmount(0));
    std::sort(vecAmounts.begin(), vecAmounts.end());

    std::vector<std::pair<CTxIn, CTxOut> > temp(portfolio);
    int depth = nLiquidityProvider ? MAX_COINJOIN_DEPTH + 1 : nCoinJoinDepth;

    if (!fMixOnly) {
        for (auto denom = COINJOIN_LOW_DENOM; denom <= COINJOIN_HIGH_DENOM; denom <<= 1) {
            int64_t count = 0;
            std::vector<std::pair<CTxIn, CTxOut> > unlock;
            auto threshold = denom == COINJOIN_LOW_DENOM ? COINJOIN_FEE_DENOM_THRESHOLD : COINJOIN_DENOM_THRESHOLD;
            auto tempAmounts(vecAmounts);
            for (const auto& amount : vecAmounts) {
                if (amount > denom) break;
                if (amount < denom) continue;
                if (amount == denom) {
                    count++;
                    nTotal -= denom;
                    // remove up to the finished denom
                    for (std::vector<std::pair<CTxIn, CTxOut> >::iterator it = temp.begin(); it != temp.end(); it++) {
                        if (it->second.nValue > denom) break;
                        if (it->second.nValue == denom) {
                            unlock.push_back(*it);
                            temp.erase(it);
                            tempAmounts.push_back(amount);
                            break;
                        }
                    }
                    if (count > threshold * COINJOIN_DENOM_WINDOW) {
                        for (const auto& out : unlock) {
                            LOCK(m_wallet->cs_wallet);
                            m_wallet->UnlockCoin(out.first.prevout);
                        }
                        portfolio = temp;
                        vecAmounts = tempAmounts;
                        return true;
                    }
                }
            }
            if (count < threshold && nTotal > 0) {
                for (const auto& out : unlock) {
                    LOCK(m_wallet->cs_wallet);
                    m_wallet->UnlockCoin(out.first.prevout);
                }
                portfolio = temp;
                vecAmounts = tempAmounts;
                return true;
            }
        }
    }

    // all in bounds so check if there's something to obscure
    for (std::vector<std::pair<CTxIn, CTxOut> >::iterator it = portfolio.begin(); it != portfolio.end(); it++) {
        if (it->second.nDepth < depth) {
            fMixOnly = true;
        } else {
            LOCK(m_wallet->cs_wallet);
            m_wallet->UnlockCoin(it->first.prevout);
            portfolio.erase(it--);
        }
    }

    if (fMixOnly && !nLiquidityProvider) return true;

    // nothing to do
    if (!nLiquidityProvider) return false;

    // Liquidity providers: don't use the full portfolio, remove randomly
    for (auto denom = COINJOIN_LOW_DENOM; denom <= COINJOIN_HIGH_DENOM; denom <<= 1) {
        int64_t count = 0;
        int threshold = (1 + GetRandInt(COINJOIN_DENOM_THRESHOLD));
        for (std::vector<std::pair<CTxIn, CTxOut> >::iterator it = portfolio.begin(); it != portfolio.end(); it++) {
            if (it->second.nValue < denom) continue;
            if (it->second.nValue > denom) break;
            if (it->second.nValue == denom) {
                count++;
                if (count > threshold && it->second.nDepth >= MAX_COINJOIN_DEPTH) {
                    LOCK(m_wallet->cs_wallet);
                    m_wallet->UnlockCoin(it->first.prevout);
                    portfolio.erase(it--);
                }
            }
        }
    }
    return true;
}

bool CCoinJoinClientManager::WaitForAnotherBlock()
{
    if (!masternodeSync.IsMasternodeListSynced())
        return true;

    return nCachedBlockHeight - nCachedLastSuccessBlock < nMinBlocksToWait;
}

bool CCoinJoinClientManager::CheckAutomaticBackup()
{
    if (!m_wallet) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Wallet is not initialized, no mixing available.\n", m_wallet->GetDisplayName());
        strAutoCoinJoinResult = _("Wallet is not initialized").translated + ", " + _("no mixing available.").translated;
        fEnableCoinJoin = false; // no mixing
        return false;
    }

    switch(nWalletBackups) {
        case 0:
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Automatic backups disabled, no mixing available.\n", m_wallet->GetDisplayName());
            strAutoCoinJoinResult = _("Automatic backups disabled").translated + ", " + _("no mixing available.").translated;
            fEnableCoinJoin = false; // stop mixing
            m_wallet->nKeysLeftSinceAutoBackup = 0; // no backup, no "keys since last backup"
            return false;
        case -1:
            // Automatic backup failed, nothing else we can do until user fixes the issue manually.
            // There is no way to bring user attention in daemon mode so we just update status and
            // keep spamming if debug is on.
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- ERROR! Failed to create automatic backup.\n", m_wallet->GetDisplayName());
            strAutoCoinJoinResult = _("ERROR! Failed to create automatic backup").translated + ", " + _("see debug.log for details.").translated;
            return false;
        case -2:
            // We were able to create automatic backup but keypool was not replenished because wallet is locked.
            // There is no way to bring user attention in daemon mode so we just update status and
            // keep spamming if debug is on.
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- WARNING! Failed to create replenish keypool, please unlock your wallet to do so.\n", m_wallet->GetDisplayName());
            strAutoCoinJoinResult = _("WARNING! Failed to replenish keypool, please unlock your wallet to do so.").translated + ", " + _("see debug.log for details.").translated;
            return false;
    }

    if (m_wallet->nKeysLeftSinceAutoBackup < COINJOIN_KEYS_THRESHOLD_STOP) {
        // We should never get here via mixing itself but probably smth else is still actively using keypool
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Very low number of keys left: %d, no mixing available.\n", m_wallet->GetDisplayName(), m_wallet->nKeysLeftSinceAutoBackup);
        strAutoCoinJoinResult = strprintf(_("Very low number of keys left: %d").translated + ", " + _("no mixing available.").translated, m_wallet->nKeysLeftSinceAutoBackup);
        // It's getting really dangerous, stop mixing
        fEnableCoinJoin = false;
        return false;
    } else if (m_wallet->nKeysLeftSinceAutoBackup < COINJOIN_KEYS_THRESHOLD_WARNING) {
        // Low number of keys left but it's still more or less safe to continue
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Very low number of keys left: %d\n", m_wallet->GetDisplayName(), m_wallet->nKeysLeftSinceAutoBackup);
        strAutoCoinJoinResult = strprintf(_("Very low number of keys left: %d").translated, m_wallet->nKeysLeftSinceAutoBackup);

        if (fCreateAutoBackups) {
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Trying to create new backup.\n", m_wallet->GetDisplayName());
            std::vector<std::string> warnings;
            std::string errorString;
            std::shared_ptr<CWallet> const pwallet = GetWallet(m_wallet->GetName());

            if (!AutoBackupWallet(pwallet, WalletLocation(), warnings, errorString)) {
                if (!warnings.empty()) {
                    // There were some issues saving backup but yet more or less safe to continue
                    LogPrintf("%s CCoinJoinClientManager::CheckAutomaticBackup -- WARNING! Something went wrong on automatic backup: %s\n", m_wallet->GetDisplayName(), Join(warnings, "\n"));
                }
                if (!errorString.empty()) {
                    // Things are really broken
                    LogPrintf("%s CCoinJoinClientManager::CheckAutomaticBackup -- ERROR! Failed to create automatic backup: %s\n", m_wallet->GetDisplayName(), errorString);
                    strAutoCoinJoinResult = strprintf(_("ERROR! Failed to create automatic backup").translated + ": %s", errorString);
                    return false;
                }
            }
        } else {
            // Wait for smth else (e.g. GUI action) to create automatic backup for us
            return false;
        }
    }

    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CheckAutomaticBackup -- Keys left since latest backup: %d\n", m_wallet->GetDisplayName(), m_wallet->nKeysLeftSinceAutoBackup);

    return true;
}

bool CCoinJoinClientSession::CreateSessionTransaction(std::vector<std::pair<CTxIn, CTxOut> >& vecPair, CAmount& nDenom, std::vector<CAmount>& vecAmounts)
{
    nDenom = 0;
    strAutoCoinJoinResult = _("Creating transaction").translated;

    CAmount nValueRem = 0;

    for (std::vector<std::pair<CTxIn, CTxOut> >::iterator it = vecPair.begin(); it != vecPair.end(); it++) {
        if (mtxSession.vin.size() >= COINJOIN_ENTRY_MAX_SIZE - std::max(GetRandInt(COINJOIN_ENTRY_MAX_SIZE / 3), 10))  break;

        vecOutPointLocked.emplace_back(it->first.prevout);
        mtxSession.vin.emplace_back(it->first);
        nValueRem += it->second.nValue;
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CreateSessionTransaction --- step 1: added input: %s, nValue: %s\n",
                 m_wallet_session->GetDisplayName(),
                 it->first.ToString(),
                 FormatMoney(it->second.nValue));
        if (fMixingOnly) {
            nValueRem -= it->second.nValue;
            CScript scriptDenom;
            keyHolderStorage.AddKey(scriptDenom, m_wallet_session);
            mtxSession.vout.emplace_back(CTxOut(it->second.nValue, scriptDenom));
            vecAmounts.emplace_back(it->second.nValue);
            nSessionDenom |= it->second.nValue;
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CreateSessionTransaction --- step 1a: added output: %s, remaining: %s\n",
                     m_wallet_session->GetDisplayName(),
                     FormatMoney(it->second.nValue),
                     FormatMoney(nValueRem));
        }
        vecPair.erase(it--);
    }

    if (!fMixingOnly) {
        std::sort(vecAmounts.begin(), vecAmounts.end());

        // fill missing denoms, small to large
        for (auto denom = COINJOIN_LOW_DENOM; denom <= COINJOIN_HIGH_DENOM; denom <<= 1) {
            if (nValueRem < denom) break;
            auto count = 0;
            auto threshold = denom == COINJOIN_LOW_DENOM ? COINJOIN_FEE_DENOM_THRESHOLD : COINJOIN_DENOM_THRESHOLD;
            auto target = threshold * COINJOIN_DENOM_WINDOW - GetRandInt(threshold);
            for (const auto& value : vecAmounts) {
                if (nValueRem < value || count >= static_cast<int>(target)) break;
                if (value < denom) continue;
                if (value == denom) count++;
                else break;
            }
            while (count < static_cast<int>(target) && nValueRem >= denom) {
                count++;
                nValueRem -= denom;
                CScript scriptDenom;
                keyHolderStorage.AddKey(scriptDenom, m_wallet_session);
                mtxSession.vout.emplace_back(CTxOut(denom, scriptDenom));
                vecAmounts.emplace_back(denom);
                nSessionDenom |= denom;
                LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CreateSessionTransaction --- step 2: added output: %s, remaining: %s\n",
                         m_wallet_session->GetDisplayName(),
                         FormatMoney(denom),
                         FormatMoney(nValueRem));
            }
        }

        // add the remainder
        for (auto denom = COINJOIN_HIGH_DENOM; denom >= COINJOIN_LOW_DENOM; denom >>=1) {
            while (nValueRem >= denom) {
                nValueRem -= denom;
                CScript scriptDenom;
                keyHolderStorage.AddKey(scriptDenom, m_wallet_session);
                mtxSession.vout.emplace_back(CTxOut(denom, scriptDenom));
                vecAmounts.emplace_back(denom);
                nSessionDenom |= denom;
                LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CreateSessionTransaction --- step 3: added output: %s, remaining: %s\n",
                         m_wallet_session->GetDisplayName(),
                         FormatMoney(denom),
                         FormatMoney(nValueRem));
            }
        }
    }

    // make sure everything went right
    assert(nValueRem == 0);

    if (AddFeesAndLocktime(vecAmounts))
        return true;

    LogPrintf("%s CCoinJoinClientSession::CreateSessionTransaction -- ERROR: no inputs found for given request.\n", m_wallet_session->GetDisplayName());
    return false;
}

/**
 * Return a height-based locktime for new transactions (uses the height of the
 * current chain tip unless we are not synced with the current chain
 */
static uint32_t GetLocktimeForCoinJoin(interfaces::Chain& chain, interfaces::Chain::Lock& locked_chain)
{
    uint32_t const height = locked_chain.getHeight().get_value_or(-1);
    uint32_t locktime;
    // Discourage fee sniping, see wallet.cpp
    // used in this context to determine the masternode which should
    // receive the fees for CoinJoin! at max 8 blocks in the future
    if (chain.isInitialBlockDownload()) {
        locktime = (int)height + GetRandInt(8);
    } else {
        locktime = 0;
    }
    assert(locktime >= height);
    assert(locktime < LOCKTIME_THRESHOLD);
    return locktime;
}

bool CCoinJoinClientSession::AddFeesAndLocktime(std::vector<CAmount>& vecAmounts)
{
    uint32_t locktime = 0;
    CScript payee = CScript();
    {
        auto locked_chain = m_wallet_session->chain().lock();
        locktime = GetLocktimeForCoinJoin(m_wallet_session->chain(), *locked_chain);
    }
    if (locktime == 0) {
        LogPrintf("CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: failed to find nLocktime!\n");
        return false;
    }

    for (auto i = 1; i < 200; ++i) { // allow some blocks back -- many blocks until MNs are using bech32
        if (mnpayments.GetBlockPayee(locktime, payee)) {
            if (!payee.IsPayToWitnessScriptHash()) continue;
            CTxDestination address;
            ExtractDestination(payee, address);
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- added payee = %s\n", m_wallet_session->GetDisplayName(), EncodeDestination(address));
            break;
        }
        else locktime -= i;
    }

    if (payee == CScript()) {
        LogPrintf("%s CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: failed to find masternode to pay!\n", m_wallet_session->GetDisplayName());
        return false;
    }
    mtxSession.nLockTime = locktime;
    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- added nLockTime = %u\n", m_wallet_session->GetDisplayName(), locktime);

    // Get long term estimate
    FeeCalculation feeCalc;
    CCoinControl coin_control;
    int nBytes;
    CAmount nFeeNeeded = 0;
    CAmount nFeeRet = 0;
    std::vector<std::pair<CTxIn, CTxOut> > tmp_select;
    bool selected = false;
    std::sort(vecAmounts.begin(), vecAmounts.end());

    while (true)
    {
        // see if there are enough fees
        {
            LOCK(m_wallet_session->cs_wallet);
            nBytes = CalculateMaximumSignedTxSize(CTransaction(mtxSession), m_wallet_session, coin_control.fAllowWatchOnly);
        }
        if (nBytes < 0) {
            LogPrintf("CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: Dummysigning transaction failed!\n");
            return false;
        }
        nFeeNeeded = GetMinimumFee(*m_wallet_session, nBytes, coin_control, &feeCalc);
        if (feeCalc.reason == FeeReason::FALLBACK && !m_wallet_session->m_allow_fallback_fee) {
            // eventually allow a fallback fee
            LogPrintf("%s CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: Fee estimation failed. Fallbackfee is disabled. Wait a few blocks or enable -fallbackfee.\n", m_wallet_session->GetDisplayName());
            return false;
        }
        // If we made it here and we aren't even able to meet the relay fee on the next pass, give up
        // because we must be at the maximum allowed fee.
        if (nFeeNeeded < ::minRelayTxFee.GetFee(nBytes))
        {
            LogPrintf("%s CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: Transaction too large for fee policy!\n", m_wallet_session->GetDisplayName());
            return false;
        }

        if (nFeeNeeded <= 2 * nFeeRet) break;

        CMutableTransaction mtxTmp (mtxSession);
        std::vector<COutPoint> outTmp (vecOutPointLocked);
        CAmount feeRetTmp (nFeeRet);

        // not enough selected? try to add additional inputs
        int n = nFeeNeeded % COINJOIN_LOW_DENOM  == 0 ? nFeeNeeded / COINJOIN_LOW_DENOM : nFeeNeeded / COINJOIN_LOW_DENOM + 1;
        selected = m_wallet_session->SelectJoinCoins(n * 2 * COINJOIN_LOW_DENOM, n * 2 * COINJOIN_LOW_DENOM, tmp_select, 1);
        for (const auto& out : tmp_select) {
            if (out.second.nValue != COINJOIN_LOW_DENOM) {
                LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- no inputs available for fees, trying to reduce outputs.\n", m_wallet_session->GetDisplayName());
                selected = false;
            }
        }
        if (selected) {
            int inCount = 0;
            for (std::vector<std::pair<CTxIn, CTxOut> >::iterator it = tmp_select.begin(); it != tmp_select.end(); it++) {
                LOCK(m_wallet_session->cs_wallet);
                outTmp.emplace_back(it->first.prevout);
                mtxTmp.vin.emplace_back(it->first);
                m_wallet_session->LockCoin(it->first.prevout);
                m_wallet_session->coinjoinClient->vecOutPointLocked.push_back(it->first.prevout);
                feeRetTmp += it->second.nValue;
                inCount++;
                LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- added existing input: %s for fees\n",
                         m_wallet_session->GetDisplayName(),
                         it->first.ToString());
                if (inCount % 2 == 0) {
                    mtxTmp.vout.emplace_back(CTxOut(COINJOIN_LOW_DENOM, payee));
                    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- added output: %s for masternode payment\n",
                             m_wallet_session->GetDisplayName(),
                             FormatMoney(it->second.nValue));
                }
                if (feeRetTmp >= 2 * nFeeNeeded && inCount % 2 == 0) {
                    mtxSession = mtxTmp;
                    vecOutPointLocked = outTmp;
                    nFeeRet = feeRetTmp;
                    break;
                }
            }
            continue;
        }

        // not enough inputs left, try to remove some outputs
        int count = 0;
        for (std::vector<CTxOut>::iterator it = mtxSession.vout.begin(); it != mtxSession.vout.end(); ++it) {
            if (it->nValue == COINJOIN_LOW_DENOM && it->scriptPubKey != payee) {
                count++;
                nFeeRet += it->nValue;
                if (vecAmounts.front() == it->nValue) vecAmounts.erase(vecAmounts.begin());
                if (count % 2 == 0) {
                    it->scriptPubKey = payee;
                    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- changed output: %s for masternode payment\n",
                             m_wallet_session->GetDisplayName(),
                             FormatMoney(it->nValue));
                } else {
                    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::AddFeesAndLocktime --- changed output: %s for fees\n",
                             m_wallet_session->GetDisplayName(),
                             FormatMoney(it->nValue));
                    mtxSession.vout.erase(it--);
                }
                if (nFeeRet >= 2 * nFeeNeeded && count % 2 == 0) break;
            }
        }
        if (nFeeRet >= 2 * nFeeNeeded && count % 2 == 0) {
            continue;
        } else {
            LogPrintf("%s CCoinJoinClientSession::AddFeesAndLocktime --- ERROR: unable to apply fees!\n", m_wallet_session->GetDisplayName());
            return false;
        }
    }

    Shuffle(mtxSession.vin.begin(), mtxSession.vin.end(), FastRandomContext());
    Shuffle(mtxSession.vout.begin(), mtxSession.vout.end(), FastRandomContext());
    LogPrintf("%s CCoinJoinClientSession::AddFeesAndLocktime --- Created transaction: %s\n", m_wallet_session->GetDisplayName(), mtxSession.GetHash().ToString());
    return true;
}

//
// Passively run mixing in the background to anonymize funds based on the given configuration.
//
void CCoinJoinClientSession::CoinJoin(std::vector<std::pair<CTxIn, CTxOut> >& vecPair, std::vector<CAmount>& vecAmounts)
{
    if (GetState() != POOL_STATE_IDLE) return;

    if (!m_wallet_session) {
        strAutoCoinJoinResult = _("Wallet is not loaded!").translated;
        SetState(POOL_STATE_ERROR);
        return;
    }

    if (m_wallet_session->IsLocked(true)) {
        strAutoCoinJoinResult = _("Wallet is locked, please unlock first!").translated;
        SetState(POOL_STATE_ERROR);
        return;
    }

    TRY_LOCK(cs_coinjoin, lockDS);
    if (!lockDS) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::CoinJoin -- mutex locked, returning\n", m_wallet_session->GetDisplayName());
        strAutoCoinJoinResult = _("Client busy...").translated;
        SetState(POOL_STATE_ERROR);
        return;
    }

    // Initial phase, find a Masternode
    // Clean if there is anything left from previous session
    SetNull();

    // Attemt to create our transaction
    if (!CreateSessionTransaction(vecPair, nSessionDenom, vecAmounts)) {
        strAutoCoinJoinResult = _("Failed to create Transaction!").translated;
        SetNull();
        SetState(POOL_STATE_IDLE);
        return;
    }

    // don't use the queues all of the time for mixing unless we are a liquidity provider
    if ((m_wallet_session->coinjoinClient->nLiquidityProvider || GetRandInt(100) > 33) && JoinExistingQueue()) return;

    // do not initiate queue if we are a liquidity provider to avoid useless inter-mixing
    if (m_wallet_session->coinjoinClient->nLiquidityProvider) {
        strAutoCoinJoinResult = _("Liquidity Provider: Idle...").translated;
        SetNull();
        SetState(POOL_STATE_IDLE);
        return;
    }

    // initiate new queue, result is updated in sub-routine
    if (!StartNewQueue()) {
        SetState(POOL_STATE_ERROR);
        return;
    }
}

void CCoinJoinClientManager::CoinJoin()
{
    if (fActive) return;
    fActive = true;
    fStartup = true;

    if (!masternodeSync.IsMasternodeListSynced()) {
        strAutoCoinJoinResult = _("Waiting for sync to finish...").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    if (!m_wallet) {
        strAutoCoinJoinResult = _("Wallet is not initialized.").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    if (m_wallet->IsLocked(true)) {
        strAutoCoinJoinResult = _("Wallet is locked, will retry...").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    if (!CheckAutomaticBackup()) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CoinJoin -- Failed to create automatic backup\n", m_wallet->GetDisplayName());
        strAutoCoinJoinResult = _("Failed to create automatic backup.").translated;
        fEnableCoinJoin = false;
        fActive = false;
        fStartup = false;
        return;
    }

    // Check if we have should create more denominated inputs i.e.
    // there are funds to denominate and denominated balance does not exceed
    // max amount to mix yet.

    std::vector<CAmount> vecAmounts;

    // denoms
    CAmount nBalanceDenominated = m_wallet->GetLegacyDenomBalance(vecAmounts);
    // excluding denoms
    CAmount nBalanceAnonimizableNonDenom = m_wallet->GetLegacyBalance(ISMINE_SPENDABLE, 0) - nBalanceDenominated;
    // amout to denominate
    CAmount nDenomTarget = nCoinJoinAmount * COIN + COINJOIN_LOW_DENOM * COINJOIN_FEE_DENOM_THRESHOLD * COINJOIN_DENOM_WINDOW;
    CAmount nDifference = nDenomTarget - nBalanceDenominated > 0 ? nDenomTarget - nBalanceDenominated : 0;
    CAmount nBalanceNeedsDenom = std::min(nDifference, nBalanceAnonimizableNonDenom);

    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CoinJoin -- nValueMin: %f, nBalanceNeedsDenom: %f, nBalanceAnonimizableNonDenom: %f, nBalanceDenominated: %f\n",
             m_wallet->GetDisplayName(),
             (float)COINJOIN_LOW_DENOM / COIN,
             (float)nBalanceNeedsDenom / COIN,
             (float)nBalanceAnonimizableNonDenom / COIN,
             (float)nBalanceDenominated / COIN);

    // anonymizable balance is way too small
    if (nBalanceDenominated + nBalanceNeedsDenom < COINJOIN_LOW_DENOM * COINJOIN_FEE_DENOM_THRESHOLD) {
        LogPrintf("%s CCoinJoinClientManager::CoinJoin -- Not enough funds to anonymize: %s available\n", m_wallet->GetDisplayName(), FormatMoney(nBalanceDenominated + nBalanceNeedsDenom));
        strAutoCoinJoinResult = _("Not enough funds to anonymize, will retry...").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    if (nBalanceNeedsDenom >= COINJOIN_LOW_DENOM * COINJOIN_FEE_DENOM_THRESHOLD) {
        strAutoCoinJoinResult = _("Creating denominated outputs.").translated;
        if (!CreateDenominated(nBalanceNeedsDenom, vecAmounts)) {
            strAutoCoinJoinResult = _("Failed to create denominated outputs.").translated;
        }
    }

    //if we are am LP and there aren't any queues active, we're done
    if (nLiquidityProvider && !GetQueueSize()) {
        fActive = false;
        fStartup = false;
        return;
    }

    // anything there to work on?
    if (nBalanceDenominated <= COINJOIN_FEE_DENOM_THRESHOLD * COINJOIN_LOW_DENOM) {
        strAutoCoinJoinResult = _("Low balance (denominated).").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    int nMnCountEnabled = mnodeman.CountEnabled(MIN_COINJOIN_PEER_PROTO_VERSION);

    if (nMnCountEnabled == 0) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CoinJoin -- No Masternodes detected\n", m_wallet->GetDisplayName());
        strAutoCoinJoinResult = _("No Masternodes detected, will retry...").translated;
        fActive = false;
        fStartup = false;
        return;
    }

    // If we've used 90% of the Masternode list then drop the oldest first ~30%
    int nThreshold_high = nMnCountEnabled / 10 * 9;
    int nThreshold_low = nThreshold_high / 10 * 7;
    LogPrint(BCLog::CJOIN, "%s Checking vecMasternodesUsed: size: %d, threshold: %d\n", m_wallet->GetDisplayName(), (int)vecMasternodesUsed.size(), nThreshold_high);

    if ((int)vecMasternodesUsed.size() > nThreshold_high) {
        vecMasternodesUsed.erase(vecMasternodesUsed.begin(), vecMasternodesUsed.begin() + vecMasternodesUsed.size() - nThreshold_low);
        LogPrint(BCLog::CJOIN, "%s   vecMasternodesUsed: new size: %d, threshold: %d\n", m_wallet->GetDisplayName(), (int)vecMasternodesUsed.size(), nThreshold_high);
    }


    std::vector<std::pair<CTxIn, CTxOut> > portfolio;

    // lock the coins we are going to use early
    if (!m_wallet->SelectJoinCoins(COINJOIN_LOW_DENOM * COINJOIN_FEE_DENOM_THRESHOLD, nBalanceDenominated, portfolio, 1)) {
        LogPrintf("%s CCoinJoinClientManager::CoinJoin -- Can't mix: no compatible inputs found, retry at the next block!\n", m_wallet->GetDisplayName());
        fActive = false;
        fStartup = false;
        return;
    }

     bool fMixOnly = false;

    if (IsMixingRequired(portfolio, vecAmounts, fMixOnly)) {
        for (const auto& txin : portfolio) {
            LOCK(m_wallet->cs_wallet);
            m_wallet->LockCoin(txin.first.prevout);
            vecOutPointLocked.push_back(txin.first.prevout);
        }
    } else {
        fActive = false;
        fStartup = false;
        return; //nothing to do
    }

    LOCK(cs_deqsessions);
    while (portfolio.size() > 2 && (int)deqSessions.size() < MAX_COINJOIN_SESSIONS) {
        deqSessions.emplace_back(m_wallet, fMixOnly);
        deqSessions.back().CoinJoin(portfolio, vecAmounts);
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CoinJoin -- Added session, deqSessions.size: %d, queue size: %d\n", m_wallet->GetDisplayName(), deqSessions.size(), GetQueueSize());
        // session creation successful? if not remove and exit
        if (deqSessions.back().GetState() == POOL_STATE_IDLE) deqSessions.pop_back();
        if (!IsMixingRequired(portfolio, vecAmounts, fMixOnly)) break;
    }
    // unlock unused coins
    for (const auto& txin : portfolio) {
        LOCK(m_wallet->cs_wallet);
        m_wallet->UnlockCoin(txin.first.prevout);
    }

    fStartup = false;
    if (deqSessions.empty()) fActive = false;

    // LPs can drop out here to be available for the next user
    if (nLiquidityProvider && fMixOnly) fActive = false;
}

void CCoinJoinClientManager::AddUsedMasternode(const COutPoint& outpointMn)
{
    vecMasternodesUsed.push_back(outpointMn);
}

masternode_info_t CCoinJoinClientManager::GetNotUsedMasternode()
{
    std::vector<COutPoint> vecToExclude(vecMasternodesUsed);

    {
        LOCK(cs_vecqueue);
        for (const auto& q : vecCoinJoinQueue) {
            vecToExclude.emplace_back(q.masternodeOutpoint);
        }
    }

    return mnodeman.FindRandomNotInVec(vecToExclude, MIN_COINJOIN_PEER_PROTO_VERSION);
}

bool CCoinJoinClientSession::JoinExistingQueue()
{
    if (!m_wallet_session) return false;

    // Look through the queues and see if anything matches
    CCoinJoinQueue queue;
    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::JoinExistingQueue -- looking for queue.\n", m_wallet_session->GetDisplayName());

    while (m_wallet_session->coinjoinClient->GetQueueItem(queue)) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::JoinExistingQueue -- found queue: %s\n", m_wallet_session->GetDisplayName(), queue.ToString());

        masternode_info_t infoMn;

        if (!mnodeman.GetMasternodeInfo(queue.masternodeOutpoint, infoMn)) {
            LogPrintf("%s CCoinJoinClientSession::JoinExistingQueue -- queue masternode is not in masternode list, masternode=%s\n", m_wallet_session->GetDisplayName(), queue.masternodeOutpoint.ToStringShort());
            continue;
        }

        if (infoMn.nProtocolVersion < MIN_COINJOIN_PEER_PROTO_VERSION) continue;

        if (!CCoinJoin::IsInDenomRange(queue.nDenom)) {
            // incompatible denom
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::JoinExistingQueue -- found invalid queue: %s\n", m_wallet_session->GetDisplayName(), queue.ToString());
            continue;
        }

        if ((nSessionDenom ^ queue.nDenom) == (nSessionDenom | queue.nDenom)) {
            // no matching denom bit
            LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::JoinExistingQueue -- queue doesn't match denom: %s\n", m_wallet_session->GetDisplayName(), queue.ToString());
            continue;
        }

        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::JoinExistingQueue -- found valid queue: %s\n", m_wallet_session->GetDisplayName(), queue.ToString());

        m_wallet_session->coinjoinClient->AddUsedMasternode(queue.masternodeOutpoint);

        if (!m_wallet_session->chain().addMasternode(infoMn)) {
            LogPrintf("%s CCoinJoinClientSession::JoinExistingQueue -- skipping connection, addr=%s\n", m_wallet_session->GetDisplayName(), infoMn.addr.ToString());
            continue;
        }

        SetState(POOL_STATE_CONNECTING);
        infoMixingMasternode = infoMn;
        pendingCJaRequest = CPendingCJaRequest(infoMn.addr, nSessionDenom);

        LogPrintf("%s CCoinJoinClientSession::JoinExistingQueue -- pending connection (from queue): nSessionDenom: %d (%s), addr=%s\n",
                m_wallet_session->GetDisplayName(),
                nSessionDenom, CCoinJoin::GetDenominationsToString(nSessionDenom), infoMn.addr.ToString());
        strAutoCoinJoinResult = _("Trying to connect...").translated;
        return true;
    }
    strAutoCoinJoinResult = _("Failed to find mixing queue to join, will retry...").translated;
    return false;
}

bool CCoinJoinClientSession::StartNewQueue()
{
    if (!m_wallet_session) return false;

    int nTries = 0;

    // connect to masternode
    while(nTries < 30) {
        masternode_info_t infoMn = m_wallet_session->coinjoinClient->GetNotUsedMasternode();

        if (!infoMn.fInfoValid) {
            LogPrintf("%s CCoinJoinClientSession::StartNewQueue -- Can't find random masternode!\n", m_wallet_session->GetDisplayName());
            strAutoCoinJoinResult = _("Can't find random Masternode, will retry...").translated;
            return false;
        }

        m_wallet_session->coinjoinClient->AddUsedMasternode(infoMn.outpoint);

        // skip next mn payments winners
        if (mnpayments.IsScheduled(infoMn, 0)) {
            LogPrintf("%s CCoinJoinClientSession::StartNewQueue -- skipping winner, masternode=%s\n", m_wallet_session->GetDisplayName(), infoMn.outpoint.ToStringShort());
            nTries++;
            continue;
        }

        LogPrintf("%s CCoinJoinClientSession::StartNewQueue -- attempt %d connection to Masternode %s\n", m_wallet_session->GetDisplayName(), nTries, infoMn.addr.ToString());

        if (!m_wallet_session->chain().addMasternode(infoMn)) {
            LogPrintf("%s CCoinJoinClientSession::StartNewQueue -- skipping connection, addr=%s\n", m_wallet_session->GetDisplayName(), infoMn.addr.ToString());
            continue;
        }

        SetState(POOL_STATE_CONNECTING);
        infoMixingMasternode = infoMn;
        pendingCJaRequest = CPendingCJaRequest(infoMn.addr, nSessionDenom);
        LogPrintf("%s CCoinJoinClientSession::StartNewQueue -- pending connection, nSessionDenom: %d (%s), addr=%s\n",
                  m_wallet_session->GetDisplayName(),
                  nSessionDenom, CCoinJoin::GetDenominationsToString(nSessionDenom), infoMn.addr.ToString());
        strAutoCoinJoinResult = _("Trying to connect...").translated;
        return true;
    }

    // network is not ready, try later
    strAutoCoinJoinResult = _("No compatible Masternode found, will retry...").translated;
    SetState(POOL_STATE_ERROR);
    return false;
}

bool CCoinJoinClientSession::ProcessPendingCJaRequest()
{
    if (!pendingCJaRequest) return false;

    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::ProcessPendingDsaRequest -- processing cja queue for addr=%s\n", m_wallet_session->GetDisplayName(), pendingCJaRequest.GetAddr().ToString());

    if (m_wallet_session->chain().processCJRequest(pendingCJaRequest.GetAddr(), NetMsgType::CJACCEPT, pendingCJaRequest.GetDenom())) {
        SetState(POOL_STATE_QUEUE);
        pendingCJaRequest = CPendingCJaRequest();
        return true;
    } else if (pendingCJaRequest.IsExpired()) {
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientSession::ProcessPendingDsaRequest -- failed to connect to %s\n", m_wallet_session->GetDisplayName(), pendingCJaRequest.GetAddr().ToString());
        SetNull();
    }
    return false;
}

void CCoinJoinClientManager::ProcessPendingCJaRequest()
{
    LOCK(cs_deqsessions);
    for (auto& session : deqSessions) {
        if (session.ProcessPendingCJaRequest()) {
            strAutoCoinJoinResult = _("Mixing in progress...").translated;
        }
    }
}

// Create denominations
bool CCoinJoinClientManager::CreateDenominated(const CAmount& nValue, std::vector<CAmount>& vecAmounts)
{
    if (!m_wallet) return false;


    CAmount nValueLeft = nValue;
    CKeyHolderStorage keyHolderStorageDenom;
    CMutableTransaction mtx;

    auto feetarget = COINJOIN_FEE_DENOM_THRESHOLD * COINJOIN_DENOM_WINDOW - 2;
    auto normtarget = COINJOIN_DENOM_WINDOW * COINJOIN_DENOM_THRESHOLD - GetRandInt(COINJOIN_DENOM_THRESHOLD);
    if (normtarget == COINJOIN_DENOM_WINDOW * COINJOIN_DENOM_THRESHOLD) normtarget -= 2;

    // ****** Add outputs for denoms ************ /

    while (nValueLeft > 0)
    {
        size_t tx_size = 3 + GetRand(15);
        for (auto denom = COINJOIN_LOW_DENOM; denom <= COINJOIN_HIGH_DENOM; denom <<= 1) {
            if (nValueLeft < denom) break;
            int64_t count = 0;
            auto target = denom == COINJOIN_LOW_DENOM ? feetarget : normtarget;
            for (const auto& amount : vecAmounts) {
                if (amount == denom) count++;
                if (count >= target) break;
            }
            if (count >= target) continue;
            // missing denoms
            while (nValueLeft >= denom && count < target && mtx.vout.size() < tx_size) {
                count++;
                CScript scriptDenom;
                keyHolderStorageDenom.AddKey(scriptDenom, m_wallet);

                if (scriptDenom.empty()) {
                    LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- No script available, Keypool exhausted?\n", m_wallet->GetDisplayName());
                    return false;
                }

                vecAmounts.push_back(denom);
                mtx.vout.push_back(CTxOut(denom, scriptDenom));

                //subtract denomination amount
                nValueLeft -= denom;
                LogPrint(BCLog::CJOIN, "%s CreateDenominated step 1: mtx: %s, outputs: %u, nValueLeft: %f\n", m_wallet->GetDisplayName(), mtx.GetHash().ToString(), mtx.vout.size(), (float)nValueLeft/COIN);
            }
        }

        // add the remainder
        for (auto denom = COINJOIN_HIGH_DENOM; denom >= COINJOIN_LOW_DENOM; denom >>= 1) {
            if (nValueLeft == 0) break;
            if (nValueLeft >= denom  && mtx.vout.size() < tx_size) {
                CScript scriptDenom;
                keyHolderStorageDenom.AddKey(scriptDenom, m_wallet);

                if (scriptDenom.empty()) {
                    LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- No script available, Keypool exhausted?\n", m_wallet->GetDisplayName());
                    return false;
                }

                vecAmounts.push_back(denom);
                mtx.vout.push_back(CTxOut(denom, scriptDenom));

                //subtract denomination amount
                nValueLeft -= denom;
                LogPrint(BCLog::CJOIN, "%s CreateDenominated step 2: mtx: %s, outputs: %u, nValueLeft: %f\n", m_wallet->GetDisplayName(), mtx.GetHash().ToString(), mtx.vout.size(), (float)nValueLeft/COIN);
            }
        }

        std::sort(vecAmounts.begin(), vecAmounts.end());
        // add entropy
        Shuffle(mtx.vout.begin(), mtx.vout.end(), FastRandomContext());

        // add fees
        CAmount fee_out = 0;
        int change_position = -1;
        std::string strFailReason;
        std::set<int> setSubtractFeeFromOutputs;
        CCoinControl coinControl;
        coinControl.fAllowOtherInputs = true;

        if (!m_wallet->FundTransaction(mtx, fee_out, change_position, strFailReason, true, setSubtractFeeFromOutputs, coinControl)) {
            LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- ERROR: funding transaction failed, mtx=%s, reason=%s\n",
                      m_wallet->GetDisplayName(),
                      mtx.GetHash().ToString(), strFailReason);
            return false;
        }

        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CreateDenominated -- FundTransaction: %s fees: %u\n", m_wallet->GetDisplayName(), mtx.GetHash().ToString(), fee_out);

        PartiallySignedTransaction ptx(mtx);

        bool complete = true;
        bool sign = true;
        const TransactionError err = FillPSBT(m_wallet, ptx, complete, 1, sign, false);
        LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::CreateDenominated -- FillPSBT completed: %b\n", m_wallet->GetDisplayName(), complete);

        if (err != TransactionError::OK) {
            LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- ERROR: signing transaction failed, ptx=%s, error=%s\n",
                      m_wallet->GetDisplayName(),
                      ptx.tx->GetHash().ToString(), TransactionErrorString(err));
            return false;
        }

        if (!FinalizeAndExtractPSBT(ptx, mtx)) {
            LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- FinalizeAndExtractPSBT() error: Transaction not final\n", m_wallet->GetDisplayName());
            return false;
        }

        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        CWalletTx wtx(m_wallet, tx);

        // Broadcast transaction
        m_wallet->CommitTransaction(tx, std::move(wtx.mapValue), {} /* orderForm */);

        keyHolderStorageDenom.KeepAll();

        UpdatedSuccessBlock();
        LogPrintf("%s CCoinJoinClientManager::CreateDenominated -- Success!\n", m_wallet->GetDisplayName());
    }

    return true;
}

void CCoinJoinClientSession::SetState(PoolState nStateNew)
{
    LogPrintf("%s CCoinJoinClientSession::SetState -- nState: %d, nStateNew: %d\n", m_wallet_session->GetDisplayName(), GetStateString(), nStateNew);
    nState = nStateNew;
}

void CCoinJoinClientManager::UpdatedBlockTip(const int nHeight) {
    nCachedBlockHeight = nHeight;
    LogPrint(BCLog::CJOIN, "%s CCoinJoinClientManager::UpdatedBlockTip -- nCachedBlockHeight: %d\n", m_wallet->GetDisplayName(), nCachedBlockHeight);
    CheckResult(nCachedBlockHeight);
    if (fEnableCoinJoin && !WaitForAnotherBlock() && !fActive) CoinJoin();
}

void CCoinJoinClientManager::ClientTask()
{
    if (fLiteMode || !masternodeSync.IsBlockchainSynced() || ShutdownRequested())
        return;

    if (fEnableCoinJoin) ProcessPendingCJaRequest();
}
