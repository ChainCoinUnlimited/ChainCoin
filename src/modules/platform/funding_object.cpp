// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <modules/platform/funding_object.h>

#include <modules/masternode/masternode_man.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/platform/funding.h>
#include <modules/platform/funding_validators.h>
#include <ui_interface.h>
#include <util/message.h>
#include <util/strencodings.h>
#include <validation.h>


CGovernanceObject::CGovernanceObject():
    cs_fobject(),
    nObjectType(GOVERNANCE_OBJECT_UNKNOWN),
    nHashParent(),
    nRevision(0),
    nTime(0),
    nDeletionTime(0),
    nCollateralHash(),
    vchData(),
    masternodeOutpoint(),
    vchSig(),
    fCachedLocalValidity(false),
    strLocalValidityError(),
    fCachedFunding(false),
    fCachedValid(true),
    fCachedDelete(false),
    fCachedEndorsed(false),
    fDirtyCache(true),
    fExpired(false),
    fUnparsable(false),
    mapCurrentMNVotes(),
    cmmapOrphanVotes(),
    fileVotes()
{
    // PARSE JSON DATA STORAGE (VCHDATA)
    LoadData();
}

CGovernanceObject::CGovernanceObject(const uint256& nHashParentIn, int nRevisionIn, int64_t nTimeIn, const uint256& nCollateralHashIn, const std::string& strDataHexIn):
    cs_fobject(),
    nObjectType(GOVERNANCE_OBJECT_UNKNOWN),
    nHashParent(nHashParentIn),
    nRevision(nRevisionIn),
    nTime(nTimeIn),
    nDeletionTime(0),
    nCollateralHash(nCollateralHashIn),
    vchData(ParseHex(strDataHexIn)),
    masternodeOutpoint(),
    vchSig(),
    fCachedLocalValidity(false),
    strLocalValidityError(),
    fCachedFunding(false),
    fCachedValid(true),
    fCachedDelete(false),
    fCachedEndorsed(false),
    fDirtyCache(true),
    fExpired(false),
    fUnparsable(false),
    mapCurrentMNVotes(),
    cmmapOrphanVotes(),
    fileVotes()
{
    // PARSE JSON DATA STORAGE (VCHDATA)
    LoadData();
}

CGovernanceObject::CGovernanceObject(const CGovernanceObject& other):
    cs_fobject(),
    nObjectType(other.nObjectType),
    nHashParent(other.nHashParent),
    nRevision(other.nRevision),
    nTime(other.nTime),
    nDeletionTime(other.nDeletionTime),
    nCollateralHash(other.nCollateralHash),
    vchData(other.vchData),
    masternodeOutpoint(other.masternodeOutpoint),
    vchSig(other.vchSig),
    fCachedLocalValidity(other.fCachedLocalValidity),
    strLocalValidityError(other.strLocalValidityError),
    fCachedFunding(other.fCachedFunding),
    fCachedValid(other.fCachedValid),
    fCachedDelete(other.fCachedDelete),
    fCachedEndorsed(other.fCachedEndorsed),
    fDirtyCache(other.fDirtyCache),
    fExpired(other.fExpired),
    fUnparsable(other.fUnparsable),
    mapCurrentMNVotes(other.mapCurrentMNVotes),
    cmmapOrphanVotes(other.cmmapOrphanVotes),
    fileVotes(other.fileVotes)
{}

