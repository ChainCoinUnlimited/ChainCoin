// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_MODULES_H
#define BITCOIN_INTERFACES_MODULES_H

#include <validationinterface.h>

enum class NetMsgDest
{
    MSG_NONE = 0,
    MSG_ALL,
    MSG_MN_MAN,
    MSG_MN_PAY,
    MSG_MN_SYNC,
    MSG_PSEND,
    MSG_FUND
};


class ModuleInterface : public CValidationInterface {
public:
    ModuleInterface(CConnman* _connman): connman(_connman) {}

    virtual ~ModuleInterface() {}

    // a small helper to initialize current block height in sub-modules on startup
    void InitializeCurrentBlockTip();

protected:
    // CValidationInterface
    void ProcessModuleMessage(CNode* pfrom, const NetMsgDest& dest, const std::string& strCommand, CDataStream& vRecv) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;

private:
    CConnman* connman;
};

#endif // BITCOIN_MODULEINTERFACE_H
