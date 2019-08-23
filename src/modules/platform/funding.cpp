// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <modules/platform/funding.h>

#include <consensus/validation.h>
#include <messagesigner.h>
#include <modules/masternode/masternode.h>
#include <modules/masternode/masternode_config.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/masternode/masternode_man.h>
#include <modules/platform/funding_classes.h>
#include <modules/platform/funding_validators.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <netfulfilledman.h>
#include <ui_interface.h>
#include <util/strencodings.h>
#include <util/system.h>

CGovernanceManager funding;

int nSubmittedFinalBudget;

const std::string CGovernanceManager::SERIALIZATION_VERSION_STRING = "CGovernanceManager-Version-12";
const int CGovernanceManager::MAX_TIME_FUTURE_DEVIATION = 60*60;
const int CGovernanceManager::RELIABLE_PROPAGATION_TIME = 60;

CGovernanceManager::CGovernanceManager()
    : nTimeLastDiff(0),
      nCachedBlockHeight(0),
      mapObjects(),
      mapErasedGovernanceObjects(),
      mapMasternodeOrphanObjects(),
      cmapVoteToObject(MAX_CACHE_SIZE),
      cmapInvalidVotes(MAX_CACHE_SIZE),
      cmmapOrphanVotes(MAX_CACHE_SIZE),
      mapLastMasternodeObject(),
      setRequestedObjects(),
      fRateChecksEnabled(true),
      cs()
{}

// Accessors for thread-safe access to maps
bool CGovernanceManager::HaveObjectForHash(const uint256& nHash) const
{
    LOCK(cs);
    return (mapObjects.count(nHash) == 1 || mapPostponedObjects.count(nHash) == 1);
}

bool CGovernanceManager::SerializeObjectForHash(const uint256& nHash, CDataStream& ss) const
{
    LOCK(cs);
    const auto& it1 = mapObjects.find(nHash);
    const auto& it2 = mapPostponedObjects.find(nHash);
    if (it1 !=mapObjects.end()) {
        ss << it1->second;
        return true;
    } else if (it2 != mapPostponedObjects.end()) {
        ss << it2->second;
        return true;
    } else return false;
}

bool CGovernanceManager::HaveVoteForHash(const uint256& nHash) const
{
    LOCK(cs);

    CGovernanceObject* pGovobj = nullptr;
    return cmapVoteToObject.Get(nHash, pGovobj) && pGovobj->GetVoteFile().HasVote(nHash);
}

int CGovernanceManager::GetVoteCount() const
{
    LOCK(cs);
    return (int)cmapVoteToObject.GetSize();
}

bool CGovernanceManager::SerializeVoteForHash(const uint256& nHash, CDataStream& ss) const
{
    LOCK(cs);

    CGovernanceObject* pGovobj = nullptr;
    return cmapVoteToObject.Get(nHash,pGovobj) && pGovobj->GetVoteFile().SerializeVoteToStream(nHash, ss);
}

void CGovernanceManager::ProcessModuleMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman)
{
    // lite mode is not supported
    if(fLiteMode) return;
    if(!masternodeSync.IsBlockchainSynced()) return;

    // ANOTHER USER IS ASKING US TO HELP THEM SYNC GOVERNANCE OBJECT DATA
    if (strCommand == NetMsgType::MNGOVERNANCESYNC)
    {
        if(pfrom->GetSendVersion() < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint(BCLog::GOV, "MNGOVERNANCESYNC -- peer=%d using obsolete version %i\n", pfrom->GetId(), pfrom->GetSendVersion());
            connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
            return;
        }

        // Ignore such requests until we are fully synced.
        // We could start processing this after masternode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!masternodeSync.IsSynced()) return;

        uint256 nProp;

        vRecv >> nProp;

        if(nProp == uint256()) {
            SyncAll(pfrom, connman);
        } else {
            CBloomFilter filter;
            vRecv >> filter;
            filter.UpdateEmptyFull();
            SyncSingleObjAndItsVotes(pfrom, nProp, filter, connman);
        }
        LogPrint(BCLog::GOV, "MNGOVERNANCESYNC -- syncing funding objects to our peer at %s\n", pfrom->addr.ToString());
    }

    // A NEW GOVERNANCE OBJECT HAS ARRIVED
    else if (strCommand == NetMsgType::MNGOVERNANCEOBJECT)
    {
        // MAKE SURE WE HAVE A VALID REFERENCE TO THE TIP BEFORE CONTINUING

        CGovernanceObject govobj;
        vRecv >> govobj;

        uint256 nHash = govobj.GetHash();

        if(pfrom->GetSendVersion() < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECT -- peer=%d using obsolete version %i\n", pfrom->GetId(), pfrom->GetSendVersion());
            connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
            return;
        }

        if(!masternodeSync.IsMasternodeListSynced()) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECT -- masternode list not synced\n");
            return;
        }

        std::string strHash = nHash.ToString();

        LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECT -- Received object: %s\n", strHash);

        if(!AcceptObjectMessage(nHash)) {
            LogPrintf("MNGOVERNANCEOBJECT -- Received unrequested object: %s\n", strHash);
            return;
        }

        LOCK2(cs_main, cs);

        if(mapObjects.count(nHash) || mapPostponedObjects.count(nHash) ||
           mapErasedGovernanceObjects.count(nHash) || mapMasternodeOrphanObjects.count(nHash)) {
            // TODO - print error code? what if it's GOVOBJ_ERROR_IMMATURE?
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECT -- Received already seen object: %s\n", strHash);
            return;
        }

        bool fRateCheckBypassed = false;
        if(!MasternodeRateCheck(govobj, true, false, fRateCheckBypassed)) {
            LogPrintf("MNGOVERNANCEOBJECT -- masternode rate check failed - %s - (current block height %d) \n", strHash, nCachedBlockHeight);
            return;
        }

        std::string strError = "";
        // CHECK OBJECT AGAINST LOCAL BLOCKCHAIN

        bool fMasternodeMissing = false;
        bool fMissingConfirmations = false;
        bool fIsValid = govobj.IsValidLocally(strError, fMasternodeMissing, fMissingConfirmations, true);

        if(fRateCheckBypassed && (fIsValid || fMasternodeMissing)) {
            if(!MasternodeRateCheck(govobj, true)) {
                LogPrintf("MNGOVERNANCEOBJECT -- masternode rate check failed (after signature verification) - %s - (current block height %d) \n", strHash, nCachedBlockHeight);
                return;
            }
        }

        if(!fIsValid) {
            if(fMasternodeMissing) {
                ExpirationInfo info(pfrom->GetId(), GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME);
                mapMasternodeOrphanObjects.insert(std::make_pair(nHash, object_info_pair_t(govobj, info)));
                LogPrintf("MNGOVERNANCEOBJECT -- Missing masternode %s for: %s, strError = %s\n", govobj.GetMasternodeOutpoint().ToStringShort(), strHash, strError);
            } else if(fMissingConfirmations) {
                AddPostponedObject(govobj);
                LogPrintf("MNGOVERNANCEOBJECT -- Not enough fee confirmations for: %s, strError = %s\n", strHash, strError);
            } else {
                LogPrintf("MNGOVERNANCEOBJECT -- Governance object is invalid - %s\n", strError);
                // apply node's ban score
                Misbehaving(pfrom->GetId(), 20);
            }

            return;
        }

        AddGovernanceObject(govobj, connman, pfrom);
    }

    // A NEW GOVERNANCE OBJECT VOTE HAS ARRIVED
    else if (strCommand == NetMsgType::MNGOVERNANCEOBJECTVOTE)
    {
        CGovernanceVote vote;
        vRecv >> vote;

        uint256 nHash = vote.GetHash();

        if(pfrom->GetSendVersion() < MIN_GOVERNANCE_PEER_PROTO_VERSION) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- peer=%d using obsolete version %i\n", pfrom->GetId(), pfrom->GetSendVersion());
            connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_GOVERNANCE_PEER_PROTO_VERSION)));
        }

        // Ignore such messages until masternode list is synced
        if(!masternodeSync.IsMasternodeListSynced()) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- masternode list not synced\n");
            return;
        }

        LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- Received vote: %s\n", vote.ToString());

        std::string strHash = nHash.ToString();

        if(!AcceptVoteMessage(nHash)) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- Received unrequested vote object: %s, hash: %s, peer = %d\n",
                      vote.ToString(), strHash, pfrom->GetId());
            return;
        }

        CGovernanceException exception;
        if(ProcessVote(pfrom, vote, exception, connman)) {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- %s new\n", strHash);
            masternodeSync.BumpAssetLastTime("MNGOVERNANCEOBJECTVOTE");
            vote.Relay(connman);
        }
        else {
            LogPrint(BCLog::GOV, "MNGOVERNANCEOBJECTVOTE -- Rejected vote, error = %s\n", exception.what());
            if((exception.GetNodePenalty() != 0) && masternodeSync.IsSynced()) {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), exception.GetNodePenalty());
            }
            return;
        }
        // SEND NOTIFICATION TO SCRIPT/ZMQ
        GetMainSignals().NotifyGovernanceVote(vote);
        uiInterface.NotifyProposalChanged(vote.GetParentHash(), CT_UPDATED);
    }
}

