// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODULES_PLATFORM_FUNDING_H
#define BITCOIN_MODULES_PLATFORM_FUNDING_H

#include <bloom.h>
#include <cachemap.h>
#include <cachemultimap.h>
#include <chain.h>
#include <modules/platform/funding_exceptions.h>
#include <modules/platform/funding_object.h>
#include <modules/platform/funding_vote.h>
#include <net.h>
#include <sync.h>
#include <timedata.h>
#include <univalue.h>

#include <boost/signals2/signal.hpp>

class CGovernanceManager;
class CGovernanceTriggerManager;
class CGovernanceObject;
class CGovernanceVote;

extern CGovernanceManager governance;

struct ExpirationInfo {
    ExpirationInfo(int64_t _nExpirationTime, int _idFrom) : nExpirationTime(_nExpirationTime), idFrom(_idFrom) {}

    int64_t nExpirationTime;
    NodeId idFrom;
};

typedef std::pair<CGovernanceObject, ExpirationInfo> object_info_pair_t;

static const int RATE_BUFFER_SIZE = 5;

class CRateCheckBuffer
{
private:
    std::vector<int64_t> vecTimestamps;

    int nDataStart;

    int nDataEnd;

    bool fBufferEmpty;

public:
    CRateCheckBuffer()
        : vecTimestamps(RATE_BUFFER_SIZE),
          nDataStart(0),
          nDataEnd(0),
          fBufferEmpty(true)
        {}

    void AddTimestamp(int64_t nTimestamp)
    {
        if((nDataEnd == nDataStart) && !fBufferEmpty) {
            // Buffer full, discard 1st element
            nDataStart = (nDataStart + 1) % RATE_BUFFER_SIZE;
        }
        vecTimestamps[nDataEnd] = nTimestamp;
        nDataEnd = (nDataEnd + 1) % RATE_BUFFER_SIZE;
        fBufferEmpty = false;
    }

    int64_t GetMinTimestamp()
    {
        int nIndex = nDataStart;
        int64_t nMin = std::numeric_limits<int64_t>::max();
        if(fBufferEmpty) {
            return nMin;
        }
        do {
            if(vecTimestamps[nIndex] < nMin) {
                nMin = vecTimestamps[nIndex];
            }
            nIndex = (nIndex + 1) % RATE_BUFFER_SIZE;
        } while(nIndex != nDataEnd);
        return nMin;
    }

    int64_t GetMaxTimestamp()
    {
        int nIndex = nDataStart;
        int64_t nMax = 0;
        if(fBufferEmpty) {
            return nMax;
        }
        do {
            if(vecTimestamps[nIndex] > nMax) {
                nMax = vecTimestamps[nIndex];
            }
            nIndex = (nIndex + 1) % RATE_BUFFER_SIZE;
        } while(nIndex != nDataEnd);
        return nMax;
    }

    int GetCount()
    {
        int nCount = 0;
        if(fBufferEmpty) {
            return 0;
        }
        if(nDataEnd > nDataStart) {
            nCount = nDataEnd - nDataStart;
        }
        else {
            nCount = RATE_BUFFER_SIZE - nDataStart + nDataEnd;
        }

        return nCount;
    }

    double GetRate()
    {
        int nCount = GetCount();
        if(nCount < RATE_BUFFER_SIZE) {
            return 0.0;
        }
        int64_t nMin = GetMinTimestamp();
        int64_t nMax = GetMaxTimestamp();
        if(nMin == nMax) {
            // multiple objects with the same timestamp => infinite rate
            return 1.0e10;
        }
        return double(nCount) / double(nMax - nMin);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(vecTimestamps);
        READWRITE(nDataStart);
        READWRITE(nDataEnd);
        READWRITE(fBufferEmpty);
    }
};

