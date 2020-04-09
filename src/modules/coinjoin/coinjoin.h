// Copyright (c) 2014-2017 The CoinJoin! developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODULES_COINJOIN_COINJOIN_H
#define BITCOIN_MODULES_COINJOIN_COINJOIN_H

#include <chain.h>
#include <chainparams.h>
#include <primitives/transaction.h>
#include <psbt.h>
#include <pubkey.h>
#include <sync.h>
#include <timedata.h>
#include <tinyformat.h>

class CCoinJoin;
class CConnman;
class CNode;

// denominations
static const unsigned char COINJOIN_MAX_SHIFT = 0x0b;
static const CAmount COINJOIN_BASE_DENOM = 102400000;
// boundaries for convenience
static const CAmount COINJOIN_HIGH_DENOM = COINJOIN_BASE_DENOM << COINJOIN_MAX_SHIFT;
static const CAmount COINJOIN_LOW_DENOM = COINJOIN_BASE_DENOM >> COINJOIN_MAX_SHIFT;

// time for all participants to sign
static const int COINJOIN_SIGNING_TIMEOUT        = 30;
// timeout for nodes to submit their tx
static const int COINJOIN_ACCEPT_TIMEOUT         = 60;
// timeout for queues in blocks
static const int COINJOIN_DEFAULT_TIMEOUT        = 4;

//! minimum peer version accepted by mixing pool
static const int MIN_COINJOIN_PEER_PROTO_VERSION            = 70017;
//! maximum number of inputs on a single pool transaction
static const size_t COINJOIN_ENTRY_MAX_SIZE                 = 135;
//! number of denoms each size before new ones are created
static const unsigned int COINJOIN_DENOM_THRESHOLD          = 3;
//! number of denoms each size before new ones are created
static const unsigned int COINJOIN_FEE_DENOM_THRESHOLD      = 9;
//! number of denoms each size before new ones are created
static const unsigned int COINJOIN_DENOM_WINDOW             = 3;

//! depth boundaries
static const int MIN_COINJOIN_DEPTH              = 1;
static const int DEFAULT_COINJOIN_DEPTH          = 2;
static const int MAX_COINJOIN_DEPTH              = 3;

// pool responses
enum PoolMessage {
    ERR_ALREADY_HAVE,
    ERR_DENOM,
    ERR_ENTRIES_FULL,
    ERR_INVALID_OUT,
    ERR_MN_FEES,
    ERR_INVALID_INPUT,
    ERR_FEES,
    ERR_INVALID_TX,
    ERR_MAXIMUM,
    ERR_MN_LIST,
    ERR_MODE,
    ERR_QUEUE_FULL,
    ERR_RECENT,
    ERR_SESSION,
    ERR_MISSING_TX,
    ERR_VERSION,
    MSG_NOERR,
    MSG_SUCCESS,
    MSG_ENTRIES_ADDED,
    MSG_POOL_MIN = ERR_ALREADY_HAVE,
    MSG_POOL_MAX = MSG_ENTRIES_ADDED
};

// pool states
enum PoolState {
    POOL_STATE_IDLE,
    POOL_STATE_CONNECTING,
    POOL_STATE_QUEUE,
    POOL_STATE_ACCEPTING_ENTRIES,
    POOL_STATE_SIGNING,
    POOL_STATE_ERROR,
    POOL_STATE_SUCCESS,
    POOL_STATE_MIN = POOL_STATE_IDLE,
    POOL_STATE_MAX = POOL_STATE_SUCCESS
};

// status update message constants
enum PoolStatusUpdate {
    STATUS_CLOSED,
    STATUS_OPEN,
    STATUS_READY,
    STATUS_FULL,
    STATUS_REJECTED,
    STATUS_ACCEPTED
};


// A clients transaction in the mixing pool
class CCoinJoinEntry
{
public:
    int nSessionID;
    PartiallySignedTransaction psbtx;
    // mem only
    CService addr;

    CCoinJoinEntry() :
        nSessionID(0),
        psbtx(PartiallySignedTransaction()),
        addr(CService())
    {
    }

    CCoinJoinEntry(const int& nSessionID, const PartiallySignedTransaction& psbtx) :
        nSessionID(nSessionID),
        psbtx(psbtx),
        addr(CService())
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(nSessionID);
        READWRITE(psbtx);
    }

    friend bool operator==(const CCoinJoinEntry& a, const CCoinJoinEntry& b)
    {
        return a.nSessionID == b.nSessionID && a.psbtx.tx->GetHash() == b.psbtx.tx->GetHash();
    }

};


/**
 * A currently in progress mixing merge and denomination information
 */
class CCoinJoinQueue
{
public:
    CAmount nDenom;
    COutPoint masternodeOutpoint;
    int nHeight;
    PoolStatusUpdate status;
    std::vector<unsigned char> vchSig;
    // memory only
    bool fTried;

    CCoinJoinQueue() :
        nDenom(0),
        masternodeOutpoint(COutPoint()),
        nHeight(0),
        status(STATUS_CLOSED),
        vchSig(std::vector<unsigned char>()),
        fTried(false)
    {
    }

