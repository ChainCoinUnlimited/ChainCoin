// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <modules/coinjoin/coinjoin.h>

#include <modules/masternode/activemasternode.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <modules/masternode/masternode_payments.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/masternode/masternode_man.h>
#include <messagesigner.h>
#include <netmessagemaker.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <reverse_iterator.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/translation.h>

#include <numeric>
#include <string>

//! Discourage users to set fees higher than this amount (in satoshis) per kB
constexpr CAmount HIGH_TX_FEE_PER_KB{COIN / 500};
//! -maxtxfee will warn if called with a higher fee than this amount (in satoshis)
constexpr CAmount HIGH_MAX_TX_FEE{100 * HIGH_TX_FEE_PER_KB};

uint256 CCoinJoinQueue::GetSignatureHash() const
{
    return SerializeHash(*this);
}

bool CCoinJoinQueue::Sign()
{
    if(!fMasternodeMode) return false;

    std::string strError = "";

    uint256 hash = GetSignatureHash();

    if (!CHashSigner::SignHash(hash, activeMasternode.keyMasternode, vchSig)) {
        LogPrintf("CCoinJoinQueue::Sign -- SignHash() failed\n");
        return false;
    }

    if (!CHashSigner::VerifyHash(hash, activeMasternode.pubKeyMasternode, vchSig, strError)) {
        LogPrintf("CCoinJoinQueue::Sign -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CCoinJoinQueue::CheckSignature(const CPubKey& pubKeyMasternode) const
{
    std::string strError = "";

    uint256 hash = GetSignatureHash();

    if (!CHashSigner::VerifyHash(hash, pubKeyMasternode, vchSig, strError)) {
        // we don't care about queues with old signature format
        LogPrintf("CCoinJoinQueue::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CCoinJoinQueue::Relay(CConnman* connman)
{
    connman->ForEachNode([&connman, this](CNode* pnode) {
        CNetMsgMaker msgMaker(pnode->GetSendVersion());
        if (pnode->nVersion >= MIN_COINJOIN_PEER_PROTO_VERSION)
            connman->PushMessage(pnode, msgMaker.Make(NetMsgType::CJQUEUE, (*this)));
    });
    return true;
}

bool CCoinJoinQueue::Push(const CService pto, CConnman* connman)
{
    bool fOK = connman->ForNode(pto, [&connman, this](CNode* pnode) {
        CNetMsgMaker msgMaker(pnode->GetSendVersion());
        if (pnode->nVersion >= MIN_COINJOIN_PEER_PROTO_VERSION)
            connman->PushMessage(pnode, msgMaker.Make(NetMsgType::CJQUEUE, (*this)));
            return true;
    });
    return fOK;
}

uint256 CCoinJoinBroadcastTx::GetSignatureHash() const
{
    return SerializeHash(*this);
}

bool CCoinJoinBroadcastTx::Sign()
{
    if(!fMasternodeMode) return false;

    std::string strError = "";

    uint256 hash = GetSignatureHash();

    if (!CHashSigner::SignHash(hash, activeMasternode.keyMasternode, vchSig)) {
        LogPrintf("CCoinJoinBroadcastTx::Sign -- SignHash() failed\n");
        return false;
    }

    if (!CHashSigner::VerifyHash(hash, activeMasternode.pubKeyMasternode, vchSig, strError)) {
        LogPrintf("CCoinJoinBroadcastTx::Sign -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CCoinJoinBroadcastTx::CheckSignature(const CPubKey& pubKeyMasternode) const
{
    std::string strError = "";

    uint256 hash = GetSignatureHash();

    if (!CHashSigner::VerifyHash(hash, pubKeyMasternode, vchSig, strError)) {
        // we don't care about dstxes with old signature format
        LogPrintf("CCoinJoinBroadcastTx::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CCoinJoinBaseSession::SetNull()
{
    // Both sides
    nState = POOL_STATE_IDLE;
    nSessionID = 0;
    nSessionDenom = 0;
    vecEntries.clear();
    finalPartiallySignedTransaction = PartiallySignedTransaction();
}

void CCoinJoinBaseManager::SetNull()
{
    LOCK(cs_vecqueue);
    vecCoinJoinQueue.clear();
}

void CCoinJoinBaseManager::CheckQueue(int nHeight)
{
    LOCK(cs_vecqueue);

    // check mixing queue objects for timeouts
    for (std::vector<CCoinJoinQueue>::iterator it = vecCoinJoinQueue.begin(); it!=vecCoinJoinQueue.end(); ++it) {
        if (it!=vecCoinJoinQueue.end() && it->IsExpired(nHeight)) {
            LogPrint(BCLog::CJOIN, "CCoinJoinBase::%s -- Removing expired queue (%s)\n", __func__, it->ToString());
            vecCoinJoinQueue.erase(it--);
        }
    }
}

bool CCoinJoinBaseManager::GetQueueItem(CCoinJoinQueue& queueRet)
{
    LOCK(cs_vecqueue);

    for (auto& queue : vecCoinJoinQueue) {
        // only try each queue once
        if (queue.fTried || !queue.IsOpen()) continue;
        queue.fTried = true;
        queueRet = queue;
        return true;
    }

    return false;
}

bool CCoinJoinBaseSession::CheckTransaction(PartiallySignedTransaction& psbtxIn, CAmount& nFeeRet, PoolMessage& errRet, bool fUnsigned)
{
    //check it like a partially signed transaction
    CAmount in_amt = 0;

    // Get the output amount
    CAmount out_amt = std::accumulate(psbtxIn.tx->vout.begin(), psbtxIn.tx->vout.end(), CAmount(0),
                                      [](CAmount a, const CTxOut& b) {
        return a += b.nValue;
    }
    );

    // Estimate the size
    CMutableTransaction mtx(*psbtxIn.tx);
    CCoinsView view_dummy;
    CCoinsViewCache view(&view_dummy);

    for (unsigned int i = 0; i < psbtxIn.tx->vin.size(); ++i) {
        PSBTInput& input = psbtxIn.inputs[i];

        CTxOut utxo;
        if (!psbtxIn.GetInputUTXO(utxo, i)) {
            LogPrintf("CCoinJoinBaseSession::CheckTransaction -- missing input! tx=%s\n", psbtxIn.tx->GetHash().ToString());
            errRet = ERR_MISSING_TX;
            return false;
        }
        if (!CCoinJoin::IsDenominatedAmount(utxo.nValue)) {
            LogPrintf("CCoinJoinBaseSession::CheckTransaction -- input not denominated! tx=%s\n", psbtxIn.tx->GetHash().ToString());
            errRet = ERR_INVALID_INPUT;
            return false;
        }

        in_amt += utxo.nValue;
        nFeeRet = in_amt - out_amt;

        if (fUnsigned) continue;

        if (SignPSBTInput(DUMMY_SIGNING_PROVIDER, psbtxIn, i, 1, nullptr, true)) {
            mtx.vin[i].scriptSig = input.final_script_sig;
            mtx.vin[i].scriptWitness = input.final_script_witness;

            Coin newcoin;
            if (!psbtxIn.GetInputUTXO(newcoin.out, i)) {
                LogPrintf("CCoinJoinBaseSession::CheckTransaction -- missing input! tx=%s\n", psbtxIn.tx->GetHash().ToString());
                errRet = ERR_MISSING_TX;
                return false;
            }
            newcoin.nHeight = 1;
            view.AddCoin(psbtxIn.tx->vin[i].prevout, std::move(newcoin), true);
        } else {
            LogPrintf("CCoinJoinBaseSession::CheckTransaction -- dummy signing input failed! tx=%s\n", psbtxIn.tx->GetHash().ToString());
            errRet = ERR_INVALID_INPUT;
            return false;
        }
    }

    if (fUnsigned) return true;

    CTransaction ctx = CTransaction(mtx);
    size_t size = GetVirtualTransactionSize(ctx, GetTransactionSigOpCost(ctx, view, STANDARD_SCRIPT_VERIFY_FLAGS));
    // Estimate fee rate
    CFeeRate feerate(nFeeRet, size);

    LogPrint(BCLog::CJOIN, "CCoinJoinBaseSession::CheckTransaction -- estimated_vsize: %d, estimated_feerate: %s\n", size, feerate.ToString());

    // There should be fee in mixing tx right now, but no sig data - simple check
    if (feerate < ::minRelayTxFee.GetFeePerK() || feerate > HIGH_TX_FEE_PER_KB || nFeeRet > HIGH_MAX_TX_FEE) {
        LogPrintf("CCoinJoinBaseSession::CheckTransaction -- there must be fee in mixing tx! feerate: %lld, tx=%s\n", feerate.ToString(), psbtxIn.tx->GetHash().ToString());
        errRet = ERR_FEES;
        return false;
    }
    return true;
}

std::string CCoinJoinBaseSession::GetStateString() const
{
    switch (nState) {
    case POOL_STATE_IDLE:
        return "IDLE";
    case POOL_STATE_CONNECTING:
        return "CONNECTING";
    case POOL_STATE_QUEUE:
        return "QUEUE";
    case POOL_STATE_ACCEPTING_ENTRIES:
        return "ACCEPTING_ENTRIES";
    case POOL_STATE_SIGNING:
        return "SIGNING";
    case POOL_STATE_ERROR:
        return "ERROR";
    case POOL_STATE_SUCCESS:
        return "SUCCESS";
    default:
        return "UNKNOWN";
    }
}



/*  Check for the nDenom inside boundaries and return a string*/
std::string CCoinJoin::GetDenominationsToString(const CAmount& nDenom)
{
    std::string strDenom = "";

    if(!IsInDenomRange(nDenom)) {
        return "out-of-bounds";
    }

    for (auto denom = COINJOIN_HIGH_DENOM; denom >= COINJOIN_LOW_DENOM; denom >>=1) {
        if(nDenom == denom) {
            strDenom += (strDenom.empty() ? "" : "+") + FormatMoney(nDenom);
        }
    }

    if(strDenom.empty()) {
        return "multi-denom";
    }

    return strDenom;
}

CAmount CCoinJoin::GetDenomRange()
{
    static CAmount result = 0;
    if (result > 0) return result;
    for (auto denom = COINJOIN_HIGH_DENOM; denom >= COINJOIN_LOW_DENOM; denom >>=1) {
        result |= denom;
    }
    return result;
}

bool CCoinJoin::IsInDenomRange(const CAmount& nAmount)
{
    if ((nAmount | GetDenomRange()) != GetDenomRange()) return false;
    return true;

}

bool CCoinJoin::IsDenominatedAmount(const CAmount& nInputAmount)
{
    for (auto denom = COINJOIN_LOW_DENOM; denom <= COINJOIN_HIGH_DENOM; denom <<=1) {
        if(nInputAmount == denom) {
            return true;
        }
    }
    return false;
}

std::string CCoinJoin::GetMessageByID(PoolMessage nMessageID)
{
    switch (nMessageID) {
    case ERR_ALREADY_HAVE:
        return _("Already have that input.").translated;
    case ERR_DENOM:
        return _("No matching denominations found for mixing.").translated;
    case ERR_ENTRIES_FULL:
        return _("Entries are full.").translated;
    case ERR_INVALID_OUT:
        return _("Not compatible with existing transactions.").translated;
    case ERR_MN_FEES:
        return _("Missing or high masternode fees.").translated;
    case ERR_INVALID_INPUT:
        return _("Input is not valid.").translated;
    case ERR_FEES:
        return _("Included fees too high or too low.").translated;
    case ERR_INVALID_TX:
        return _("Transaction not valid.").translated;
    case ERR_MAXIMUM:
        return _("Entry exceeds maximum size.").translated;
    case ERR_MN_LIST:
        return _("Not in the Masternode list.").translated;
    case ERR_MODE:
        return _("Incompatible mode.").translated;
    case ERR_QUEUE_FULL:
        return _("Masternode queue is full.").translated;
    case ERR_RECENT:
        return _("Last CoinJoin was too recent.").translated;
    case ERR_SESSION:
        return _("Session not complete!").translated;
    case ERR_MISSING_TX:
        return _("Missing input transaction information.").translated;
    case ERR_VERSION:
        return _("Incompatible version.").translated;
    case MSG_NOERR:
        return _("No errors detected.").translated;
    case MSG_SUCCESS:
        return _("Transaction created successfully.").translated;
    case MSG_ENTRIES_ADDED:
        return _("Your entries added successfully.").translated;
    default:
        return _("Unknown response.").translated;
    }
}

