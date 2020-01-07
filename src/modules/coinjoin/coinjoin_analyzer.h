// Copyright (c) 2014-2017 The CoinJoin! developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODULES_COINJOIN_COINJOIN_ANALYZER_H
#define BITCOIN_MODULES_COINJOIN_COINJOIN_ANALYZER_H

#include <primitives/transaction.h>
#include <sync.h>

#include <map>
#include <vector>

typedef std::map<uint256, std::vector<std::pair<CTxOut, int> > > m_cache;

class CAnalyzer
{
private:
    m_cache mDenomTx;
    RecursiveMutex cs;

    static const std::string SERIALIZATION_VERSION_STRING;

    /** Recursively calculate the depth of obscuring a single outpoint. */
    bool FindRoot(const COutPoint& outpoint, std::vector<int>& vRoots, int nDepth = 2);

public:
    CAnalyzer() {}
    virtual ~CAnalyzer() {}

    void Clear()
    {
        LOCK(cs);
        mDenomTx.clear();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING;
            READWRITE(strVersion);
        }
        READWRITE(mDenomTx);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
            return;
        }
    }

    /** Return the average CoinJoin depth of an outpoint. */
    int AnalyzeCoin(const COutPoint& outpoint);

    /** Write cache. */
    void WriteCache();

    /** Read and update cache. */
    void ReadCache();

    /** Remove spent UTXOs from the cache. */
    void Flush();
};

/// The global transaction analyzer. May be null.
extern std::unique_ptr<CAnalyzer> g_analyzer;

#endif //BITCOIN_MODULES_COINJOIN_COINJOIN_ANALYZER_H
