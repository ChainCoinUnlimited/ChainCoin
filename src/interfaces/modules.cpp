// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/modules.h>
#include <modules/platform/funding.h>
#include <modules/masternode/masternode_man.h>
#include <modules/masternode/masternode_payments.h>
#include <modules/masternode/masternode_sync.h>
#include <modules/coinjoin/coinjoin.h>
#include <modules/coinjoin/coinjoin_server.h>

void ModuleInterface::InitializeCurrentBlockTip()
{
    LOCK(cs_main);
    UpdatedBlockTip(::ChainActive().Tip(), nullptr, IsInitialBlockDownload());
}

void ModuleInterface::ProcessModuleMessage(CNode* pfrom, const NetMsgDest& dest, const std::string& strCommand, CDataStream& vRecv)
{
    CDataStream ss(vRecv);

    switch (dest) {
    case NetMsgDest::MSG_NONE:
        return;
    case NetMsgDest::MSG_FUND:
        funding.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        return;
    case NetMsgDest::MSG_MN_MAN:
        mnodeman.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        return;
    case NetMsgDest::MSG_MN_SYNC:
        masternodeSync.ProcessModuleMessage(pfrom, strCommand, ss);
        return;
    case NetMsgDest::MSG_MN_PAY:
        mnpayments.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        return;
    case NetMsgDest::MSG_PSEND:
        coinJoinServer.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        return;
    case NetMsgDest::MSG_ALL:
        funding.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        mnodeman.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        masternodeSync.ProcessModuleMessage(pfrom, strCommand, ss);
        mnpayments.ProcessModuleMessage(pfrom, strCommand, ss, connman);
        coinJoinServer.ProcessModuleMessage(pfrom, strCommand, ss, connman);
    }
}

void ModuleInterface::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    if (pindexNew == pindexFork) // blocks were disconnected without any new ones
        return;

    masternodeSync.UpdatedBlockTip(pindexNew, fInitialDownload, connman);

    if (fLiteMode || fInitialDownload) return;

    coinJoinServer.UpdatedBlockTip(pindexNew);
    mnodeman.UpdatedBlockTip(pindexNew);
    mnpayments.UpdatedBlockTip(pindexNew, fInitialDownload, connman);
    funding.UpdatedBlockTip(pindexNew, fInitialDownload, connman);
}

