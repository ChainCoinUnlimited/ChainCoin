// Copyright (c) 2019 The CoinJoin! developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_COINJOIN_CLIENT_H
#define BITCOIN_WALLET_COINJOIN_CLIENT_H

#include <interfaces/chain.h>
#include <modules/masternode/masternode.h>
#include <modules/coinjoin/coinjoin.h>

class CCoinJoinClientManager;
class CReserveKey;
class CWallet;

static const int MIN_COINJOIN_AMOUNT             = 2;
static const int MIN_COINJOIN_LIQUIDITY          = 0;
static const int MAX_COINJOIN_SESSIONS           = 21;
static const int MAX_COINJOIN_AMOUNT             = MAX_MONEY / COIN;
static const int MAX_COINJOIN_LIQUIDITY          = 100;
static const int DEFAULT_COINJOIN_AMOUNT         = 1000;
static const int DEFAULT_COINJOIN_LIQUIDITY      = 0;

// Warn user if mixing in gui or try to create backup if mixing in daemon mode
// when we have only this many keys left
static const int COINJOIN_KEYS_THRESHOLD_WARNING = 100;
// Stop mixing completely, it's too dangerous to continue when we have only this many keys left
static const int COINJOIN_KEYS_THRESHOLD_STOP    = 50;

class CKeyHolderStorage
{
private:
    std::vector<std::shared_ptr<CReserveKey> > storage;
    mutable CCriticalSection cs_storage;

public:
    void AddKey(CScript &script, CWallet* pwalletIn);
    void KeepAll();
    void ReturnAll();

};

class CPendingCJaRequest
{
private:
    static const int TIMEOUT = 90;

    CService addr;
    CAmount nDenom;
    int64_t nTimeCreated;

public:
    CPendingCJaRequest():
        addr(CService()),
        nDenom(0),
        nTimeCreated(0)
    {
    }

    CPendingCJaRequest(const CService& addr_, const CAmount& nDenom_):
        addr(addr_),
        nDenom(nDenom_),
        nTimeCreated(GetTime())
    {
    }

    CService GetAddr() { return addr; }
    CAmount GetDenom() { return nDenom; }
    bool IsExpired() { return GetTime() - nTimeCreated > TIMEOUT; }

    friend bool operator==(const CPendingCJaRequest& a, const CPendingCJaRequest& b)
    {
        return a.addr == b.addr && a.nDenom == b.nDenom;
    }
    friend bool operator!=(const CPendingCJaRequest& a, const CPendingCJaRequest& b)
    {
        return !(a == b);
    }

