// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/modules.h>
#include <modules/platform/funding.h>
#include <modules/masternode/masternode_man.h>
#include <modules/masternode/masternode_payments.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/privatesend/privatesend.h>
#include <modules/privatesend/privatesend_server.h>

void ModuleInterface::InitializeCurrentBlockTip()
{
    LOCK(cs_main);
    UpdatedBlockTip(chainActive.Tip(), nullptr, IsInitialBlockDownload());
}

void ModuleInterface::ProcessModuleMessage(CNode* pfrom, const NetMsgDest &dest, const std::string& strCommand, CDataStream& vRecv, CConnman* connman)
{
    switch (dest) {
    case NetMsgDest::MSG_FUND:
        governance.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
    case NetMsgDest::MSG_MN_MAN:
        mnodeman.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
    case NetMsgDest::MSG_MN_SYNC:
        masternodeSync.ProcessModuleMessage(pfrom, strCommand, vRecv);
    case NetMsgDest::MSG_MN_PAY:
        mnpayments.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
    case NetMsgDest::MSG_PSEND:
        privateSendServer.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
    case NetMsgDest::MSG_ALL:
        governance.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
        mnodeman.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
        masternodeSync.ProcessModuleMessage(pfrom, strCommand, vRecv);
        mnpayments.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
        privateSendServer.ProcessModuleMessage(pfrom, strCommand, vRecv, connman);
    }
}

void ModuleInterface::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    if (pindexNew == pindexFork) // blocks were disconnected without any new ones
        return;

    masternodeSync.UpdatedBlockTip(pindexNew, fInitialDownload, connman);

    if (fInitialDownload || fLiteMode)
        return;

    mnodeman.UpdatedBlockTip(pindexNew);
    CPrivateSend::UpdatedBlockTip(pindexNew);
    mnpayments.UpdatedBlockTip(pindexNew, fInitialDownload, connman);
    governance.UpdatedBlockTip(pindexNew, fInitialDownload, connman);
}

void ModuleInterface::TransactionAddedToMempool(const CTransactionRef &tx)
{
    CPrivateSend::SyncTransaction(tx, nullptr);
}

void ModuleInterface::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock)
{
    for (const CTransactionRef& ptx : pblock->vtx) {
        // Do a normal sync for each transaction removed in block disconnection
        CPrivateSend::SyncTransaction(ptx, nullptr);
    }
}
