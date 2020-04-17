// Copyright (c) 2018-2020 PM-Tech
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODULES_MASTERNODE_MASTERNODE_CONFIG_H
#define BITCOIN_MODULES_MASTERNODE_MASTERNODE_CONFIG_H

#include <key_io.h>

#include <string>
#include <vector>

class CMasternodeConfig;

extern CMasternodeConfig masternodeConfig;

struct MasternodeEntry
{
    std::string alias;
    std::string ip;
    CKey privKey;
    COutPoint outpoint;

    MasternodeEntry() {}
    MasternodeEntry(std::string _alias, std::string _ip, CKey _privKey, COutPoint _outpoint)
        : alias(_alias)
        , ip(_ip)
        , privKey(_privKey)
        , outpoint(_outpoint)
    {}

    const std::string& getAlias() const {
        return alias;
    }

    const std::string& getIp() const {
        return ip;
    }

    const COutPoint& getOutPoint() const {
        return outpoint;
    }

    const CKey& getPrivKey() const {
        return privKey;
    }
};

class CMasternodeConfig
{

public:
    CMasternodeConfig() {
        entries = std::vector<MasternodeEntry>();
    }

    void clear();
    bool read(std::string& strErrRet);
    void add(const std::string& alias, const std::string& ip, const std::string& privKey, const std::string& txHash, const std::string& outputIndex);

    std::vector<MasternodeEntry>& getEntries() {
        return entries;
    }

    int getCount() {
        return (int)entries.size();
    }

private:
    std::vector<MasternodeEntry> entries;
};


#endif /* SRC_MASTERNODECONFIG_H_ */