//
// Governance Manager : Contains all proposals for the budget
//
class CGovernanceManager
{
    friend class CGovernanceObject;

public: // Types
    struct last_object_rec {
        last_object_rec(bool fStatusOKIn = true)
            : triggerBuffer(),
              fStatusOK(fStatusOKIn)
            {}

        ADD_SERIALIZE_METHODS;

        template <typename Stream, typename Operation>
        inline void SerializationOp(Stream& s, Operation ser_action)
        {
            READWRITE(triggerBuffer);
            READWRITE(fStatusOK);
        }

        CRateCheckBuffer triggerBuffer;
        bool fStatusOK;
    };

private:
    static const int MAX_CACHE_SIZE = 1000000;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int MAX_TIME_FUTURE_DEVIATION;
    static const int RELIABLE_PROPAGATION_TIME;

    int64_t nTimeLastDiff;

    // keep track of current block height
    int nCachedBlockHeight;

    // keep track of the scanning errors
    std::map<uint256, CGovernanceObject> mapObjects;

    // mapErasedGovernanceObjects contains key-value pairs, where
    //   key   - governance object's hash
    //   value - expiration time for deleted objects
    std::map<uint256, int64_t> mapErasedGovernanceObjects;

    std::map<uint256, object_info_pair_t> mapMasternodeOrphanObjects;
    std::map<COutPoint, int> mapMasternodeOrphanCounter;

    std::map<uint256, CGovernanceObject> mapPostponedObjects;
    std::set<uint256> setAdditionalRelayObjects;

    CacheMap<uint256, CGovernanceObject*> cmapVoteToObject;

    CacheMap<uint256, CGovernanceVote> cmapInvalidVotes;

    CacheMultiMap<uint256, vote_time_pair_t> cmmapOrphanVotes;

    std::map<COutPoint, last_object_rec> mapLastMasternodeObject;

    std::set<uint256> setRequestedObjects;

    std::set<uint256> setRequestedVotes;

    bool fRateChecksEnabled;

    class ScopedLockBool
    {
        bool& ref;
        bool fPrevValue;

    public:
        ScopedLockBool(CCriticalSection& _cs, bool& _ref, bool _value) : ref(_ref)
        {
            AssertLockHeld(_cs);
            fPrevValue = ref;
            ref = _value;
        }

        ~ScopedLockBool()
        {
            ref = fPrevValue;
        }
    };

public:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    CGovernanceManager();

    virtual ~CGovernanceManager() {}

    /**
     * This is called by AlreadyHave in net_processing.cpp as part of the inventory
     * retrieval process.  Returns true if we want to retrieve the object, otherwise
     * false. (Note logic is inverted in AlreadyHave).
     */
    bool ConfirmInventoryRequest(const CInv& inv);

    void SyncSingleObjAndItsVotes(CNode* pnode, const uint256& nProp, const CBloomFilter& filter, CConnman* connman);
    void SyncAll(CNode* pnode, CConnman* connman) const;

    void ClientTask(CConnman* connman);

    void Controller(CScheduler& scheduler, CConnman* connman);

    CGovernanceObject* FindGovernanceObject(const uint256& nHash);

    // These commands are only used in RPC
    std::vector<CGovernanceVote> GetMatchingVotes(const uint256& nParentHash) const;
    std::vector<CGovernanceVote> GetCurrentVotes(const uint256& nParentHash, const COutPoint& mnCollateralOutpointFilter) const;
    std::vector<const CGovernanceObject*> GetAllNewerThan(int64_t nMoreThanTime) const;

    void AddGovernanceObject(CGovernanceObject& govobj, CConnman* connman, CNode* pfrom = nullptr);

    void UpdateCachesAndClean();

