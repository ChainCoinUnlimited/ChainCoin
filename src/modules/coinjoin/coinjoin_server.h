// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODULES_COINJOIN_COINJOINSERVER_H
#define BITCOIN_MODULES_COINJOIN_COINJOINSERVER_H

#include <net.h>
#include <modules/coinjoin/coinjoin.h>

class CCoinJoinServer;

// The main object for accessing mixing
extern CCoinJoinServer coinJoinServer;

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinServer : public CCoinJoinBaseSession, public CCoinJoinBaseManager
{
private:
    std::vector<std::pair<CService, CAmount> > vecDenom;
    CCoinJoinQueue activeQueue;

    bool fUnitTest;

    // Keep track of current block height
    int nCachedBlockHeight;

    /// Add a clients entry to the pool
    bool AddEntry(const CCoinJoinEntry& entryNew, PoolMessage& nMessageIDRet);

    /// Charge fees to bad actors (Charge clients a fee if they're abusive)
    // void BanAbusive(CConnman* connman);

    /// Check for process
    void CheckPool(CConnman* connman);

    /// Check Session
    bool CheckSessionMessage(CNode* pnode, CConnman* connman);

    void CreateFinalTransaction(CConnman* connman);
    void CommitFinalTransaction(CConnman* connman);

    bool CreateNewSession(const CAmount& nDenom, PoolMessage &nMessageIDRet);
    bool AddUserToExistingSession(const CAmount& nDenom, PoolMessage &nMessageIDRet);
    /// Do we have enough users to take entries?
    bool IsSessionReady() { return vecDenom.size() >= CCoinJoin::GetMinPoolInputs(); }
    bool IsSessionClosed() { return vecDenom.size() >= CCoinJoin::GetMaxPoolInputs() - 1; }
    bool IsSessionFull() { return vecDenom.size() >= CCoinJoin::GetMaxPoolInputs(); }

    /// Are these outputs compatible with other client in the pool?
    bool IsCompatibleTxOut(const CMutableTransaction mtx, CAmount& nMNfee);

    // Set the 'state' value, with some logging and capturing when the state changed
    void SetState(PoolState nStateNew);

    /// Relay mixing Messages
    void RelayFinalTransaction(const PartiallySignedTransaction &txFinal, CConnman* connman);
    void PushStatus(CNode* pnode, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID, CConnman* connman);
    void RelayStatus(PoolStatusUpdate nStatusUpdate, CConnman* connman, PoolMessage nMessageID = MSG_NOERR);
    void RelayCompletedTransaction(PoolMessage nMessageID, CConnman* connman);

    void UpdateQueue(PoolStatusUpdate update);
    void SetNull();

public:
    CCoinJoinServer() :
        vecDenom(),
        activeQueue(),
        fUnitTest(false),
        nCachedBlockHeight(0)
        { SetNull(); }

    void ProcessModuleMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman* connman);
    void CheckTimeout(int nHeight);
    void CheckForCompleteQueue();
    void UpdatedBlockTip(const CBlockIndex *pindexNew);
};

#endif  //BITCOIN_MODULES_COINJOIN_COINJOINSERVER_H
