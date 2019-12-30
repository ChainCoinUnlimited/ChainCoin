// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CACHEDB_H
#define BITCOIN_CACHEDB_H

#include <fs.h>
#include <net_types.h> // For banmap_t
#include <serialize.h>

#include <string>
#include <map>

class CSubNet;
class CAddrMan;
class CAnalyzer;
class CMasternodeMan;
class CGovernanceManager;
class CNetFulfilledRequestManager;
class CMasternodePayments;

class CDataStream;

typedef enum BanReason
{
    BanReasonUnknown          = 0,
    BanReasonNodeMisbehaving  = 1,
    BanReasonManuallyAdded    = 2
} BanReason;

class CBanEntry
{
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    int64_t nCreateTime;
    int64_t nBanUntil;
    uint8_t banReason;

    CBanEntry()
    {
        SetNull();
    }

    explicit CBanEntry(int64_t nCreateTimeIn)
    {
        SetNull();
        nCreateTime = nCreateTimeIn;
    }

    explicit CBanEntry(int64_t n_create_time_in, BanReason ban_reason_in) : CBanEntry(n_create_time_in)
    {
        banReason = ban_reason_in;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(this->nVersion);
        READWRITE(nCreateTime);
        READWRITE(nBanUntil);
        READWRITE(banReason);
    }

    void SetNull()
    {
        nVersion = CBanEntry::CURRENT_VERSION;
        nCreateTime = 0;
        nBanUntil = 0;
        banReason = BanReasonUnknown;
    }

    std::string banReasonToString() const
    {
        switch (banReason) {
        case BanReasonNodeMisbehaving:
            return "node misbehaving";
        case BanReasonManuallyAdded:
            return "manually added";
        default:
            return "unknown";
        }
    }
};

/** Access to the (IP) address database (peers.dat) */
class CAddrDB
{
private:
    fs::path pathAddr;
public:
    CAddrDB();
    bool Write(const CAddrMan& addr);
    bool Read(CAddrMan& addr);
    static bool Read(CAddrMan& addr, CDataStream& ssPeers);
};

/** Access to the banlist database (banlist.dat) */
class CBanDB
{
private:
    const fs::path m_ban_list_path;
public:
    explicit CBanDB(fs::path ban_list_path);
    bool Write(const banmap_t& banSet);
    bool Read(banmap_t& banSet);
};

// Chaincoin specific cache files

/** Access to the mncache database (mncache.dat) */
class CMNCacheDB
{
private:
    fs::path pathMNCache;
public:
    CMNCacheDB();
    bool Write(const CMasternodeMan& mncache);
    bool Read(CMasternodeMan& mncache);
};

/** Access to the mnpayments database (mnpayments.dat) */
class CMNPayDB
{
private:
    fs::path pathMNPay;
public:
    CMNPayDB();
    bool Write(const CMasternodePayments& mnpayments);
    bool Read(CMasternodePayments& mnpayments);
};

/** Access to the funding database (funding.dat) */
class CGovDB
{
private:
    fs::path pathGovernance;
public:
    CGovDB();
    bool Write(const CGovernanceManager& funding);
    bool Read(CGovernanceManager& funding);
};

/** Access to the netfulfilled database (netfulfilled.dat) */
class CNetFulDB
{
private:
    fs::path pathNetfulfilled;
public:
    CNetFulDB();
    bool Write(const CNetFulfilledRequestManager& netfulfilled);
    bool Read(CNetFulfilledRequestManager& netfulfilled);
};

/** Access to the CoinJoin! database (coinjoin.dat) */
class CCoinJoinDB
{
private:
    fs::path pathCoinJoin;
public:
    CCoinJoinDB();
    bool Write(const CAnalyzer& coinjoin);
    bool Read(CAnalyzer& coinjoin);
};

#endif // BITCOIN_CACHEDB_H