void CGovernanceManager::CheckOrphanVotes(CGovernanceObject& govobj, CGovernanceException& exception, CConnman* connman)
{
    uint256 nHash = govobj.GetHash();
    std::vector<vote_time_pair_t> vecVotePairs;
    cmmapOrphanVotes.GetAll(nHash, vecVotePairs);

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    int64_t nNow = GetAdjustedTime();
    for(size_t i = 0; i < vecVotePairs.size(); ++i) {
        bool fRemove = false;
        vote_time_pair_t& pairVote = vecVotePairs[i];
        CGovernanceVote& vote = pairVote.first;
        CGovernanceException exception;
        if(pairVote.second < nNow) {
            fRemove = true;
        }
        else if(govobj.ProcessVote(nullptr, vote, exception, connman)) {
            vote.Relay(connman);
            fRemove = true;
        }
        if(fRemove) {
            cmmapOrphanVotes.Erase(nHash, pairVote);
        }
    }
}

void CGovernanceManager::AddGovernanceObject(CGovernanceObject& govobj, CConnman* connman, CNode* pfrom)
{
    uint256 nHash = govobj.GetHash();
    std::string strHash = nHash.ToString();

    // UPDATE CACHED VARIABLES FOR THIS OBJECT AND ADD IT TO OUR MANANGED DATA

    govobj.UpdateSentinelVariables(); //this sets local vars in object

    LOCK2(cs_main, cs);
    std::string strError = "";

    // MAKE SURE THIS OBJECT IS OK

    if(!govobj.IsValidLocally(strError, true)) {
        LogPrintf("CGovernanceManager::AddGovernanceObject -- invalid funding object - %s - (nCachedBlockHeight %d) \n", strError, nCachedBlockHeight);
        return;
    }

    LogPrint(BCLog::GOV, "CGovernanceManager::AddGovernanceObject -- Adding object: hash = %s, type = %d\n", nHash.ToString(), govobj.GetObjectType());

    // INSERT INTO OUR GOVERNANCE OBJECT MEMORY
    // IF WE HAVE THIS OBJECT ALREADY, WE DON'T WANT ANOTHER COPY
    auto objpair = mapObjects.emplace(nHash, govobj);

    if(!objpair.second) {
        LogPrintf("CGovernanceManager::AddGovernanceObject -- already have funding object %s\n", nHash.ToString());
        return;
    }

    // SHOULD WE ADD THIS OBJECT TO ANY OTHER MANANGERS?

    if (govobj.nObjectType == GOVERNANCE_OBJECT_TRIGGER) {
        if (!triggerman.AddNewTrigger(nHash)) {
            LogPrint(BCLog::GOV, "CGovernanceManager::AddGovernanceObject -- undo adding invalid trigger object: hash = %s\n", nHash.ToString());
            CGovernanceObject& objref = objpair.first->second;
            objref.fCachedDelete = true;
            if (objref.nDeletionTime == 0) {
                objref.nDeletionTime = GetAdjustedTime();
            }
            return;
        }
    }

    LogPrintf("CGovernanceManager::AddGovernanceObject -- %s new, received from %s\n", strHash, pfrom? pfrom->GetAddrName() : "not found");
    govobj.Relay(connman);

    // Update the rate buffer
    MasternodeRateUpdate(govobj);

    masternodeSync.BumpAssetLastTime("CGovernanceManager::AddGovernanceObject");

    // WE MIGHT HAVE PENDING/ORPHAN VOTES FOR THIS OBJECT

    CGovernanceException exception;
    CheckOrphanVotes(govobj, exception, connman);

    // SEND NOTIFICATION TO SCRIPT/ZMQ
    GetMainSignals().NotifyGovernanceObject(govobj);
    uiInterface.NotifyProposalChanged(govobj.GetHash(), CT_NEW);
}

