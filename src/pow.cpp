// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2018-2022 John "ComputerCraftr" Studnicka
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <mutex>

static Mutex cs_target_cache;

// peercoin: find last block index up to pindex
static inline const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, const bool fProofOfStake)
{
    while (pindex && pindex->IsProofOfStake() != fProofOfStake && pindex->pprev) {
        pindex = pindex->pprev;
    }
    return pindex;
}

static inline const CBlockIndex* GetLastBlockIndexForAlgo(const CBlockIndex* pindex, const int algo)
{
    while (pindex && CBlockHeader::GetAlgoType(pindex->nVersion) != algo && pindex->pprev) {
        pindex = pindex->pprev;
    }
    return pindex;
}

static inline const CBlockIndex* GetASERTReferenceBlockForAlgo(const CBlockIndex* pindex, const int nASERTStartHeight, const int algo)
{
    if (!pindex)
        return pindex;

    while (pindex->nHeight >= nASERTStartHeight) {
        const CBlockIndex* pprev = GetLastBlockIndexForAlgo(pindex->pprev, algo);
        if (pprev) {
            pindex = pprev;
        } else {
            break;
        }
    }
    return pindex;
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    const int algo = CBlockHeader::GetAlgoType(pblock->nVersion);
    const uint32_t nProofOfWorkLimit = UintToArith256(params.powLimit[algo == -1 ? CBlockHeader::AlgoType::ALGO_POW_SHA256 : algo]).GetCompactBase256();
    if (pindexLast == nullptr || params.fPowNoRetargeting)
        return nProofOfWorkLimit;

    if (params.fPowAllowMinDifficultyBlocks && algo != -1) {
        // Special difficulty rule:
        // If the new block's timestamp is more than 30 minutes (be careful to ensure this is at least twice the actual PoW target spacing to avoid interfering with retargeting)
        // then allow mining of a min-difficulty block.
        const CBlockIndex* pindexPrev = GetLastBlockIndexForAlgo(pindexLast, algo);
        if (pindexPrev->nHeight > 10 && pblock->GetBlockTime() > pindexPrev->GetBlockTime() + (30*60))
            return (nProofOfWorkLimit - 1);
        if (pindexPrev->pprev && pindexPrev->nBits == (nProofOfWorkLimit - 1)) {
            // Return the block before the last non-special-min-difficulty-rules-block
            const CBlockIndex* pindex = pindexPrev;
            while (pindex->pprev && (pindex->nBits == (nProofOfWorkLimit - 1) || CBlockHeader::GetAlgoType(pindex->nVersion) != algo))
                pindex = pindex->pprev;
            const CBlockIndex* pprev = GetLastBlockIndexForAlgo(pindex->pprev, algo);
            if (pprev && pprev->nHeight > 10) {
                // Don't return pprev->nBits if it is another min-difficulty block; instead return pindex->nBits
                if (pprev->nBits != (nProofOfWorkLimit - 1))
                    return pprev->nBits;
                else
                    return pindex->nBits;
            }
        }
    }

    return AverageTargetASERT(pindexLast, pblock, params);
}

unsigned int GetNextWorkRequiredXEP(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit[CBlockHeader::AlgoType::ALGO_POW_SHA256]).GetCompactBase256();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit[CBlockHeader::AlgoType::ALGO_POW_SHA256]);
    arith_uint256 bnNew;
    bnNew.SetCompactBase256(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompactBase256();
}