bool CGovernanceObject::ProcessVote(CNode* pfrom,
    const CGovernanceVote& vote,
    CGovernanceException& exception,
    CConnman* connman)
{
    LOCK(cs_fobject);

    std::string strResult;

    // do not process already known valid votes twice
    if (fileVotes.HasVote(vote.GetHash())) {
        // nothing to do here, not an error
        strResult = "CGovernanceObject::ProcessVote -- Already known valid vote";
        LogPrint(BCLog::GOV, "%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_NONE);
        return false;
    }

    if (!mnodeman.Has(vote.GetMasternodeOutpoint())) {
        strResult = strprintf("CGovernanceObject::ProcessVote -- Masternode " + vote.GetMasternodeOutpoint().ToStringShort() + " not found");
        LogPrint(BCLog::GOV, "%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_WARNING);
        if (cmmapOrphanVotes.Insert(vote.GetMasternodeOutpoint(), vote_time_pair_t(vote, GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME))) {
            if (pfrom) {
                mnodeman.AskForMN(pfrom, vote.GetMasternodeOutpoint(), connman);
            }
            LogPrintf("%s\n", strResult);
        }
        else {
            LogPrint(BCLog::GOV, "%s\n", strResult);
        }
        return false;
    }

    vote_m_it it = mapCurrentMNVotes.emplace(vote_m_t::value_type(vote.GetMasternodeOutpoint(), vote_rec_t())).first;
    vote_rec_t& voteRecordRef = it->second;
    vote_signal_enum_t eSignal = vote.GetSignal();
    if (eSignal == VOTE_SIGNAL_NONE) {
        strResult = "CGovernanceObject::ProcessVote -- Vote signal: none";
        LogPrint(BCLog::GOV, "%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_WARNING);
        return false;
    }
    if (eSignal > MAX_SUPPORTED_VOTE_SIGNAL) {
        strResult = strprintf("CGovernanceObject::ProcessVote -- Unsupported vote signal: " + CGovernanceVoting::ConvertSignalToString(vote.GetSignal()));
        LogPrintf("%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        return false;
    }
    vote_instance_m_it it2 = voteRecordRef.mapInstances.emplace(vote_instance_m_t::value_type(int(eSignal), vote_instance_t())).first;
    vote_instance_t& voteInstanceRef = it2->second;

    // Reject obsolete votes
    if (vote.GetTimestamp() < voteInstanceRef.nCreationTime) {
        strResult = "CGovernanceObject::ProcessVote -- Obsolete vote";
        LogPrint(BCLog::GOV, "%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_NONE);
        return false;
    }

    int64_t nNow = GetAdjustedTime();
    int64_t nVoteTimeUpdate = voteInstanceRef.nTime;
    if (funding.AreRateChecksEnabled()) {
        int64_t nTimeDelta = nNow - voteInstanceRef.nTime;
        if (nTimeDelta < GOVERNANCE_UPDATE_MIN) {
            strResult = strprintf("CGovernanceObject::ProcessVote -- Masternode voting too often, MN outpoint = "
                 + vote.GetMasternodeOutpoint().ToStringShort()
                 + ", funding object hash = " + GetHash().ToString()
                 + ", time delta = %d", nTimeDelta);
            LogPrint(BCLog::GOV, "%s\n", strResult);
            exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_TEMPORARY_ERROR);
            nVoteTimeUpdate = nNow;
            return false;
        }
    }

    // Finally check that the vote is actually valid (done last because of cost of signature verification)
    if (!vote.IsValid(true)) {
        strResult = strprintf("CGovernanceObject::ProcessVote -- Invalid vote, MN outpoint = "
                + vote.GetMasternodeOutpoint().ToStringShort()
                + ", funding object hash = " + GetHash().ToString()
                + ", vote hash = " + vote.GetHash().ToString());
        LogPrintf("%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        funding.AddInvalidVote(vote);
        return false;
    }

    if (!mnodeman.AddGovernanceVote(vote.GetMasternodeOutpoint(), vote.GetParentHash())) {
        strResult =  strprintf("CGovernanceObject::ProcessVote -- Unable to add funding vote, MN outpoint = "
             + vote.GetMasternodeOutpoint().ToStringShort()
             + ", funding object hash = " + GetHash().ToString());
        LogPrint(BCLog::GOV, "%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_PERMANENT_ERROR);
        return false;
    }

    voteInstanceRef = vote_instance_t(vote.GetOutcome(), nVoteTimeUpdate, vote.GetTimestamp());
    fileVotes.AddVote(vote);
    fDirtyCache = true;
    return true;
}

void CGovernanceObject::ClearMasternodeVotes()
{
    LOCK(cs_fobject);

    vote_m_it it = mapCurrentMNVotes.begin();
    while(it != mapCurrentMNVotes.end()) {
        if (!mnodeman.Has(it->first)) {
            fileVotes.RemoveVotesFromMasternode(it->first);
            mapCurrentMNVotes.erase(it++);
        }
        else {
            ++it;
        }
    }
}

std::string CGovernanceObject::GetSignatureMessage() const
{
    LOCK(cs_fobject);
    std::string strMessage = nHashParent.ToString() + "|" +
        std::to_string(nRevision) + "|" +
        std::to_string(nTime) + "|" +
        GetDataAsHexString() + "|" +
        masternodeOutpoint.ToStringShort() + "|" +
        nCollateralHash.ToString();

    return strMessage;
}

uint256 CGovernanceObject::GetHash() const
{
    // Note: doesn't match serialization

    // CREATE HASH OF ALL IMPORTANT PIECES OF DATA

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nHashParent;
    ss << nRevision;
    ss << nTime;
    ss << GetDataAsHexString();
    ss << masternodeOutpoint;
    ss << vchSig;
    // fee_tx is left out on purpose

    return ss.GetHash();
}

void CGovernanceObject::SetMasternodeOutpoint(const COutPoint& outpoint)
{
    masternodeOutpoint = outpoint;
}

bool CGovernanceObject::Sign(const CKey& keyMasternode)
{
    std::string strError;

    const uint256 hash = SerializeHash(*this);

    if (!HashSign(keyMasternode, hash, vchSig)) {
        LogPrintf("CGovernanceObject::Sign -- HashSign() failed\n");
        return false;
    }

    return CheckSignature(keyMasternode.GetPubKey()) ;
}

bool CGovernanceObject::CheckSignature(const CPubKey& pubKeyMasternode) const
{
    const uint256 hash = SerializeHash(*this);

    const auto result = HashVerify(hash, pubKeyMasternode, vchSig);

    if (result != MessageVerificationResult::OK) {
        LogPrintf("CGovernance::CheckSignaturee -- HashVerify() failed!\n");
        return false;
    }

    return true;
}

/**
   Return the actual object from the vchData JSON structure.

   Returns an empty object on error.
 */
UniValue CGovernanceObject::GetJSONObject()
{
    UniValue obj(UniValue::VOBJ);
    if (vchData.empty()) {
        return obj;
    }

    UniValue objResult(UniValue::VOBJ);
    objResult.read(GetDataAsPlainString());

    if (objResult.isObject()) {
        obj = objResult;
    } else {
        std::vector<UniValue> arr1 = objResult.getValues();
        std::vector<UniValue> arr2 = arr1.at(0).getValues();
        obj = arr2.at(1);
    }

    return obj;
}

/**
*   LoadData
*   --------------------------------------------------------
*
*   Attempt to load data from vchData
*
*/

void CGovernanceObject::LoadData()
{
    if (vchData.empty()) {
        return;
    }

    try  {
        // ATTEMPT TO LOAD JSON STRING FROM VCHDATA
        UniValue obj = GetJSONObject();
        nObjectType = obj["type"].get_int();
    }
    catch(std::exception& e) {
        std::string strException;
        fUnparsable = true;
        strException = strprintf("CGovernanceObject::LoadData Error parsing JSON, e.what() = %s", e.what());
        LogPrintf("%s\n", strException);
        return;
    }
    catch(...) {
        fUnparsable = true;
        std::string strException;
        strException = "CGovernanceObject::LoadData Unknown Error parsing JSON";
        LogPrintf("%s\n", strException);
        return;
    }
}

/**
*   GetData - As
*   --------------------------------------------------------
*
*/

std::string CGovernanceObject::GetDataAsHexString() const
{
    return HexStr(vchData);
}

std::string CGovernanceObject::GetDataAsPlainString() const
{
    return std::string(vchData.begin(), vchData.end());
}

void CGovernanceObject::UpdateLocalValidity()
{
    LOCK(cs_fobject);
    // THIS DOES NOT CHECK COLLATERAL, THIS IS CHECKED UPON ORIGINAL ARRIVAL
    bool fMissingMasternode = false;
    bool fMissingConfirmations = false;
    fCachedLocalValidity = IsValidLocally(strLocalValidityError, fMissingMasternode, fMissingConfirmations, false /*fCheckCollateral*/);
};


bool CGovernanceObject::IsValidLocally(std::string& strError, bool& fMissingMasternode, bool& fMissingConfirmations, bool fCheckCollateral) const
{
    fMissingMasternode = false;
    fMissingConfirmations = false;

    if (fUnparsable) {
        strError = "Object data unparseable";
        return false;
    }

    switch(nObjectType) {
        case GOVERNANCE_OBJECT_WATCHDOG: {
            // watchdogs are deprecated
            return false;
        }
        case GOVERNANCE_OBJECT_PROPOSAL: {
            CProposalValidator validator(GetDataAsHexString());
            // Note: It's ok to have expired proposals
            // they are going to be cleared by CGovernanceManager::UpdateCachesAndClean()
            // TODO: should they be tagged as "expired" to skip vote downloading?
            if (!validator.Validate(false)) {
                strError = strprintf("Invalid proposal data, error messages: %s", validator.GetErrorMessages());
                return false;
            }
            if (fCheckCollateral && !IsCollateralValid(strError, fMissingConfirmations)) {
                strError = "Invalid proposal collateral";
                return false;
            }
            return true;
        }
        case GOVERNANCE_OBJECT_TRIGGER: {
            if (!fCheckCollateral)
                // nothing else we can check here (yet?)
                return true;

            std::string strOutpoint = masternodeOutpoint.ToStringShort();
            masternode_info_t infoMn;
            if (!mnodeman.GetMasternodeInfo(masternodeOutpoint, infoMn)) {

                CMasternode::CollateralStatus err = CMasternode::CheckCollateral(masternodeOutpoint, CPubKey());
                if (err == CMasternode::COLLATERAL_UTXO_NOT_FOUND) {
                    strError = "Failed to find Masternode UTXO, missing masternode=" + strOutpoint + "\n";
                } else if (err == CMasternode::COLLATERAL_INVALID_AMOUNT) {
                    strError = "Masternode UTXO should have 1000 CHC, missing masternode=" + strOutpoint + "\n";
                } else if (err == CMasternode::COLLATERAL_INVALID_PUBKEY) {
                    fMissingMasternode = true;
                    strError = "Masternode not found: " + strOutpoint;
                } else if (err == CMasternode::COLLATERAL_OK) {
                    // this should never happen with CPubKey() as a param
                    strError = "CheckCollateral critical failure! Masternode: " + strOutpoint;
                }

                return false;
            }

            // Check that we have a valid MN signature
            if (!CheckSignature(infoMn.pubKeyMasternode)) {
                strError = "Invalid masternode signature for: " + strOutpoint + ", pubkey id = " + infoMn.pubKeyMasternode.GetID().ToString();
                return false;
            }

            return true;
        }
        default: {
            strError = strprintf("Invalid object type %d", nObjectType);
            return false;
        }
    }

}

CAmount CGovernanceObject::GetMinCollateralFee() const
{
    // Only 1 type has a fee for the moment but switch statement allows for future object types
    switch(nObjectType) {
        case GOVERNANCE_OBJECT_PROPOSAL:    return GOVERNANCE_PROPOSAL_FEE_TX;
        case GOVERNANCE_OBJECT_TRIGGER:     return 0;
        case GOVERNANCE_OBJECT_WATCHDOG:    return 0;
        default:                            return MAX_MONEY;
    }
}

bool CGovernanceObject::IsCollateralValid(std::string& strError, bool& fMissingConfirmations) const
{
    strError = "";
    fMissingConfirmations = false;
    CAmount nMinFee = GetMinCollateralFee();
    uint256 nExpectedHash = GetHash();

    CTransactionRef txCollateral;
    uint256 nBlockHash;

    // RETRIEVE TRANSACTION IN QUESTION

    if (!GetTransaction(nCollateralHash, txCollateral, Params().GetConsensus(), nBlockHash)){
        strError = strprintf("Can't find collateral tx %s", nCollateralHash.ToString());
        LogPrintf("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    if (txCollateral->vout.size() < 1) {
        strError = strprintf("tx vout size less than 1 | %d", txCollateral->vout.size());
        LogPrintf("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    // LOOK FOR SPECIALIZED GOVERNANCE SCRIPT (PROOF OF BURN)

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    bool foundOpReturn = false;
    for (const auto& output : txCollateral->vout) {
        if (output.scriptPubKey == findScript && output.nValue >= nMinFee) {
            foundOpReturn = true;
            if (!output.scriptPubKey.IsUnspendable()) {
                strError = strprintf("Invalid Script %s", txCollateral->ToString());
                LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);
                return false;
            }
        }
    }

    if (!foundOpReturn){
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral->ToString());
        LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    // GET CONFIRMATIONS FOR TRANSACTION

    AssertLockHeld(cs_main);
    int nConfirmationsIn = GOVERNANCE_FEE_CONFIRMATIONS;
    if (nBlockHash != uint256()) {
        CBlockIndex* pindex = LookupBlockIndex(nBlockHash);
        if (::ChainActive().Contains(pindex)) {
            nConfirmationsIn += ::ChainActive().Height() - pindex->nHeight + 1;
        }
    }

    if (nConfirmationsIn < GOVERNANCE_FEE_CONFIRMATIONS) {
        strError = strprintf("Collateral requires at least %d confirmations to be relayed throughout the network (it has only %d)", GOVERNANCE_FEE_CONFIRMATIONS, nConfirmationsIn);
        if (nConfirmationsIn >= GOVERNANCE_MIN_RELAY_FEE_CONFIRMATIONS) {
            fMissingConfirmations = true;
            strError += ", pre-accepted -- waiting for required confirmations";
            LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);
            return true;
        } else {
            strError += ", rejected -- try again later";
        }
        LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);

        return false;
    }

    strError = "valid";
    return true;
}

int CGovernanceObject::CountMatchingVotes(vote_signal_enum_t eVoteSignalIn, vote_outcome_enum_t eVoteOutcomeIn) const
{
    LOCK(cs_fobject);
    int nCount = 0;
    for (const auto& votepair : mapCurrentMNVotes) {
        const vote_rec_t& recVote = votepair.second;
        vote_instance_m_cit it2 = recVote.mapInstances.find(eVoteSignalIn);
        if (it2 != recVote.mapInstances.end() && it2->second.eOutcome == eVoteOutcomeIn) {
            ++nCount;
        }
    }
    return nCount;
}

/**
*   Get specific vote counts for each outcome (funding, validity, etc)
*/

int CGovernanceObject::GetAbsoluteYesCount(vote_signal_enum_t eVoteSignalIn) const
{
    return GetYesCount(eVoteSignalIn) - GetNoCount(eVoteSignalIn);
}

int CGovernanceObject::GetAbsoluteNoCount(vote_signal_enum_t eVoteSignalIn) const
{
    return GetNoCount(eVoteSignalIn) - GetYesCount(eVoteSignalIn);
}

int CGovernanceObject::GetYesCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_YES);
}

int CGovernanceObject::GetNoCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_NO);
}

int CGovernanceObject::GetAbstainCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_ABSTAIN);
}