void CGovernanceManager::UpdateCachesAndClean()
{
    LogPrint(BCLog::GOV, "CGovernanceManager::UpdateCachesAndClean\n");

    std::vector<uint256> vecDirtyHashes = mnodeman.GetAndClearDirtyGovernanceObjectHashes();

    LOCK2(cs_main, cs);

    for(size_t i = 0; i < vecDirtyHashes.size(); ++i) {
        const auto& it = mapObjects.find(vecDirtyHashes[i]);
        if(it == mapObjects.end()) {
            continue;
        }
        it->second.ClearMasternodeVotes();
        it->second.fDirtyCache = true;
    }

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    // Clean up any expired or invalid triggers
    triggerman.CleanAndRemove();

    auto it = mapObjects.begin();
    int64_t nNow = GetAdjustedTime();

    while(it != mapObjects.end())
    {
        CGovernanceObject* pObj = &((*it).second);

        if(!pObj) {
            ++it;
            continue;
        }

        uint256 nHash = it->first;
        std::string strHash = nHash.ToString();

        // IF CACHE IS NOT DIRTY, WHY DO THIS?
        if(pObj->IsSetDirtyCache()) {
            // UPDATE LOCAL VALIDITY AGAINST CRYPTO DATA
            pObj->UpdateLocalValidity();

            // UPDATE SENTINEL SIGNALING VARIABLES
            pObj->UpdateSentinelVariables();
        }

        // IF DELETE=TRUE, THEN CLEAN THE MESS UP!

        int64_t nTimeSinceDeletion = nNow - pObj->GetDeletionTime();

        LogPrint(BCLog::GOV, "CGovernanceManager::UpdateCachesAndClean -- Checking object for deletion: %s, deletion time = %d, time since deletion = %d, delete flag = %d, expired flag = %d\n",
                 strHash, pObj->GetDeletionTime(), nTimeSinceDeletion, pObj->IsSetCachedDelete(), pObj->IsSetExpired());

        if((pObj->IsSetCachedDelete() || pObj->IsSetExpired()) &&
           (nTimeSinceDeletion >= GOVERNANCE_DELETION_DELAY)) {
            LogPrintf("CGovernanceManager::UpdateCachesAndClean -- erase obj %s\n", (*it).first.ToString());
            mnodeman.RemoveGovernanceObject(pObj->GetHash());

            // Remove vote references
            const CacheMap<uint256, CGovernanceObject*>::list_t& listItems = cmapVoteToObject.GetItemList();
            CacheMap<uint256, CGovernanceObject*>::list_cit lit = listItems.begin();
            while(lit != listItems.end()) {
                if(lit->value == pObj) {
                    uint256 nKey = lit->key;
                    ++lit;
                    cmapVoteToObject.Erase(nKey);
                }
                else {
                    ++lit;
                }
            }

            int64_t nTimeExpired{0};

            if(pObj->GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL) {
                // keep hashes of deleted proposals forever
                nTimeExpired = std::numeric_limits<int64_t>::max();
                uiInterface.NotifyProposalChanged(pObj->GetHash(), CT_DELETED);
            } else {
                int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;
                nTimeExpired = pObj->GetCreationTime() + 2 * nSuperblockCycleSeconds + GOVERNANCE_DELETION_DELAY;
            }

            mapErasedGovernanceObjects.insert(std::make_pair(nHash, nTimeExpired));
            mapObjects.erase(it++);
        } else {
            // NOTE: triggers are handled via triggerman
            if (pObj->GetObjectType() == GOVERNANCE_OBJECT_PROPOSAL) {
                CProposalValidator validator(pObj->GetDataAsHexString());
                if (!validator.Validate()) {
                    LogPrintf("CGovernanceManager::UpdateCachesAndClean -- set for deletion expired obj %s\n", (*it).first.ToString());
                    pObj->fCachedDelete = true;
                    if (pObj->nDeletionTime == 0) {
                        pObj->nDeletionTime = nNow;
                    }
                }
            }
            ++it;
        }
    }

    // forget about expired deleted objects
    std::map<uint256, int64_t>::const_iterator s_it = mapErasedGovernanceObjects.begin();
    while(s_it != mapErasedGovernanceObjects.end()) {
        if(s_it->second < nNow)
            mapErasedGovernanceObjects.erase(s_it++);
        else
            ++s_it;
    }

    LogPrintf("CGovernanceManager::UpdateCachesAndClean -- %s\n", ToString());
}

CGovernanceObject* CGovernanceManager::FindGovernanceObject(const uint256& nHash)
{
    LOCK(cs);

    if(mapObjects.count(nHash))
        return &mapObjects[nHash];

    return nullptr;
}

std::vector<CGovernanceVote> CGovernanceManager::GetMatchingVotes(const uint256& nParentHash) const
{
    LOCK(cs);
    std::vector<CGovernanceVote> vecResult;

    const auto& it = mapObjects.find(nParentHash);
    if(it == mapObjects.end()) {
        return vecResult;
    }

    return it->second.GetVoteFile().GetVotes();
}

std::vector<CGovernanceVote> CGovernanceManager::GetCurrentVotes(const uint256& nParentHash, const COutPoint& mnCollateralOutpointFilter) const
{
    LOCK(cs);
    std::vector<CGovernanceVote> vecResult;

    // Find the funding object or short-circuit.
    const auto& it = mapObjects.find(nParentHash);
    if(it == mapObjects.end()) return vecResult;
    const CGovernanceObject& govobj = it->second;

    CMasternode mn;
    std::map<COutPoint, CMasternode> mapMasternodes;
    if(mnCollateralOutpointFilter.IsNull()) {
        mapMasternodes = mnodeman.GetFullMasternodeMap();
    } else if (mnodeman.Get(mnCollateralOutpointFilter, mn)) {
        mapMasternodes[mnCollateralOutpointFilter] = mn;
    }

    // Loop thru each MN collateral outpoint and get the votes for the `nParentHash` funding object
    for (const auto& mnpair : mapMasternodes)
    {
        // get a vote_rec_t from the govobj
        vote_rec_t voteRecord;
        if (!govobj.GetCurrentMNVotes(mnpair.first, voteRecord)) continue;

        for (const auto& voteInstancePair : voteRecord.mapInstances) {
            int signal = voteInstancePair.first;
            int outcome = voteInstancePair.second.eOutcome;
            int64_t nCreationTime = voteInstancePair.second.nCreationTime;

            CGovernanceVote vote = CGovernanceVote(mnpair.first, nParentHash, (vote_signal_enum_t)signal, (vote_outcome_enum_t)outcome);
            vote.SetTime(nCreationTime);

            vecResult.push_back(vote);
        }
    }

    return vecResult;
}