unsigned int WeightedTargetExponentialMovingAverage(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    const int algo = CBlockHeader::GetAlgoType(pblock->nVersion);
    const bool fAlgoMissing = algo == -1;
    const bool fProofOfStake = pblock->IsProofOfStake();
    const arith_uint256 bnPowLimit = fAlgoMissing ? UintToArith256(params.powLimit[fProofOfStake ? CBlockHeader::AlgoType::ALGO_POS : CBlockHeader::AlgoType::ALGO_POW_SHA256]) : UintToArith256(params.powLimit[algo]);
    const uint32_t nProofOfWorkLimit = bnPowLimit.GetCompactBase256();
    if (pindexLast == nullptr)
        return nProofOfWorkLimit; // genesis block

    const CBlockIndex* pindexPrev = fAlgoMissing ? GetLastBlockIndex(pindexLast, fProofOfStake) : GetLastBlockIndexForAlgo(pindexLast, algo);
    if (pindexPrev->pprev == nullptr)
        return nProofOfWorkLimit; // first block

    const CBlockIndex* pindexPrevPrev = fAlgoMissing ? GetLastBlockIndex(pindexPrev->pprev, fProofOfStake) : GetLastBlockIndexForAlgo(pindexPrev->pprev, algo);
    if (pindexPrevPrev->pprev == nullptr)
        return nProofOfWorkLimit; // second block

    int nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime(); // Difficulty for PoW and PoS are calculated separately

    arith_uint256 bnNew;
    bnNew.SetCompactBase256(pindexPrev->nBits);
    int nTargetSpacing = params.nPowTargetSpacing;
    const uint32_t nTargetTimespan = params.nPowTargetTimespan;
    if (!fProofOfStake) {
        nTargetSpacing = 10 * 60; // PoW spacing is 10 minutes
    }
    const int nInterval = nTargetTimespan / (nTargetSpacing * 2); // alpha_reciprocal = (N(SMA) + 1) / 2 for same "center of mass" as SMA

    const uint32_t numerator = std::max((nInterval - 1) * nTargetSpacing + nActualSpacing, 1);
    const uint32_t denominator = nInterval * nTargetSpacing;

    // Keep in mind the order of operations and integer division here - this is why the *= operator cannot be used, as it could cause overflow or integer division to occur
    arith_uint512 bnNew512 = arith_uint512(bnNew) * numerator / denominator; // For WTEMA: next_target = prev_target * (nInterval - 1 + prev_solvetime/target_solvetime) / nInterval
    bnNew = bnNew512.trim256();

    if (bnNew512 > arith_uint512(bnPowLimit) || bnNew == arith_uint256())
        return nProofOfWorkLimit;

    return bnNew.GetCompactRoundedBase256();
}