bool CGovernanceObject::GetCurrentMNVotes(const COutPoint& mnCollateralOutpoint, vote_rec_t& voteRecord) const
{
    LOCK(cs_fobject);

    vote_m_cit it = mapCurrentMNVotes.find(mnCollateralOutpoint);
    if (it == mapCurrentMNVotes.end()) {
        return false;
    }
    voteRecord = it->second;
    return  true;
}

void CGovernanceObject::Relay(CConnman* connman)
{
    // Do not relay until fully synced
    if (!masternodeSync.IsSynced()) {
        LogPrint(BCLog::GOV, "CGovernanceObject::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_GOVERNANCE_OBJECT, GetHash());
    connman->RelayInv(inv, MIN_GOVERNANCE_PEER_PROTO_VERSION);
}

void CGovernanceObject::UpdateSentinelVariables()
{
    // CALCULATE MINIMUM SUPPORT LEVELS REQUIRED

    int nMnCount = mnodeman.CountEnabled();
    if (nMnCount == 0) return;

    // CALCULATE THE MINUMUM VOTE COUNT REQUIRED FOR FULL SIGNAL

    int nAbsVoteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, nMnCount / 10);
    int nAbsDeleteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, (2 * nMnCount) / 3);

    // SET SENTINEL FLAGS TO FALSE

    fCachedValid = true; //default to valid
    fCachedEndorsed = false;

    // TOGGLE SENTINEL FLAGS IF MIMIMUM SUPPORT LEVELS HAVE CHANGED
    // ARE ANY OF THESE FLAGS CURRENTLY ACTIVATED?

    if ((GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING) >= nAbsVoteReq) && !fCachedFunding) {
        fCachedFunding = true;
        uiInterface.NotifyProposalChanged(this->GetHash(), CT_UPDATED);
    } else if ((GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING) < nAbsVoteReq) && fCachedFunding) {
        fCachedFunding = false;
        uiInterface.NotifyProposalChanged(this->GetHash(), CT_UPDATED);
    }

    if ((GetAbsoluteYesCount(VOTE_SIGNAL_DELETE) >= nAbsDeleteReq) && !fCachedDelete) {
        fCachedDelete = true;
        if (nDeletionTime == 0) {
            nDeletionTime = GetAdjustedTime();
        }
    }

    if ((GetAbsoluteYesCount(VOTE_SIGNAL_ENDORSED) >= nAbsVoteReq) && !fCachedEndorsed) {
        fCachedEndorsed = true;
        uiInterface.NotifyProposalChanged(this->GetHash(), CT_UPDATED);
    } else if ((GetAbsoluteYesCount(VOTE_SIGNAL_ENDORSED) < nAbsVoteReq) && fCachedEndorsed) {
        fCachedEndorsed = false;
        uiInterface.NotifyProposalChanged(this->GetHash(), CT_UPDATED);
    }

    if (GetAbsoluteNoCount(VOTE_SIGNAL_VALID) >= nAbsVoteReq) fCachedValid = false;

    fDirtyCache = false;
}

void CGovernanceObject::CheckOrphanVotes(CConnman* connman)
{
    int64_t nNow = GetAdjustedTime();
    const auto& listVotes = cmmapOrphanVotes.GetItemList();
    auto it = listVotes.begin();
    while(it != listVotes.end()) {
        bool fRemove = false;
        const COutPoint& key = it->key;
        const vote_time_pair_t& pairVote = it->value;
        const CGovernanceVote& vote = pairVote.first;
        if (pairVote.second < nNow) {
            fRemove = true;
        }
        else if (!mnodeman.Has(vote.GetMasternodeOutpoint())) {
            ++it;
            continue;
        }
        CGovernanceException exception;
        if (!ProcessVote(nullptr, vote, exception, connman)) {
            LogPrintf("CGovernanceObject::CheckOrphanVotes -- Failed to add orphan vote: %s\n", exception.what());
        }
        else {
            vote.Relay(connman);
            fRemove = true;
        }
        ++it;
        if (fRemove) {
            cmmapOrphanVotes.Erase(key, pairVote);
        }
    }
}