std::vector<const CGovernanceObject*> CGovernanceManager::GetAllNewerThan(int64_t nMoreThanTime) const
{
    LOCK(cs);

    std::vector<const CGovernanceObject*> vGovObjs;

    for (const auto& objPair : mapObjects) {
        // IF THIS OBJECT IS OLDER THAN TIME, CONTINUE
        if(objPair.second.GetCreationTime() < nMoreThanTime) {
            continue;
        }

        // ADD GOVERNANCE OBJECT TO LIST
        const CGovernanceObject* pGovObj = &(objPair.second);
        vGovObjs.push_back(pGovObj);
    }

    return vGovObjs;
}

//
// Sort by votes, if there's a tie sort by their feeHash TX
//
struct sortProposalsByVotes {
    bool operator()(const std::pair<CGovernanceObject*, int> &left, const std::pair<CGovernanceObject*, int> &right) {
        if (left.second != right.second)
            return (left.second > right.second);
        return (UintToArith256(left.first->GetCollateralHash()) > UintToArith256(right.first->GetCollateralHash()));
    }
};

void CGovernanceManager::ClientTask(CConnman* connman)
{
    if(fLiteMode || !masternodeSync.IsSynced()) return;

    // CHECK OBJECTS WE'VE ASKED FOR, REMOVE OLD ENTRIES

    CleanOrphanObjects();

    RequestOrphanObjects(connman);

    // CHECK AND REMOVE - REPROCESS GOVERNANCE OBJECTS

    UpdateCachesAndClean();
}

bool CGovernanceManager::ConfirmInventoryRequest(const CInv& inv)
{
    // do not request objects until it's time to sync
    if(!masternodeSync.IsWinnersListSynced()) return false;

    LOCK(cs);

    LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest inv = %s\n", inv.ToString());

    // First check if we've already recorded this object
    switch(inv.type) {
    case MSG_GOVERNANCE_OBJECT:
    {
        if(mapObjects.count(inv.hash) == 1 || mapPostponedObjects.count(inv.hash) == 1) {
            LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest already have funding object, returning false\n");
            return false;
        }
    }
    break;
    case MSG_GOVERNANCE_OBJECT_VOTE:
    {
        if(cmapVoteToObject.HasKey(inv.hash)) {
            LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest already have funding vote, returning false\n");
            return false;
        }
    }
    break;
    default:
        LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest unknown type, returning false\n");
        return false;
    }


    std::set<uint256>* setHash = nullptr;
    switch(inv.type) {
    case MSG_GOVERNANCE_OBJECT:
        setHash = &setRequestedObjects;
        break;
    case MSG_GOVERNANCE_OBJECT_VOTE:
        setHash = &setRequestedVotes;
        break;
    default:
        return false;
    }

    const auto& it = setHash->find(inv.hash);
    if(it == setHash->end()) {
        setHash->insert(inv.hash);
        LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest added inv to requested set\n");
    }

    LogPrint(BCLog::GOV, "CGovernanceManager::ConfirmInventoryRequest reached end, returning true\n");
    return true;
}

void CGovernanceManager::SyncSingleObjAndItsVotes(CNode* pnode, const uint256& nProp, const CBloomFilter& filter, CConnman* connman)
{
    // do not provide any data until our node is synced
    if(!masternodeSync.IsSynced()) return;

    int nVoteCount = 0;

    // SYNC GOVERNANCE OBJECTS WITH OTHER CLIENT

    LogPrint(BCLog::GOV, "CGovernanceManager::%s -- syncing single object to peer=%d, nProp = %s\n", __func__, pnode->GetId(), nProp.ToString());

    LOCK2(cs_main, cs);

    // single valid object and its valid votes
    const auto& it = mapObjects.find(nProp);
    if(it == mapObjects.end()) {
        LogPrint(BCLog::GOV, "CGovernanceManager::%s -- no matching object for hash %s, peer=%d\n", __func__, nProp.ToString(), pnode->GetId());
        return;
    }
    CGovernanceObject& govobj = it->second;
    std::string strHash = it->first.ToString();

    LogPrint(BCLog::GOV, "CGovernanceManager::%s -- attempting to sync govobj: %s, peer=%d\n", __func__, strHash, pnode->GetId());

    if(govobj.IsSetCachedDelete() || govobj.IsSetExpired()) {
        LogPrintf("CGovernanceManager::%s -- not syncing deleted/expired govobj: %s, peer=%d\n", __func__,
                  strHash, pnode->GetId());
        return;
    }

    // Push the govobj inventory message over to the other client
    LogPrint(BCLog::GOV, "CGovernanceManager::%s -- syncing govobj: %s, peer=%d\n", __func__, strHash, pnode->GetId());
    pnode->PushInventory(CInv(MSG_GOVERNANCE_OBJECT, it->first));

    auto fileVotes = govobj.GetVoteFile();

    for (const auto& vote : fileVotes.GetVotes()) {
        uint256 nVoteHash = vote.GetHash();
        if(filter.contains(nVoteHash) || !vote.IsValid(true)) {
            continue;
        }
        pnode->PushInventory(CInv(MSG_GOVERNANCE_OBJECT_VOTE, nVoteHash));
        ++nVoteCount;
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman->PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ, 1));
    connman->PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ_VOTE, nVoteCount));
    LogPrintf("CGovernanceManager::%s -- sent 1 object and %d votes to peer=%d\n", __func__, nVoteCount, pnode->GetId());
}

