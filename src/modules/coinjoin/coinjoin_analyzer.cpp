// Copyright (c) 2014-2017 The CoinJoin! developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <modules/coinjoin/coinjoin.h>
#include <modules/coinjoin/coinjoin_analyzer.h>
#include <util/time.h>
#include <validation.h>

#include <numeric>

std::unique_ptr<CAnalyzer> g_analyzer;

const std::string CAnalyzer::SERIALIZATION_VERSION_STRING = "CAnalyzer-Version-1";

int CAnalyzer::AnalyzeCoin(const COutPoint& outpoint)
{
    uint256 hash = outpoint.hash;
    unsigned int nout = outpoint.n;

    LOCK(cs);

    // return early if we have it
    m_cache::iterator mdwi = mDenomTx.find(hash);

    if (mdwi != mDenomTx.end() && mdwi->second[nout].first.nDepth != -10) {
        return mdwi->second[nout].first.nDepth;
    }

    CTransactionRef tx;
    uint256 hash_block;

    if(GetTransaction(hash, tx, Params().GetConsensus(), hash_block))
    {
        if (mdwi == mDenomTx.end()) {
            // not known yet, let's add it
            LogPrint(BCLog::CJOIN, "[chain] AnalyzeCoin INSERTING %s\n", hash.ToString());
            std::vector<std::pair<CTxOut, int> > cache;
            for (const auto& out : tx->vout) {
                cache.emplace_back(std::make_pair(out, -10));
            }
            mDenomTx.emplace(hash, cache);
        }

        mdwi = mDenomTx.find(hash);

        //make sure the final output is non-denominate
        if (!CCoinJoin::IsDenominatedAmount(mdwi->second[nout].first.nValue)) { //NOT DENOM
            mdwi->second[nout].first.nDepth = -2;
            LogPrint(BCLog::CJOIN, "[chain] AnalyzeCoin UPDATED to -2   %s %3d %3d\n", hash.ToString(), nout, mdwi->second[nout].first.nDepth);
            return mdwi->second[nout].first.nDepth;
        }

        bool fAllDenoms = true;
        for (const auto& out : tx->vout) {
            fAllDenoms = fAllDenoms && CCoinJoin::IsDenominatedAmount(out.nValue);
        }

        // this one is denominated but there is another non-denominated output found in the same tx
        if (!fAllDenoms) {
            mdwi->second[nout].first.nDepth = 0;
            LogPrint(BCLog::CJOIN, "[chain] AnalyzeCoin UPDATED to  0   %s %3d %3d\n", hash.ToString(), nout, mdwi->second[nout].first.nDepth);
            return mdwi->second[nout].first.nDepth;
        }

        // only denoms here so let's look up
        std::vector<int> roots;
        roots.clear();
        const int64_t analyze_tx_start_time = GetTimeMillis();

        for (const auto& txinNext : tx->vin) {
            uint256 hashNext = txinNext.prevout.hash;
            unsigned int noutNext = txinNext.prevout.n;
            m_cache::iterator mdwiNext = mDenomTx.find(hashNext);
            if (mdwiNext != mDenomTx.end() && mdwiNext->second[noutNext].first.nDepth >= 0) {
                roots.push_back(mdwiNext->second[noutNext].first.nDepth + 1);
            } else {
                if (!FindRoot(txinNext.prevout, roots)) roots.push_back(1);
            }
        }

        mdwi->second[nout].first.nDepth = std::accumulate(roots.begin(), roots.end(), int64_t(0)) / roots.size();
        LogPrint(BCLog::CJOIN, "[chain] AnalyzeCoin UPDATED as analyzed   %s %3d %3d analyze %7dms\n", hash.ToString(), nout, mdwi->second[nout].first.nDepth, GetTimeMillis() - analyze_tx_start_time);
        return mdwi->second[nout].first.nDepth;
    }

    return 1;
}

bool CAnalyzer::FindRoot(const COutPoint& outpoint, std::vector<int>& vRoots, int nDepth)
{
    if(nDepth >= MAX_COINJOIN_DEPTH) {
        // limit the depth of analysis
        return false;
    }

    static std::map<uint256, std::pair<CMutableTransaction, bool> > mDenomTxCache;

    uint256 hash = outpoint.hash;

    std::map<uint256, std::pair<CMutableTransaction, bool> >::const_iterator mdwi = mDenomTxCache.find(hash);
    if (mdwi == mDenomTxCache.end()) {
        CTransactionRef tx;
        uint256 hash_block;
        if(GetTransaction(hash, tx, Params().GetConsensus(), hash_block))
            mDenomTxCache.emplace(hash, std::make_pair(CMutableTransaction(*tx), false));
        else return false;
    } else if (!mDenomTxCache[hash].second) return false;

    if (!mDenomTxCache[hash].second) {
        bool fAllDenoms = true;
        for (const auto& out : mDenomTxCache[hash].first.vout) {
            fAllDenoms = fAllDenoms && CCoinJoin::IsDenominatedAmount(out.nValue);
            if (!fAllDenoms) break;
        }

        // this one is denominated but there is another non-denominated output found in the same tx
        if (fAllDenoms) {
            mDenomTxCache[hash].second = true;
        } else {
            return false;
        }
    }

    nDepth++;

    // only denoms here so let's look up
    for (const auto& txinNext : mDenomTxCache[hash].first.vin) {
        if (!FindRoot(txinNext.prevout, vRoots, nDepth)) {
            vRoots.push_back(nDepth);
        }
    }
    if (!vRoots.empty()) return true;
    else return false;
}

void CAnalyzer::Flush()
{
    LOCK2(cs_main, cs);
    for (m_cache::iterator it = mDenomTx.begin(); it != mDenomTx.end(); ++it) {
        const Coin& coin = AccessByTxid(::ChainstateActive().CoinsTip(), it->first);
        if (coin.IsSpent()) {
            mDenomTx.erase(it--);
        }
    }
}

void CAnalyzer::ReadCache()
{
    LOCK(cs);
    for (auto& it : mDenomTx) {
        for (auto& it2 : it.second) {
            it2.first.nDepth = it2.second;
            LogPrint(BCLog::CJOIN, "[chain] ReadCache %s cache: %d nDepth: %d\n", it.first.ToString(), it2.second, it2.first.nDepth);
        }
    }

}

void CAnalyzer::WriteCache()
{
    LOCK(cs);
    for (auto& it : mDenomTx) {
        for (auto& it2 : it.second) {
            it2.second = it2.first.nDepth;
            LogPrint(BCLog::CJOIN, "[chain] WriteCache %s cache: %d nDepth: %d\n", it.first.ToString(), it2.second, it2.first.nDepth);
        }
    }

}