    explicit operator bool() const
    {
        return *this != CPendingCJaRequest();
    }
};

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinClientSession : public CCoinJoinBaseSession
{
private:
    CWallet* m_wallet_session;

    CMutableTransaction mtxSession; // clients entries

    int nEntriesCount;
    bool fLastEntryAccepted;
    std::vector<COutPoint> vecOutPointLocked;

    std::string strLastMessage;
    std::string strAutoCoinJoinResult;

    masternode_info_t infoMixingMasternode;
    CPendingCJaRequest pendingCJaRequest;
    CKeyHolderStorage keyHolderStorage;

    const bool fMixingOnly;

    bool JoinExistingQueue();
    bool StartNewQueue();

    /// Get Masternode updates about the progress of mixing
    bool CheckPoolStateUpdate(PoolState nStateNew, int nEntriesCountNew, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID, int nSessionIDNew = 0);
    // Set the 'state' value, with some logging and capturing when the state changed
    void SetState(PoolState nStateNew);

    /// Create the transaction we want to include in the mixing pool
    bool CreateSessionTransaction(std::vector<std::pair<CTxIn, CTxOut> >& vecPair, CAmount& nDenom, std::vector<CAmount>& vecAmounts);

    /// Add some fees
    bool AddFeesAndLocktime(std::vector<CAmount>& vecAmounts);

    void CompletedTransaction(PoolMessage nMessageID);

    /// As a client, check and sign the final transaction
    bool SignFinalTransaction(PartiallySignedTransaction& finalTransactionNew, CNode* pnode);

    void RelayIn(const CCoinJoinEntry& entry);

    void UnlockCoins();

public:
    explicit CCoinJoinClientSession(CWallet* pwallet, bool _fMixingOnly) :
        m_wallet_session(pwallet),
        mtxSession(CMutableTransaction()),
        nEntriesCount(0),
        fLastEntryAccepted(false),
        vecOutPointLocked(0),
        strLastMessage(strprintf("none")),
        strAutoCoinJoinResult(strprintf("Initialized")),
        infoMixingMasternode(),
        pendingCJaRequest(CPendingCJaRequest()),
        keyHolderStorage(),
        fMixingOnly(_fMixingOnly)
    {
    }

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);

    std::string GetStatus(bool fWaitForBlock);

    bool GetMixingMasternodeInfo(masternode_info_t& mnInfoRet) const;

    /// Passively run mixing in the background according to the configuration in settings
    void CoinJoin(std::vector<std::pair<CTxIn, CTxOut> >& vecPair, std::vector<CAmount>& vecAmounts);

    /// As a client, submit part of a future mixing transaction to a Masternode to start the process
    bool SendDenominate();

    bool ProcessPendingCJaRequest(CConnman* connman);

    void SetError() { nState = POOL_STATE_ERROR; }

    void SetNull();

    /// Check for process
    bool PoolStateManager();
};

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinClientManager : public CCoinJoinBaseManager
{
private:
    CWallet* m_wallet;

    // Keep track of the used Masternodes
    std::vector<COutPoint> vecMasternodesUsed;

    std::deque<CCoinJoinClientSession> deqSessions;
    mutable CCriticalSection cs_deqsessions;

    int nCachedLastSuccessBlock;
    int nMinBlocksToWait;
    std::string strAutoCoinJoinResult;

    // Keep track of current block height
    int nCachedBlockHeight;

    void UnlockCoins();

    /// Create denominations
    bool CreateDenominated(const CAmount& nValue, std::vector<CAmount>& vecAmounts);

    /// Check if mixing is needed
    bool IsMixingRequired(std::vector<std::pair<CTxIn, CTxOut> >& portfolio, std::vector<CAmount>& vecAmounts, bool& fMixOnly);

    bool WaitForAnotherBlock();

    // Make sure we have enough keys since last backup
    bool CheckAutomaticBackup();

public:
    std::atomic_bool fStartup;
    std::atomic_bool fActive;
    int nCoinJoinDepth;
    int nCoinJoinAmount;
    int nLiquidityProvider;
    bool fEnableCoinJoin;
    std::vector<COutPoint> vecOutPointLocked;

    int nCachedNumBlocks; //used for the overview screen
    bool fCreateAutoBackups; //builtin support for automatic backups

    explicit CCoinJoinClientManager(CWallet* pwallet) :
        m_wallet(pwallet),
        vecMasternodesUsed(0),
        deqSessions(),
        nCachedLastSuccessBlock(0),
        nMinBlocksToWait(1),
        strAutoCoinJoinResult(strprintf("Initialized")),
        nCachedBlockHeight(0),
        fStartup(false),
        fActive(false),
        nCoinJoinDepth(DEFAULT_COINJOIN_DEPTH),
        nCoinJoinAmount(DEFAULT_COINJOIN_AMOUNT),
        nLiquidityProvider(DEFAULT_COINJOIN_LIQUIDITY),
        fEnableCoinJoin(false),
        vecOutPointLocked(0),
        nCachedNumBlocks(std::numeric_limits<int>::max()),
        fCreateAutoBackups(true)
    {
    }

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);

    void SetMinBlocksToWait(int nMinBlocksToWaitIn) { nMinBlocksToWait = nMinBlocksToWaitIn; }

    void ResetPool();

    std::string GetStatuses();
    std::string GetSessionDenoms();

    bool GetMixingMasternodesInfo(std::vector<masternode_info_t>& vecMnInfoRet) const;

    /// Passively run mixing in the background according to the configuration in settings
    void CoinJoin();

    void ProcessPendingCJaRequest();

    void AddUsedMasternode(const COutPoint& outpointMn);
    masternode_info_t GetNotUsedMasternode();

    void UpdatedSuccessBlock();

    void CheckResult(int nHeight);

    void UpdatedBlockTip(const int nHeight);
    void ClientTask();
};

#endif // BITCOIN_WALLET_COINJOIN_CLIENT_H