void CGovernanceManager::SyncAll(CNode* pnode, CConnman* connman) const
{
    // do not provide any data until our node is synced
    if(!masternodeSync.IsSynced()) return;

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, NetMsgType::MNGOVERNANCESYNC)) {
        LOCK(cs_main);
        // Asking for the whole list multiple times in a short period of time is no good
        LogPrint(BCLog::GOV, "CGovernanceManager::%s -- peer already asked me for the list\n", __func__);
        Misbehaving(pnode->GetId(), 20);
        return;
    }
    netfulfilledman.AddFulfilledRequest(pnode->addr, NetMsgType::MNGOVERNANCESYNC);

    int nObjCount = 0;
    int nVoteCount = 0;

    // SYNC GOVERNANCE OBJECTS WITH OTHER CLIENT

    LogPrint(BCLog::GOV, "CGovernanceManager::%s -- syncing all objects to peer=%d\n", __func__, pnode->GetId());

    LOCK2(cs_main, cs);

    // all valid objects, no votes
    for (const auto& objPair : mapObjects) {
        uint256 nHash = objPair.first;
        const CGovernanceObject& govobj = objPair.second;
        std::string strHash = nHash.ToString();

        LogPrint(BCLog::GOV, "CGovernanceManager::%s -- attempting to sync govobj: %s, peer=%d\n", __func__, strHash, pnode->GetId());

        if(govobj.IsSetCachedDelete() || govobj.IsSetExpired()) {
            LogPrintf("CGovernanceManager::%s -- not syncing deleted/expired govobj: %s, peer=%d\n", __func__,
                      strHash, pnode->GetId());
            continue;
        }

        // Push the inventory budget proposal message over to the other client
        LogPrint(BCLog::GOV, "CGovernanceManager::%s -- syncing govobj: %s, peer=%d\n", __func__, strHash, pnode->GetId());
        pnode->PushInventory(CInv(MSG_GOVERNANCE_OBJECT, nHash));
        ++nObjCount;
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman->PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ, nObjCount));
    connman->PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_GOVOBJ_VOTE, nVoteCount));
    LogPrintf("CGovernanceManager::%s -- sent %d objects and %d votes to peer=%d\n", __func__, nObjCount, nVoteCount, pnode->GetId());
}

void CGovernanceManager::MasternodeRateUpdate(const CGovernanceObject& govobj)
{
    if(govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER)
        return;

    const COutPoint& masternodeOutpoint = govobj.GetMasternodeOutpoint();
    auto it  = mapLastMasternodeObject.find(masternodeOutpoint);

    if(it == mapLastMasternodeObject.end())
        it = mapLastMasternodeObject.insert(std::map<COutPoint, last_object_rec>::value_type(masternodeOutpoint, last_object_rec(true))).first;

    int64_t nTimestamp = govobj.GetCreationTime();
    it->second.triggerBuffer.AddTimestamp(nTimestamp);

    if (nTimestamp > GetTime() + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME) {
        // schedule additional relay for the object
        setAdditionalRelayObjects.insert(govobj.GetHash());
    }

    it->second.fStatusOK = true;
}

bool CGovernanceManager::MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus)
{
    bool fRateCheckBypassed;
    return MasternodeRateCheck(govobj, fUpdateFailStatus, true, fRateCheckBypassed);
}

bool CGovernanceManager::MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus, bool fForce, bool& fRateCheckBypassed)
{
    LOCK(cs);

    fRateCheckBypassed = false;

    if(!masternodeSync.IsSynced()) {
        return true;
    }

    if(!fRateChecksEnabled) {
        return true;
    }

    if(govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER) {
        return true;
    }

    const COutPoint& masternodeOutpoint = govobj.GetMasternodeOutpoint();
    int64_t nTimestamp = govobj.GetCreationTime();
    int64_t nNow = GetAdjustedTime();
    int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;

    std::string strHash = govobj.GetHash().ToString();

    if(nTimestamp < nNow - 2 * nSuperblockCycleSeconds) {
        LogPrintf("CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too old timestamp, masternode = %s, timestamp = %d, current time = %d\n",
                 strHash, masternodeOutpoint.ToStringShort(), nTimestamp, nNow);
        return false;
    }

    if(nTimestamp > nNow + MAX_TIME_FUTURE_DEVIATION) {
        LogPrintf("CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too new (future) timestamp, masternode = %s, timestamp = %d, current time = %d\n",
                 strHash, masternodeOutpoint.ToStringShort(), nTimestamp, nNow);
        return false;
    }

    const auto& it  = mapLastMasternodeObject.find(masternodeOutpoint);
    if(it == mapLastMasternodeObject.end())
        return true;

    if(it->second.fStatusOK && !fForce) {
        fRateCheckBypassed = true;
        return true;
    }

    // Allow 1 trigger per mn per cycle, with a small fudge factor
    double dMaxRate = 2 * 1.1 / double(nSuperblockCycleSeconds);

    // Temporary copy to check rate after new timestamp is added
    CRateCheckBuffer buffer = it->second.triggerBuffer;

    buffer.AddTimestamp(nTimestamp);
    double dRate = buffer.GetRate();

    if(dRate < dMaxRate) {
        return true;
    }

    LogPrintf("CGovernanceManager::MasternodeRateCheck -- Rate too high: object hash = %s, masternode = %s, object timestamp = %d, rate = %f, max rate = %f\n",
              strHash, masternodeOutpoint.ToStringShort(), nTimestamp, dRate, dMaxRate);

    if (fUpdateFailStatus)
        it->second.fStatusOK = false;

    return false;
}