    void ProcessModuleMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);

    void UpdatedBlockTip(const CBlockIndex *pindexNew, bool fInitialDownload, CConnman* connman);

    void Clear()
    {
        LOCK(cs);

        LogPrint(BCLog::GOV, "Governance object manager was cleared\n");
        mapObjects.clear();
        mapErasedGovernanceObjects.clear();
        cmapVoteToObject.Clear();
        cmapInvalidVotes.Clear();
        cmmapOrphanVotes.Clear();
        mapLastMasternodeObject.clear();
    }

    std::string ToString() const;
    UniValue ToJson() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING;
            READWRITE(strVersion);
        }
        READWRITE(mapErasedGovernanceObjects);
        READWRITE(cmapInvalidVotes);
        READWRITE(cmmapOrphanVotes);
        READWRITE(mapObjects);
        READWRITE(mapLastMasternodeObject);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
            return;
        }
    }

    int64_t GetLastDiffTime() const { return nTimeLastDiff; }
    void UpdateLastDiffTime(int64_t nTimeIn) { nTimeLastDiff = nTimeIn; }

    int GetCachedBlockHeight() const { return nCachedBlockHeight; }

    // Accessors for thread-safe access to maps
    bool HaveObjectForHash(const uint256& nHash) const;

    bool HaveVoteForHash(const uint256& nHash) const;

    int GetVoteCount() const;

    bool SerializeObjectForHash(const uint256& nHash, CDataStream& ss) const;

    bool SerializeVoteForHash(const uint256& nHash, CDataStream& ss) const;

    void AddPostponedObject(const CGovernanceObject& govobj)
    {
        LOCK(cs);
        mapPostponedObjects.insert(std::make_pair(govobj.GetHash(), govobj));
    }

    void AddSeenGovernanceObject(const uint256& nHash, int status);

    void AddSeenVote(const uint256& nHash, int status);

    void MasternodeRateUpdate(const CGovernanceObject& govobj);

    bool MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus = false);

    bool MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus, bool fForce, bool& fRateCheckBypassed);

    bool ProcessVoteAndRelay(const CGovernanceVote& vote, CGovernanceException& exception, CConnman* connman) {
        bool fOK = ProcessVote(nullptr, vote, exception, connman);
        if(fOK) {
            vote.Relay(connman);
        }
        return fOK;
    }

    void CheckMasternodeOrphanVotes(CConnman* connman);

    void CheckMasternodeOrphanObjects(CConnman* connman);

    void CheckPostponedObjects(CConnman* connman);

    bool AreRateChecksEnabled() const {
        LOCK(cs);
        return fRateChecksEnabled;
    }

    void InitOnLoad();

    int RequestGovernanceObjectVotes(CNode* pnode, CConnman* connman);
    int RequestGovernanceObjectVotes(const std::vector<CNode*>& vNodesCopy, CConnman* connman);

    bool VoteWithAll(const uint256& hash, const std::pair<std::string, std::string>& strVoteSignal, std::pair<int, int>& nResult, CConnman* connman);

private:
    void RequestGovernanceObject(CNode* pfrom, const uint256& nHash, CConnman* connman, bool fUseFilter = false);

    void AddInvalidVote(const CGovernanceVote& vote)
    {
        cmapInvalidVotes.Insert(vote.GetHash(), vote);
    }

    void AddOrphanVote(const CGovernanceVote& vote)
    {
        cmmapOrphanVotes.Insert(vote.GetHash(), vote_time_pair_t(vote, GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME));
    }

    bool ProcessVote(CNode* pfrom, const CGovernanceVote& vote, CGovernanceException& exception, CConnman* connman);

    /// Called to indicate a requested object has been received
    bool AcceptObjectMessage(const uint256& nHash);

    /// Called to indicate a requested vote has been received
    bool AcceptVoteMessage(const uint256& nHash);

    static bool AcceptMessage(const uint256& nHash, std::set<uint256>& setHash);

    void CheckOrphanVotes(CGovernanceObject& govobj, CGovernanceException& exception, CConnman* connman);

    void RebuildIndexes();

    void AddCachedTriggers();

    void RequestOrphanObjects(CConnman* connman);

    void CleanOrphanObjects();

};

#endif