    CCoinJoinQueue(CAmount _nDenom, COutPoint _outpoint, int _nHeight, PoolStatusUpdate _status) :
        nDenom(_nDenom),
        masternodeOutpoint(_outpoint),
        nHeight(_nHeight),
        status(_status),
        vchSig(std::vector<unsigned char>()),
        fTried(false)
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int statusInt = status;
        READWRITE(nDenom);
        READWRITE(masternodeOutpoint);
        READWRITE(nHeight);
        READWRITE(statusInt);
        status = static_cast<PoolStatusUpdate>(statusInt);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
    }

    uint256 GetSignatureHash() const;
    /** Sign this mixing transaction
     *  \return true if all conditions are met:
     *     1) we have an active Masternode,
     *     2) we have a valid Masternode private key,
     *     3) we signed the message successfully, and
     *     4) we verified the message successfully
     */
    bool Sign();
    /// Check if we have a valid Masternode address
    bool CheckSignature(const CPubKey& pubKeyMasternode) const;

    bool Relay();
    bool Push(const CService pto, CConnman* connman);

    /// Is this queue expired?
    bool IsExpired(int nHeightIn) const { return nHeightIn - nHeight > COINJOIN_DEFAULT_TIMEOUT; }
    bool IsOpen() const { return status > 0; }

    std::string ToString() const
    {
        return strprintf("nDenom=%d, nHeight=%lld, status=%d, fTried=%s, masternode=%s",
            nDenom, nHeight, status, fTried ? "true" : "false", masternodeOutpoint.ToStringShort());
    }

    friend bool operator==(const CCoinJoinQueue& a, const CCoinJoinQueue& b)
    {
        return a.masternodeOutpoint == b.masternodeOutpoint && a.status == b.status;
    }
    friend bool operator!=(const CCoinJoinQueue& a, const CCoinJoinQueue& b)
    {
        return a.masternodeOutpoint == b.masternodeOutpoint && a.status != b.status;
    }
};

/** Helper class to store mixing transaction (tx) information.
 */
class CCoinJoinBroadcastTx
{

public:
    int nSessionID;
    PartiallySignedTransaction psbtx;
    COutPoint masternodeOutpoint;
    std::vector<unsigned char> vchSig;
    int64_t sigTime;

    CCoinJoinBroadcastTx() :
        nSessionID(0),
        psbtx(),
        masternodeOutpoint(),
        vchSig(),
        sigTime(0)
    {
    }

    CCoinJoinBroadcastTx(const int& _nSessionID, const PartiallySignedTransaction& _psbtx, COutPoint _outpoint, int64_t _sigTime) :
        nSessionID(_nSessionID),
        psbtx(_psbtx),
        masternodeOutpoint(_outpoint),
        vchSig(),
        sigTime(_sigTime)
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(nSessionID);
        READWRITE(psbtx);
        READWRITE(masternodeOutpoint);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
        READWRITE(sigTime);
    }

    friend bool operator==(const CCoinJoinBroadcastTx& a, const CCoinJoinBroadcastTx& b)
    {
        return *a.psbtx.tx == *b.psbtx.tx;
    }
    friend bool operator!=(const CCoinJoinBroadcastTx& a, const CCoinJoinBroadcastTx& b)
    {
        return !(a == b);
    }
    explicit operator bool() const
    {
        return *this != CCoinJoinBroadcastTx();
    }

    uint256 GetSignatureHash() const;

    bool Sign();
    bool CheckSignature(const CPubKey& pubKeyMasternode) const;
};

// base class
class CCoinJoinBaseSession
{
protected:
    mutable CCriticalSection cs_coinjoin;

    std::vector<CCoinJoinEntry> vecEntries; // Masternode/clients entries

    PoolState nState;               // should be one of the POOL_STATE_XXX values
    int64_t nTimeStart;      // for accepting entries and signing

    int nSessionID; // 0 if no mixing session is active

    PartiallySignedTransaction finalPartiallySignedTransaction; // the finalized transaction ready for signing

    void SetNull();

public:
    CAmount nSessionDenom; //Users must submit at least one denom matching this

    CCoinJoinBaseSession() :
        vecEntries(0),
        nState(POOL_STATE_IDLE),
        nTimeStart(0),
        nSessionID(0),
        finalPartiallySignedTransaction(PartiallySignedTransaction()),
        nSessionDenom(0)
    {
    }

    int GetState() const { return nState; }
    std::string GetStateString() const;

    bool CheckTransaction(PartiallySignedTransaction &psbtxIn, CAmount& nFeeRet, PoolMessage& errRet, bool fUnsigned);

    int GetEntriesCount() const { return vecEntries.size(); }
};

// base class
class CCoinJoinBaseManager
{
protected:
    mutable CCriticalSection cs_vecqueue;

    // The current mixing sessions in progress on the network
    std::vector<CCoinJoinQueue> vecCoinJoinQueue GUARDED_BY(cs_vecqueue);

    void SetNull();
    void CheckQueue(int nHeight);

public:
    CCoinJoinBaseManager() :
        vecCoinJoinQueue(0) {}

    int GetQueueSize() const {
        LOCK(cs_vecqueue);
        return vecCoinJoinQueue.size();
    }
    bool GetQueueItem(CCoinJoinQueue& queueRet);
};

// helper class
class CCoinJoin
{
private:
    // make constructor, destructor and copying not available
    CCoinJoin() {}
    ~CCoinJoin() {}
    CCoinJoin(CCoinJoin const&) = delete;
    CCoinJoin& operator= (CCoinJoin const&) = delete;

public:
    static bool IsDenominatedAmount(const CAmount& nInputAmount);
    static CAmount GetDenomRange();
    static bool IsInDenomRange(const CAmount& nAmount);

    static std::string GetDenominationsToString(const CAmount& nDenom);

    static std::string GetMessageByID(PoolMessage nMessageID);

    /// Get the maximum number of transactions for the pool
    static unsigned int GetMaxPoolInputs() { return Params().PoolMaxInputs(); }
    static unsigned int GetMinPoolInputs() { return Params().PoolMinInputs(); }

    static CAmount GetMaxPoolAmount() { return COINJOIN_ENTRY_MAX_SIZE * COINJOIN_HIGH_DENOM; }
};

#endif //BITCOIN_MODULES_COINJOIN_COINJOIN_H