bool CGovernanceManager::ProcessVote(CNode* pfrom, const CGovernanceVote& vote, CGovernanceException& exception, CConnman* connman)
{
    ENTER_CRITICAL_SECTION(cs);
    uint256 nHashVote = vote.GetHash();
    uint256 nHashGovobj = vote.GetParentHash();
    std::string strResult;

    if(cmapVoteToObject.HasKey(nHashVote)) {
        LogPrint(BCLog::GOV, "CGovernanceObject::ProcessVote -- skipping known valid vote %s for object %s\n", nHashVote.ToString(), nHashGovobj.ToString());
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    if(cmapInvalidVotes.HasKey(nHashVote)) {
        strResult = strprintf("CGovernanceManager::ProcessVote -- Old invalid vote, MN outpoint = "
                + vote.GetMasternodeOutpoint().ToStringShort()
                + ", funding object hash = " + nHashGovobj.ToString());
        LogPrintf("%s\n", strResult);
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    const auto& it = mapObjects.find(nHashGovobj);
    if(it == mapObjects.end()) {
        strResult = strprintf("CGovernanceManager::ProcessVote -- Unknown parent object " + nHashGovobj.ToString()
             + ", MN outpoint = "
             + vote.GetMasternodeOutpoint().ToStringShort());
        exception = CGovernanceException(strResult, GOVERNANCE_EXCEPTION_WARNING);
        if(cmmapOrphanVotes.Insert(nHashGovobj, vote_time_pair_t(vote, GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME))) {
            LEAVE_CRITICAL_SECTION(cs);
            RequestGovernanceObject(pfrom, nHashGovobj, connman);
            LogPrintf("%s\n", strResult);
            return false;
        }

        LogPrint(BCLog::GOV, "%s\n", strResult);
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    CGovernanceObject& govobj = it->second;

    if(govobj.IsSetCachedDelete() || govobj.IsSetExpired()) {
        LogPrint(BCLog::GOV, "CGovernanceObject::ProcessVote -- ignoring vote for expired or deleted object, hash = %s\n", nHashGovobj.ToString());
        LEAVE_CRITICAL_SECTION(cs);
        return false;
    }

    bool fOk = govobj.ProcessVote(pfrom, vote, exception, connman) && cmapVoteToObject.Insert(nHashVote, &govobj);
    if (fOk) {
        uiInterface.NotifyProposalChanged(govobj.GetHash(), CT_UPDATED);
    }
    LEAVE_CRITICAL_SECTION(cs);
    return fOk;
}

void CGovernanceManager::CheckMasternodeOrphanVotes(CConnman* connman)
{
    LOCK2(cs_main, cs);

    ScopedLockBool guard(cs, fRateChecksEnabled, false);

    for (auto& objPair : mapObjects) {
        objPair.second.CheckOrphanVotes(connman);
    }
}

void CGovernanceManager::CheckMasternodeOrphanObjects(CConnman* connman)
{
    LOCK2(cs_main, cs);
    int64_t nNow = GetAdjustedTime();
    ScopedLockBool guard(cs, fRateChecksEnabled, false);
    auto it = mapMasternodeOrphanObjects.begin();
    while(it != mapMasternodeOrphanObjects.end()) {
        object_info_pair_t& pair = it->second;
        CGovernanceObject& govobj = pair.first;

        if(pair.second.nExpirationTime >= nNow) {
            std::string strError;
            bool fMasternodeMissing = false;
            bool fConfirmationsMissing = false;
            bool fIsValid = govobj.IsValidLocally(strError, fMasternodeMissing, fConfirmationsMissing, true);

            if(fIsValid) {
                AddGovernanceObject(govobj, connman);
            } else if(fMasternodeMissing) {
                ++it;
                continue;
            }
        } else {
            // apply node's ban score
            Misbehaving(pair.second.idFrom, 20);
        }

        auto it_count = mapMasternodeOrphanCounter.find(govobj.GetMasternodeOutpoint());
        if(--it_count->second == 0)
            mapMasternodeOrphanCounter.erase(it_count);

        mapMasternodeOrphanObjects.erase(it++);
    }
}

void CGovernanceManager::CheckPostponedObjects(CConnman* connman)
{
    if(!masternodeSync.IsSynced()) return;

    LOCK2(cs_main, cs);

    // Check postponed proposals
    for(auto it = mapPostponedObjects.begin(); it != mapPostponedObjects.end();) {

        const uint256& nHash = it->first;
        CGovernanceObject& govobj = it->second;

        assert(govobj.GetObjectType() != GOVERNANCE_OBJECT_TRIGGER);

        std::string strError;
        bool fMissingConfirmations;
        if (govobj.IsCollateralValid(strError, fMissingConfirmations))
        {
            if(govobj.IsValidLocally(strError, false) && !fMissingConfirmations)
                AddGovernanceObject(govobj, connman);
            else
                LogPrintf("CGovernanceManager::CheckPostponedObjects -- %s invalid\n", nHash.ToString());

        } else if(fMissingConfirmations) {
            // wait for more confirmations
            ++it;
            continue;
        }

        // remove processed or invalid object from the queue
        mapPostponedObjects.erase(it++);
    }


    // Perform additional relays for triggers
    int64_t nNow = GetAdjustedTime();
    int64_t nSuperblockCycleSeconds = Params().GetConsensus().nSuperblockCycle * Params().GetConsensus().nPowTargetSpacing;

    for(auto it = setAdditionalRelayObjects.begin(); it != setAdditionalRelayObjects.end();) {

        const auto& itObject = mapObjects.find(*it);
        if(itObject != mapObjects.end()) {

            CGovernanceObject& govobj = itObject->second;

            int64_t nTimestamp = govobj.GetCreationTime();

            bool fValid = (nTimestamp <= nNow + MAX_TIME_FUTURE_DEVIATION) && (nTimestamp >= nNow - 2 * nSuperblockCycleSeconds);
            bool fReady = (nTimestamp <= nNow + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME);

            if(fValid) {
                if(fReady) {
                    LogPrintf("CGovernanceManager::CheckPostponedObjects -- additional relay: hash = %s\n", govobj.GetHash().ToString());
                    govobj.Relay(connman);
                } else {
                    it++;
                    continue;
                }
            }

        } else {
            LogPrintf("CGovernanceManager::CheckPostponedObjects -- additional relay of unknown object: %s\n", it->ToString());
        }

        setAdditionalRelayObjects.erase(it++);
    }
}

void CGovernanceManager::RequestGovernanceObject(CNode* pfrom, const uint256& nHash, CConnman* connman, bool fUseFilter)
{
    if(!pfrom) {
        return;
    }

    LogPrint(BCLog::GOV, "CGovernanceObject::RequestGovernanceObject -- hash = %s (peer=%d)\n", nHash.ToString(), pfrom->GetId());

    CNetMsgMaker msgMaker(pfrom->GetSendVersion());

    CBloomFilter filter;
    filter.clear();

    int nVoteCount = 0;
    if(fUseFilter) {
        LOCK(cs);
        CGovernanceObject* pObj = FindGovernanceObject(nHash);

        if(pObj) {
            filter = CBloomFilter(Params().GetConsensus().nGovernanceFilterElements, GOVERNANCE_FILTER_FP_RATE, GetRandInt(999999), BLOOM_UPDATE_ALL);
            std::vector<CGovernanceVote> vecVotes = pObj->GetVoteFile().GetVotes();
            nVoteCount = vecVotes.size();
            for(size_t i = 0; i < vecVotes.size(); ++i) {
                filter.insert(vecVotes[i].GetHash());
            }
        }
    }

    LogPrint(BCLog::GOV, "CGovernanceManager::RequestGovernanceObject -- nHash %s nVoteCount %d peer=%d\n", nHash.ToString(), nVoteCount, pfrom->GetId());
    connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNGOVERNANCESYNC, nHash, filter));
}

int CGovernanceManager::RequestGovernanceObjectVotes(CNode* pnode, CConnman* connman)
{
    if(pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) return -3;
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(pnode);
    return RequestGovernanceObjectVotes(vNodesCopy, connman);
}

int CGovernanceManager::RequestGovernanceObjectVotes(const std::vector<CNode*>& vNodesCopy, CConnman* connman)
{
    static std::map<uint256, std::map<CService, int64_t> > mapAskedRecently;

    if(vNodesCopy.empty()) return -1;

    int64_t nNow = GetTime();
    int nTimeout = 60 * 60;
    size_t nPeersPerHashMax = 3;

    std::vector<uint256> vTriggerObjHashes;
    std::vector<uint256> vOtherObjHashes;

    // This should help us to get some idea about an impact this can bring once deployed on mainnet.
    // Testnet is ~40 times smaller in masternode count, but only ~1000 masternodes usually vote,
    // so 1 obj on mainnet == ~10 objs or ~1000 votes on testnet. However we want to test a higher
    // number of votes to make sure it's robust enough, so aim at 2000 votes per masternode per request.
    // On mainnet nMaxObjRequestsPerNode is always set to 1.
    int nMaxObjRequestsPerNode = 1;
    size_t nProjectedVotes = 2000;
    if(Params().NetworkIDString() != CBaseChainParams::MAIN) {
        nMaxObjRequestsPerNode = std::max(1, int(nProjectedVotes / std::max(1, mnodeman.size())));
    }

    {
        LOCK2(cs_main, cs);

        if(mapObjects.empty()) return -2;

        for (const auto& objPair : mapObjects) {
            uint256 nHash = objPair.first;
            if (mapAskedRecently.count(nHash)) {
                auto it = mapAskedRecently[nHash].begin();
                while (it != mapAskedRecently[nHash].end()) {
                    if (it->second < nNow) {
                        mapAskedRecently[nHash].erase(it++);
                    } else {
                        ++it;
                    }
                }
                if (mapAskedRecently[nHash].size() >= nPeersPerHashMax) continue;
            }

            if (objPair.second.nObjectType == GOVERNANCE_OBJECT_TRIGGER) {
                vTriggerObjHashes.push_back(nHash);
            } else {
                vOtherObjHashes.push_back(nHash);
            }
        }
    }

    LogPrint(BCLog::GOV, "CGovernanceManager::RequestGovernanceObjectVotes -- start: vTriggerObjHashes %d vOtherObjHashes %d mapAskedRecently %d\n",
                vTriggerObjHashes.size(), vOtherObjHashes.size(), mapAskedRecently.size());

    Shuffle(vTriggerObjHashes.begin(), vTriggerObjHashes.end(), FastRandomContext());
    Shuffle(vOtherObjHashes.begin(), vOtherObjHashes.end(), FastRandomContext());

    for (int i = 0; i < nMaxObjRequestsPerNode; ++i) {
        uint256 nHashGovobj;

        // ask for triggers first
        if(vTriggerObjHashes.size()) {
            nHashGovobj = vTriggerObjHashes.back();
        } else {
            if(vOtherObjHashes.empty()) break;
            nHashGovobj = vOtherObjHashes.back();
        }
        bool fAsked = false;
        for (const auto& pnode : vNodesCopy) {
            // Only use regular peers, don't try to ask from outbound "masternode" connections -
            // they stay connected for a short period of time and it's possible that we won't get everything we should.
            // Only use outbound connections - inbound connection could be a "masternode" connection
            // initiated from another node, so skip it too.
            if(pnode->fMasternode || (fMasternodeMode && pnode->fInbound)) continue;
            // only use up to date peers
            if(pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) continue;
            // to early to ask the same node
            if(mapAskedRecently[nHashGovobj].count(pnode->addr)) continue;

            RequestGovernanceObject(pnode, nHashGovobj, connman, true);
            mapAskedRecently[nHashGovobj][pnode->addr] = nNow + nTimeout;
            fAsked = true;
            // stop loop if max number of peers per obj was asked
            if(mapAskedRecently[nHashGovobj].size() >= nPeersPerHashMax) break;
        }
        // NOTE: this should match `if` above (the one before `while`)
        if(vTriggerObjHashes.size()) {
            vTriggerObjHashes.pop_back();
        } else {
            vOtherObjHashes.pop_back();
        }
        if(!fAsked) i--;
    }
    LogPrint(BCLog::GOV, "CGovernanceManager::RequestGovernanceObjectVotes -- end: vTriggerObjHashes %d vOtherObjHashes %d mapAskedRecently %d\n",
                vTriggerObjHashes.size(), vOtherObjHashes.size(), mapAskedRecently.size());

    return int(vTriggerObjHashes.size() + vOtherObjHashes.size());
}

bool CGovernanceManager::VoteWithAll(const uint256& hash, const std::pair<std::string, std::string>& strVoteSignal, std::pair<int, int>& nResult, CConnman* connman)
{
    vote_signal_enum_t eVoteSignal = CGovernanceVoting::ConvertVoteSignal(strVoteSignal.first);
    vote_outcome_enum_t eVoteOutcome = CGovernanceVoting::ConvertVoteOutcome(strVoteSignal.second);

    for (const auto& mne : masternodeConfig.getEntries()) {
        CPubKey pubKeyMasternode;
        CKey keyMasternode;

        if(!CMessageSigner::GetKeysFromSecret(mne.getPrivKey(), keyMasternode, pubKeyMasternode)){
            nResult.first++;
            continue;
         }

        uint256 nTxHash;
        nTxHash.SetHex(mne.getTxHash());

        uint32_t nOutputIndex = 0;
        if(!ParseUInt32(mne.getOutputIndex(), &nOutputIndex)) {
            continue;
        }

        COutPoint outpoint(nTxHash, nOutputIndex);

        CMasternode mn;
        bool fMnFound = mnodeman.Get(outpoint, mn);

        if(!fMnFound) {
            nResult.first++;
            continue;
        }

        CGovernanceVote vote(mn.outpoint, hash, eVoteSignal, eVoteOutcome);
        if(!vote.Sign(keyMasternode, pubKeyMasternode)){
            nResult.first++;
            continue;
        }

        CGovernanceException exception;
        if(funding.ProcessVoteAndRelay(vote, exception, connman)) {
            nResult.second++;
        }
        else {
            nResult.first++;
        }
    }
    return true;
}

bool CGovernanceManager::AcceptObjectMessage(const uint256& nHash)
{
    LOCK(cs);
    return AcceptMessage(nHash, setRequestedObjects);
}

bool CGovernanceManager::AcceptVoteMessage(const uint256& nHash)
{
    LOCK(cs);
    return AcceptMessage(nHash, setRequestedVotes);
}

bool CGovernanceManager::AcceptMessage(const uint256& nHash, std::set<uint256>& setHash)
{
    const auto& it = setHash.find(nHash);
    if(it == setHash.end()) {
        // We never requested this
        return false;
    }
    // Only accept one response
    setHash.erase(it);
    return true;
}

void CGovernanceManager::RebuildIndexes()
{
    LOCK(cs);

    cmapVoteToObject.Clear();
    for (auto& objPair : mapObjects) {
        CGovernanceObject& govobj = objPair.second;
        std::vector<CGovernanceVote> vecVotes = govobj.GetVoteFile().GetVotes();
        for(size_t i = 0; i < vecVotes.size(); ++i) {
            cmapVoteToObject.Insert(vecVotes[i].GetHash(), &govobj);
        }
    }
}

void CGovernanceManager::AddCachedTriggers()
{
    LOCK(cs);

    for (auto& objpair : mapObjects) {
        CGovernanceObject& govobj = objpair.second;

        if(govobj.nObjectType != GOVERNANCE_OBJECT_TRIGGER) {
            continue;
        }

        if (!triggerman.AddNewTrigger(govobj.GetHash())) {
            govobj.fCachedDelete = true;
            if (govobj.nDeletionTime == 0) {
                govobj.nDeletionTime = GetAdjustedTime();
            }
        }
    }
}

void CGovernanceManager::InitOnLoad()
{
    LOCK(cs);
    int64_t nStart = GetTimeMillis();
    LogPrintf("Preparing masternode indexes and funding triggers...\n");
    RebuildIndexes();
    AddCachedTriggers();
    LogPrintf("Masternode indexes and funding triggers prepared  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("     %s\n", ToString());
}

std::string CGovernanceManager::ToString() const
{
    LOCK(cs);

    int nProposalCount = 0;
    int nTriggerCount = 0;
    int nOtherCount = 0;

    for (const auto& objPair : mapObjects) {
        switch(objPair.second.GetObjectType()) {
            case GOVERNANCE_OBJECT_PROPOSAL:
                nProposalCount++;
                break;
            case GOVERNANCE_OBJECT_TRIGGER:
                nTriggerCount++;
                break;
            default:
                nOtherCount++;
                break;
        }
    }

    return strprintf("Governance Objects: %d (Proposals: %d, Triggers: %d, Other: %d; Erased: %d), Votes: %d",
                    (int)mapObjects.size(),
                     nProposalCount, nTriggerCount, nOtherCount, (int)mapErasedGovernanceObjects.size(),
                    (int)cmapVoteToObject.GetSize());
}

UniValue CGovernanceManager::ToJson() const
{
    LOCK(cs);

    int nProposalCount = 0;
    int nTriggerCount = 0;
    int nOtherCount = 0;

    for (const auto& objpair : mapObjects) {
        switch(objpair.second.GetObjectType()) {
            case GOVERNANCE_OBJECT_PROPOSAL:
                nProposalCount++;
                break;
            case GOVERNANCE_OBJECT_TRIGGER:
                nTriggerCount++;
                break;
            default:
                nOtherCount++;
                break;
        }
    }

    UniValue jsonObj(UniValue::VOBJ);
    jsonObj.pushKV("objects_total", (int)mapObjects.size());
    jsonObj.pushKV("proposals", nProposalCount);
    jsonObj.pushKV("triggers", nTriggerCount);
    jsonObj.pushKV("other", nOtherCount);
    jsonObj.pushKV("erased", (int)mapErasedGovernanceObjects.size());
    jsonObj.pushKV("votes", (int)cmapVoteToObject.GetSize());
    return jsonObj;
}

void CGovernanceManager::UpdatedBlockTip(const CBlockIndex *pindexNew, bool fInitialDownload, CConnman* connman)
{
    if(!pindexNew || fLiteMode || fInitialDownload) {
        return;
    }

    nCachedBlockHeight = pindexNew->nHeight;
    LogPrint(BCLog::GOV, "CGovernanceManager::UpdatedBlockTip -- nCachedBlockHeight: %d\n", nCachedBlockHeight);

    CheckPostponedObjects(connman);

    CSuperblockManager::ExecuteBestSuperblock(pindexNew->nHeight);
}

void CGovernanceManager::RequestOrphanObjects(CConnman* connman)
{
    std::vector<CNode*> vNodesCopy = connman->CopyNodeVector();

    std::vector<uint256> vecHashesFiltered;
    {
        std::vector<uint256> vecHashes;
        LOCK(cs);
        cmmapOrphanVotes.GetKeys(vecHashes);
        for(size_t i = 0; i < vecHashes.size(); ++i) {
            const uint256& nHash = vecHashes[i];
            if(mapObjects.find(nHash) == mapObjects.end()) {
                vecHashesFiltered.push_back(nHash);
            }
        }
    }

    LogPrint(BCLog::GOV, "CGovernanceObject::RequestOrphanObjects -- number objects = %d\n", vecHashesFiltered.size());
    for(size_t i = 0; i < vecHashesFiltered.size(); ++i) {
        const uint256& nHash = vecHashesFiltered[i];
        for(size_t j = 0; j < vNodesCopy.size(); ++j) {
            CNode* pnode = vNodesCopy[j];
            if(pnode->fMasternode) {
                continue;
            }
            RequestGovernanceObject(pnode, nHash, connman);
        }
    }
    connman->ReleaseNodeVector(vNodesCopy);
}

void CGovernanceManager::CleanOrphanObjects()
{
    LOCK(cs);
    const auto& items = cmmapOrphanVotes.GetItemList();

    int64_t nNow = GetAdjustedTime();

    CacheMultiMap<uint256, vote_time_pair_t>::list_cit it = items.begin();
    while(it != items.end()) {
        const auto& prevIt = it;
        ++it;
        const auto& pairVote = prevIt->value;
        if(pairVote.second < nNow) {
            cmmapOrphanVotes.Erase(prevIt->key, prevIt->value);
        }
    }
}

void CGovernanceManager::Controller(CScheduler& scheduler, CConnman* connman)
{
    if (!fLiteMode) {
        scheduler.scheduleEvery(std::bind(&CGovernanceManager::ClientTask, this, connman), 60000*5);
    }
}