unsigned int AverageTargetASERT(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    const int algo = CBlockHeader::GetAlgoType(pblock->nVersion);
    const bool fAlgoMissing = algo == -1;
    const bool fProofOfStake = pblock->IsProofOfStake();
    const arith_uint256 bnPowLimit = fAlgoMissing ? UintToArith256(params.powLimit[fProofOfStake ? CBlockHeader::AlgoType::ALGO_POS : CBlockHeader::AlgoType::ALGO_POW_SHA256]) : UintToArith256(params.powLimit[algo]);
    const uint32_t nProofOfWorkLimit = bnPowLimit.GetCompactBase256();
    uint32_t nTargetSpacing = params.nPowTargetSpacing;
    if (!fProofOfStake) {
        nTargetSpacing = 10 * 60; // PoW spacing is 10 minutes
    }

    if (pindexLast == nullptr)
        return nProofOfWorkLimit; // genesis block

    const CBlockIndex* pindexPrev = fAlgoMissing ? GetLastBlockIndex(pindexLast, fProofOfStake) : GetLastBlockIndexForAlgo(pindexLast, algo);
    if (pindexPrev->pprev == nullptr)
        return nProofOfWorkLimit; // first block

    const CBlockIndex* pindexPrevPrev = fAlgoMissing ? GetLastBlockIndex(pindexPrev->pprev, fProofOfStake) : GetLastBlockIndexForAlgo(pindexPrev->pprev, algo);
    if (pindexPrevPrev->pprev == nullptr)
        return nProofOfWorkLimit; // second block

    constexpr uint32_t nASERTStartHeight = 0;
    // In the future, it may be a good idea to switch this from height based to a fixed time window
    const uint32_t nASERTBlockTargetsToAverage = 4 * params.nPowTargetTimespan / nTargetSpacing; // Average the past 2 days' worth of block targets

    const uint32_t nHeight = pindexLast->nHeight + 1;
    if (nHeight < nASERTStartHeight)
        return WeightedTargetExponentialMovingAverage(pindexLast, pblock, params);

    const uint32_t nBlocksPassed = (fProofOfStake ? pindexLast->nHeightPoS : pindexLast->nHeightPoW) + 1; // Account for the ASERT reference block (when it is the genesis block at height 0) by adding one to the height

    // Using a static variable concurrently in this context is safe and will not cause a race condition during initialization because C++11 guarantees that static variables will be initialized exactly once
    static const CBlockIndex* const pindexReferenceBlocks[CBlockHeader::AlgoType::ALGO_COUNT] = {
        GetASERTReferenceBlockForAlgo(pindexPrev, nASERTStartHeight, CBlockHeader::AlgoType::ALGO_POS),
        GetASERTReferenceBlockForAlgo(pindexPrev, nASERTStartHeight, CBlockHeader::AlgoType::ALGO_POW_SHA256),
    };

    const CBlockIndex* const pindexReferenceBlock = fAlgoMissing ? pindexReferenceBlocks[fProofOfStake ? CBlockHeader::AlgoType::ALGO_POS : CBlockHeader::AlgoType::ALGO_POW_SHA256] : pindexReferenceBlocks[algo];
    const CBlockIndex* const pindexReferenceBlockPrev = fAlgoMissing ? GetLastBlockIndex(pindexReferenceBlock->pprev, fProofOfStake) : GetLastBlockIndexForAlgo(pindexReferenceBlock->pprev, algo);

    // Use reference block's parent block's timestamp unless it is the genesis (not using the prev timestamp here would put us permanently one block behind schedule)
    int64_t refBlockTimestamp = pindexReferenceBlockPrev ? pindexReferenceBlockPrev->GetBlockTime() : (pindexReferenceBlock->GetBlockTime() - nTargetSpacing);

    // The reference timestamp must be divisible by (nStakeTimestampMask+1) or else the PoS block emission will never be exactly on schedule
    if (fProofOfStake) {
        while ((refBlockTimestamp & params.nStakeTimestampMask) != 0)
            refBlockTimestamp++;
    }

    const int64_t nTimeDiff = pindexPrev->GetBlockTime() - refBlockTimestamp;
    const uint32_t nHeightDiff = nBlocksPassed;
    arith_uint256 refBlockTarget;

    // We don't want to recalculate the average of several days' worth of block targets here every single time, so instead we cache the average and start height
    constexpr bool fUseCache = true;
    {
        LOCK(cs_target_cache);

        static arith_uint256 refBlockTargetCache GUARDED_BY(cs_target_cache);
        static int nTargetCacheHeight GUARDED_BY(cs_target_cache) = -2;
        static int nTargetCacheAlgo GUARDED_BY(cs_target_cache) = CBlockHeader::AlgoType::ALGO_COUNT;
        static uint256 nTargetCacheHeightHash GUARDED_BY(cs_target_cache);

        const uint32_t nBlocksToSkip = nHeightDiff % nASERTBlockTargetsToAverage;
        const CBlockIndex* pindex = pindexPrev;

        // Get last block index in averaging window
        for (unsigned int i = 0; i < nBlocksToSkip; i++) {
            pindex = fAlgoMissing ? GetLastBlockIndex(pindex->pprev, fProofOfStake) : GetLastBlockIndexForAlgo(pindex->pprev, algo);
        }

        if (pindex && nASERTBlockTargetsToAverage > 0 && nHeight >= nASERTStartHeight + nASERTBlockTargetsToAverage && nHeightDiff >= nASERTBlockTargetsToAverage) {
            const CBlockIndex* pindexWindowEnd = pindex;

            if (!fUseCache || nTargetCacheHeight != static_cast<int>(pindexWindowEnd->nHeight) || nTargetCacheAlgo != algo || nTargetCacheHeightHash != pindexWindowEnd->GetBlockHash() ||
                refBlockTargetCache == arith_uint256() || fAlgoMissing) {
                for (int i = 0; i < static_cast<int>(nASERTBlockTargetsToAverage); i++) {
                    // Don't add min difficulty targets to the average
                    if (pindex->nBits != (nProofOfWorkLimit - 1) || !params.fPowAllowMinDifficultyBlocks) {
                        arith_uint256 bnTarget = arith_uint256().SetCompactBase256(pindex->nBits);
                        refBlockTarget += bnTarget / nASERTBlockTargetsToAverage;
                    } else {
                        // Average one more block to make up for the one we skipped
                        --i;
                    }

                    pindex = fAlgoMissing ? GetLastBlockIndex(pindex->pprev, fProofOfStake) : GetLastBlockIndexForAlgo(pindex->pprev, algo);
                    if (!pindex) {
                        // If we break here due to reaching the genesis, then it will count as averaging zeroes for the number of blocks skipped which lowers the target/increases difficulty
                        break;
                    }
                }
                if (fUseCache) {
                    refBlockTargetCache = refBlockTarget;
                    nTargetCacheHeight = pindexWindowEnd->nHeight;
                    nTargetCacheAlgo = algo;
                    nTargetCacheHeightHash = pindexWindowEnd->GetBlockHash();
                }
            } else {
                refBlockTarget = refBlockTargetCache;
            }
        } else {
            if (fUseCache && !fAlgoMissing) {
                if (nTargetCacheHeight != -1 || nTargetCacheAlgo != algo || nTargetCacheHeightHash != uint256() || refBlockTargetCache == arith_uint256()) {
                    refBlockTargetCache = arith_uint256().SetCompactBase256(pindexReferenceBlock->nBits);
                    nTargetCacheHeight = -1;
                    nTargetCacheAlgo = algo;
                    nTargetCacheHeightHash = uint256();
                }
                refBlockTarget = refBlockTargetCache;
            } else
                refBlockTarget = arith_uint256().SetCompactBase256(pindexReferenceBlock->nBits);
        }
    }

    arith_uint256 bnNew(refBlockTarget);
    const int64_t dividend = nTimeDiff - nTargetSpacing * nHeightDiff;
    const bool fPositive = dividend >= 0;
    const uint32_t divisor = params.nPowTargetTimespan; // Must be positive
    const int exponent = dividend / divisor; // Note: this integer division rounds down positive and rounds up negative numbers via truncation, but the truncated fractional part is handled by the approximation below
    const uint32_t remainder = (fPositive ? dividend : -dividend) % divisor; // Must be positive
    // We are using uint512 rather than uint64_t here because a nPowTargetTimespan of more than 3 days in the divisor may cause the following cubic approximation to overflow a uint64_t
    arith_uint512 numerator(1);
    arith_uint512 denominator(1);
    if (fPositive) {
        if (exponent > 0) {
            // Left shifting the numerator is equivalent to multiplying it by a power of 2
            numerator <<= exponent;
        }

        if (remainder != 0) { // Approximate 2^x with (4x^3+11x^2+35x+50)/50 for 0<x<1 (must be equal to 1 at x=0 and equal to 2 at x=1 to avoid discontinuities) - note: x+1 and (3x^2+7x+10)/10 are also decent and less complicated approximations
            const arith_uint512 bnDivisor(divisor);
            const arith_uint512 bnRemainder(remainder);
            numerator = numerator * ((4 * bnRemainder*bnRemainder*bnRemainder) + (11 * bnRemainder*bnRemainder * bnDivisor) + (35 * bnRemainder * bnDivisor*bnDivisor) + (50 * bnDivisor*bnDivisor*bnDivisor));
            denominator = denominator * (50 * bnDivisor*bnDivisor*bnDivisor);
        }
    } else {
        if (exponent < 0) {
            // Left shifting the denominator is equivalent to multiplying it by a power of 2
            denominator <<= -exponent;
        }

        if (remainder != 0) { // Approximate 2^x with (4x^3+11x^2+35x+50)/50 for 0<x<1 (must be equal to 1 at x=0 and equal to 2 at x=1 to avoid discontinuities) - note: x+1 and (3x^2+7x+10)/10 are also decent and less complicated approximations
            const arith_uint512 bnDivisor(divisor);
            const arith_uint512 bnRemainder(remainder);
            numerator = numerator * (50 * bnDivisor*bnDivisor*bnDivisor);
            denominator = denominator * ((4 * bnRemainder*bnRemainder*bnRemainder) + (11 * bnRemainder*bnRemainder * bnDivisor) + (35 * bnRemainder * bnDivisor*bnDivisor) + (50 * bnDivisor*bnDivisor*bnDivisor));
        }
    }

    // Keep in mind the order of operations and integer division here - this is why the *= operator cannot be used, as it could cause overflow or integer division to occur
    arith_uint512 bnNew512(arith_uint512(bnNew) * numerator / denominator);
    bnNew = bnNew512.trim256();
    if (bnNew512 > arith_uint512(bnPowLimit) || bnNew == arith_uint256())
        return nProofOfWorkLimit;

    return bnNew.GetCompactRoundedBase256();
}

bool CheckProofOfWork(const uint256& hash, const unsigned int nBits, const int algo, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompactBase256(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || algo < -1 || algo == CBlockHeader::AlgoType::ALGO_POS || algo >= CBlockHeader::AlgoType::ALGO_COUNT || bnTarget > UintToArith256(params.powLimit[algo == -1 ? CBlockHeader::AlgoType::ALGO_POW_SHA256 : algo]))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
