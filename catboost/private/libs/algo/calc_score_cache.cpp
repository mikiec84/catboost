#include "calc_score_cache.h"

#include <catboost/libs/helpers/restorable_rng.h>
#include <catboost/private/libs/options/oblivious_tree_options.h>

#include <util/generic/algorithm.h>
#include <util/generic/xrange.h>
#include <util/generic/ymath.h>
#include <util/system/guard.h>


using namespace NCB;


bool IsSamplingPerTree(const NCatboostOptions::TObliviousTreeLearnerOptions& fitParams) {
    return fitParams.SamplingFrequency.Get() == ESamplingFrequency::PerTree;
}

TVector<TBucketStats, TPoolAllocator>& TBucketStatsCache::GetStats(
    const TSplitEnsemble& splitEnsemble,
    int splitStatsCount,
    bool* areStatsDirty
) {
    TVector<TBucketStats, TPoolAllocator>* splitStats;
    with_lock(Lock) {
        if (Stats.contains(splitEnsemble) && Stats[splitEnsemble] != nullptr) {
            splitStats = Stats[splitEnsemble].Get();
            Y_ASSERT(splitStats->ysize() >= splitStatsCount);
            *areStatsDirty = false;
        } else {
            splitStats = new TVector<TBucketStats, TPoolAllocator>(MemoryPool.Get());
            splitStats->yresize(MaxBodyTailCount * ApproxDimension * splitStatsCount);
            Stats[splitEnsemble] = splitStats;
            *areStatsDirty = true;
        }
    }
    return *splitStats;
}

void TBucketStatsCache::GarbageCollect() {
    if (MemoryPool->MemoryWaste() > InitialSize) { // limit memory overhead
        Stats.clear();
        MemoryPool->Clear();
    }
}

TVector<TBucketStats> TBucketStatsCache::GetStatsInUse(int segmentCount,
    int segmentSize,
    int statsCount,
    const TVector<TBucketStats, TPoolAllocator>& cachedStats
) {
    TVector<TBucketStats> stats;
    stats.yresize(segmentCount * statsCount);
    for (int segmentIdx : xrange(segmentCount)) {
        const auto* srcBegin = &cachedStats[segmentIdx * segmentSize];
        auto* dstBegin = &stats[segmentIdx * statsCount];
        Copy(srcBegin, srcBegin + statsCount, dstBegin);
    }
    return stats;
}

void TCalcScoreFold::TVectorSlicing::Create(const NPar::TLocalExecutor::TExecRangeParams& docBlockParams) {
    Total = docBlockParams.LastId;
    Slices.yresize(docBlockParams.GetBlockCount());
    for (int sliceIdx = 0; sliceIdx < docBlockParams.GetBlockCount(); ++sliceIdx) {
        Slices[sliceIdx].Offset =  docBlockParams.GetBlockSize() * sliceIdx;
        Slices[sliceIdx].Size = Min(docBlockParams.GetBlockSize(), Total - Slices[sliceIdx].Offset);
    }
}

void TCalcScoreFold::TVectorSlicing::CreateByControl(
    const NPar::TLocalExecutor::TExecRangeParams& docBlockParams,
    const TUnsizedVector<bool>& control,
    NPar::TLocalExecutor* localExecutor
) {
    Slices.yresize(docBlockParams.GetBlockCount());
    const bool* controlData = GetDataPtr(control);
    TSlice* slicesData = GetDataPtr(Slices);
    localExecutor->ExecRange(
        [=](int sliceIdx) {
            // use a local var instead of Slices[sliceIdx].Size so that the compiler can use a register
            int sliceSize = 0;
            NPar::TLocalExecutor::BlockedLoopBody(
                docBlockParams,
                [=, &sliceSize](int doc) {sliceSize += controlData[doc];}
            )(sliceIdx);
            slicesData[sliceIdx].Size = sliceSize;
        },
        0,
        docBlockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
    int offset = 0;
    for (auto& slice : Slices) {
        slice.Offset = offset;
        offset += slice.Size;
    }
    Total = offset;
}

void TCalcScoreFold::TVectorSlicing::CreateByQueriesInfo(
    const TVector<TQueryInfo>& srcQueriesInfo,
    const NPar::TLocalExecutor::TExecRangeParams& queryBlockParams
) {
    CB_ENSURE(srcQueriesInfo.size() > 0, "Empty srcQueriesInfo");

    Total = (queryBlockParams.LastId == 0) ? 0 : srcQueriesInfo[queryBlockParams.LastId - 1].End;
    Slices.yresize(queryBlockParams.GetBlockCount());
    for (int sliceIdx : xrange(queryBlockParams.GetBlockCount())) {
        int firstQueryIdx = queryBlockParams.GetBlockSize() * sliceIdx;

        // inclusive
        int lastQueryIdx = Min(firstQueryIdx + queryBlockParams.GetBlockSize(), srcQueriesInfo.ysize()) - 1;
        Slices[sliceIdx].Offset = srcQueriesInfo[firstQueryIdx].Begin;
        Slices[sliceIdx].Size = srcQueriesInfo[lastQueryIdx].End - srcQueriesInfo[firstQueryIdx].Begin;
    }
}

void TCalcScoreFold::TVectorSlicing::CreateByQueriesInfoAndControl(
    const TVector<TQueryInfo>& srcQueriesInfo,
    const NPar::TLocalExecutor::TExecRangeParams& queryBlockParams,
    const TUnsizedVector<bool>& control,
    bool isPairwiseScoring,
    NPar::TLocalExecutor* localExecutor,
    TVector<TQueryInfo>* dstQueriesInfo
) {
    int srcQueriesSize = srcQueriesInfo.ysize();
    CB_ENSURE(srcQueriesSize > 0, "Empty srcQueriesInfo");

    dstQueriesInfo->clear();
    dstQueriesInfo->resize(srcQueriesInfo.size());
    Slices.yresize(queryBlockParams.GetBlockCount());

    const bool* controlData = GetDataPtr(control);
    localExecutor->ExecRange(
        [&](int sliceIdx) {
            int beginQueryIdx = queryBlockParams.GetBlockSize() * sliceIdx;
            int endQueryIdx = Min(beginQueryIdx + queryBlockParams.GetBlockSize(), srcQueriesSize);

            TVector<int> perQuerySrcToDstDocIdx; // -1 means no such doc in dst

            for (int queryIdx : xrange(beginQueryIdx, endQueryIdx)) {
                const auto& srcQueryInfo = srcQueriesInfo[queryIdx];
                auto& dstQueryInfo = (*dstQueriesInfo)[queryIdx];

                dstQueryInfo.Weight = srcQueryInfo.Weight;

                /* use a local var instead of (*dstQueriesInfo)[queryIdx].End so that the compiler can use a
                 * register
                 */
                ui32 dstQueryDocCount = 0;

                if (isPairwiseScoring) {
                    perQuerySrcToDstDocIdx.yresize(srcQueryInfo.GetSize());
                    for (int srcDocLocalIdx : xrange(srcQueryInfo.GetSize())) {
                        if (controlData[srcQueryInfo.Begin + srcDocLocalIdx]) {
                            perQuerySrcToDstDocIdx[srcDocLocalIdx] = dstQueryDocCount;
                            ++dstQueryDocCount;
                            if (!srcQueryInfo.SubgroupId.empty()) {
                                dstQueryInfo.SubgroupId.push_back(srcQueryInfo.SubgroupId[srcDocLocalIdx]);
                            }
                        } else {
                            perQuerySrcToDstDocIdx[srcDocLocalIdx] = -1;
                        }
                    }
                    if (srcQueryInfo.GetSize() == dstQueryDocCount) {
                        dstQueryInfo.Competitors = srcQueryInfo.Competitors;
                    } else if (dstQueryDocCount > 0) {
                        dstQueryInfo.Competitors.resize(dstQueryDocCount);
                        for (int srcIdx1 : xrange(srcQueryInfo.GetSize())) {
                            if (perQuerySrcToDstDocIdx[srcIdx1] != -1) {
                                auto& dstCompetitorsPart =
                                    dstQueryInfo.Competitors[perQuerySrcToDstDocIdx[srcIdx1]];
                                for (const auto& srcCompetitor : srcQueryInfo.Competitors[srcIdx1]) {
                                    if (perQuerySrcToDstDocIdx[srcCompetitor.Id] != -1) {
                                        dstCompetitorsPart.push_back(
                                            TCompetitor(
                                                perQuerySrcToDstDocIdx[srcCompetitor.Id],
                                                srcCompetitor.Weight
                                            )
                                        );
                                        dstCompetitorsPart.back().SampleWeight = srcCompetitor.SampleWeight;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    for (int srcDocLocalIdx : xrange(srcQueryInfo.GetSize())) {
                        if (controlData[srcQueryInfo.Begin + srcDocLocalIdx]) {
                            ++dstQueryDocCount;
                            if (!srcQueryInfo.SubgroupId.empty()) {
                                dstQueryInfo.SubgroupId.push_back(srcQueryInfo.SubgroupId[srcDocLocalIdx]);
                            }
                        }
                    }
                }
                // temporarily use End as queryDocCount to avoid extra temporary storage
                (*dstQueriesInfo)[queryIdx].End = dstQueryDocCount;
            }
        },
        0,
        queryBlockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE
    );

    int offset = 0;
    for (int sliceIdx : xrange(queryBlockParams.GetBlockCount())) {
        Slices[sliceIdx].Offset = offset;

        int beginQueryIdx = queryBlockParams.GetBlockSize() * sliceIdx;
        int endQueryIdx = Min(beginQueryIdx + queryBlockParams.GetBlockSize(), srcQueriesSize);

        for (int queryIdx : xrange(beginQueryIdx, endQueryIdx)) {
            auto& dstQueryInfo = (*dstQueriesInfo)[queryIdx];
            dstQueryInfo.Begin = offset;
            // End was used as queryDocCount to avoid extra temporary storage
            offset += dstQueryInfo.End;
            dstQueryInfo.End = offset;
        }
        Slices[sliceIdx].Size = offset - Slices[sliceIdx].Offset;
    }
    Total = offset;
}


static int GetMaxBodyFinish(const TVector<TFold>& folds, int bodyTailIdx) {
    int maxBodyFinish = 0;
    for (const auto& fold : folds) {
        if (bodyTailIdx < fold.BodyTailArr.ysize()) {
            maxBodyFinish = Max(maxBodyFinish, fold.BodyTailArr[bodyTailIdx].BodyFinish);
        }
    }
    return maxBodyFinish;
}

static int GetMaxTailFinish(const TVector<TFold>& folds, int bodyTailIdx) {
    int maxTailFinish = 0;
    for (const auto& fold : folds) {
        if (bodyTailIdx < fold.BodyTailArr.ysize()) {
            maxTailFinish = Max(maxTailFinish, fold.BodyTailArr[bodyTailIdx].TailFinish);
        }
    }
    return maxTailFinish;
}

void TCalcScoreFold::Create(
    const TVector<TFold>& folds,
    bool isPairwiseScoring,
    int defaultCalcStatsObjBlockSize,
    float sampleRate
) {
    BernoulliSampleRate = sampleRate;
    Y_ASSERT(BernoulliSampleRate > 0.0f && BernoulliSampleRate <= 1.0f);
    DocCount = folds[0].GetLearnSampleCount();
    Y_ASSERT(DocCount > 0);
    Indices.yresize(DocCount);
    FeaturesSubsetBegin = folds[0].FeaturesSubsetBegin;
    IndexInFold.yresize(DocCount);
    LearnWeights.yresize(DocCount);
    SampleWeights.yresize(DocCount);
    LearnQueriesInfo.yresize(folds[0].LearnQueriesInfo.ysize());
    Control.yresize(DocCount);
    BodyTailCount = GetMaxBodyTailCount(folds);
    HasPairwiseWeights = !folds[0].BodyTailArr[0].PairwiseWeights.empty();
    IsPairwiseScoring = isPairwiseScoring;
    Y_ASSERT(BodyTailCount > 0);
    BodyTailArr.yresize(BodyTailCount);
    ApproxDimension = folds[0].GetApproxDimension();
    Y_ASSERT(ApproxDimension > 0);
    for (int bodyTailIdx = 0; bodyTailIdx < BodyTailCount; ++bodyTailIdx) {
        BodyTailArr[bodyTailIdx].WeightedDerivatives.yresize(ApproxDimension);
        BodyTailArr[bodyTailIdx].SampleWeightedDerivatives.yresize(ApproxDimension);
        const int bodyFinish = GetMaxBodyFinish(folds, bodyTailIdx);
        Y_ASSERT(bodyFinish > 0);
        const int tailFinish = GetMaxTailFinish(folds, bodyTailIdx);
        Y_ASSERT(tailFinish > 0);
        if (HasPairwiseWeights) {
            BodyTailArr[bodyTailIdx].PairwiseWeights.yresize(tailFinish);
            BodyTailArr[bodyTailIdx].SamplePairwiseWeights.yresize(tailFinish);
        }
        for (int dimIdx = 0; dimIdx < ApproxDimension; ++dimIdx) {
            BodyTailArr[bodyTailIdx].WeightedDerivatives[dimIdx].yresize(bodyFinish);
            BodyTailArr[bodyTailIdx].SampleWeightedDerivatives[dimIdx].yresize(tailFinish);
        }
    }
    DefaultCalcStatsObjBlockSize = defaultCalcStatsObjBlockSize;
    LeavesCount = 1;
    LeavesBounds.assign(1, {0, static_cast<ui32>(DocCount)});
    LeavesIndices.assign(1, 0);
}

template <typename TSrcRef, typename TGetElementFunc, typename TDstRef>
static inline void SetElements(
    TArrayRef<const bool> srcControlRef,
    TSrcRef srcRef,
    TGetElementFunc GetElementFunc,
    TDstRef dstRef,
    int* dstCount
) {
    const auto* sourceData = srcRef.data();
    const size_t sourceCount = srcRef.size();
    auto* __restrict destinationData = dstRef.data();
    const size_t destinationCount = dstRef.size();
    if (sourceData != nullptr && srcControlRef.size() == destinationCount) {
        Copy(sourceData, sourceData + sourceCount, destinationData);
        *dstCount = sourceCount;
        return;
    }
    const bool* controlData = srcControlRef.data();
    size_t endElementIdx = 0;
#pragma unroll(4)
    for (size_t sourceIdx = 0; sourceIdx < sourceCount && endElementIdx < destinationCount; ++sourceIdx) {
        destinationData[endElementIdx] = GetElementFunc(sourceData, sourceIdx);
        endElementIdx += controlData[sourceIdx];
    }
    *dstCount = endElementIdx;
}

template <typename TData>
static inline TData GetElement(const TData* source, size_t j) {
    return source[j];
}

template <typename TData, typename TDstRef>
static inline void SetElementsToConstant(
    TArrayRef<const bool> srcControlRef,
    TData constant,
    TDstRef dstRef,
    int* dstCount
) {
    const bool* controlData = srcControlRef.data();
    const size_t sourceCount = srcControlRef.size();
    auto* __restrict destinationData = dstRef.data();
    const size_t destinationCount = dstRef.size();
    size_t endElementIdx = 0;
#pragma unroll(4)
    for (size_t sourceIdx = 0; sourceIdx < sourceCount && endElementIdx < destinationCount; ++sourceIdx) {
        destinationData[endElementIdx] = constant;
        endElementIdx += controlData[sourceIdx];
    }
    *dstCount = endElementIdx;
}


template <typename TFoldType>
void TCalcScoreFold::SelectBlockFromFold(const TFoldType& fold, TSlice srcBlock, TSlice dstBlock) {
    int ignored;
    const auto srcControlRef = srcBlock.GetConstRef(Control);
    SetElements(
        srcControlRef,
        srcBlock.GetConstRef(fold.LearnPermutationFeaturesSubset.template Get<TIndexedSubset<ui32>>()),
        GetElement<ui32>,
        dstBlock.GetRef(LearnPermutationFeaturesSubset.template Get<TIndexedSubset<ui32>>()),
        &ignored
    );

    const auto& srcLearnWeights = fold.GetLearnWeights();
    TArrayRef<float> dstLearnWeights = dstBlock.GetRef(LearnWeights);
    if (srcLearnWeights.empty()) {
        SetElementsToConstant(srcControlRef, 1.0f, dstLearnWeights, &ignored);
    } else {
        SetElements(
            srcControlRef,
            srcBlock.GetConstRef(srcLearnWeights),
            GetElement<float>,
            dstLearnWeights,
            &ignored
        );
    }
    SetElements(
        srcControlRef,
        srcBlock.GetConstRef(fold.SampleWeights),
        GetElement<float>,
        dstBlock.GetRef(SampleWeights),
        &ignored
    );
    for (int bodyTailIdx = 0; bodyTailIdx < BodyTailCount; ++bodyTailIdx) {
        const auto& srcBodyTail = fold.BodyTailArr[bodyTailIdx];
        auto& dstBodyTail = BodyTailArr[bodyTailIdx];
        const auto srcBodyBlock = srcBlock.Clip(srcBodyTail.BodyFinish);
        const auto srcTailBlock = srcBlock.Clip(srcBodyTail.TailFinish);
        int bodyCount = 0;
        int tailCount = 0;
        if (HasPairwiseWeights) {
            SetElements(
                srcControlRef,
                srcTailBlock.GetConstRef(srcBodyTail.PairwiseWeights),
                GetElement<float>,
                dstBlock.GetRef(dstBodyTail.PairwiseWeights),
                &tailCount
            );
            SetElements(
                srcControlRef,
                srcTailBlock.GetConstRef(srcBodyTail.SamplePairwiseWeights),
                GetElement<float>,
                dstBlock.GetRef(dstBodyTail.SamplePairwiseWeights),
                &tailCount
            );
        }
        for (int dim = 0; dim < ApproxDimension; ++dim) {
            SetElements(
                srcControlRef,
                srcBodyBlock.GetConstRef(srcBodyTail.WeightedDerivatives[dim]),
                GetElement<double>,
                dstBlock.GetRef(dstBodyTail.WeightedDerivatives[dim]),
                &bodyCount
            );
            SetElements(
                srcControlRef,
                srcTailBlock.GetConstRef(srcBodyTail.SampleWeightedDerivatives[dim]),
                GetElement<double>,
                dstBlock.GetRef(dstBodyTail.SampleWeightedDerivatives[dim]),
                &tailCount
            );
        }
        AtomicAdd(dstBodyTail.BodyFinish, bodyCount); // these atomics may take up to 2-3% of iteration time
        AtomicAdd(dstBodyTail.TailFinish, tailCount);
    }
}

void TCalcScoreFold::SelectSmallestSplitSide(
    int curDepth,
    const TCalcScoreFold& fold,
    NPar::TLocalExecutor* localExecutor
) {
    SetSmallestSideControl(curDepth, fold.DocCount, fold.Indices, localExecutor);

    TVectorSlicing srcBlocks;
    TVectorSlicing dstBlocks;
    int blockCount = 0;

    CreateBlocksAndUpdateQueriesInfoByControl(
        localExecutor,
        fold.DocCount,
        fold.LearnQueriesInfo,
        &blockCount,
        &srcBlocks,
        &dstBlocks,
        &LearnQueriesInfo
    );

    DocCount = dstBlocks.Total;
    LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>().yresize(DocCount);
    ClearBodyTail();
    BodyTailCount = fold.GetBodyTailCount();
    localExecutor->ExecRange(
        [&](int blockIdx) {
            int ignored;
            const auto srcBlock = srcBlocks.Slices[blockIdx];
            const auto srcControlRef = srcBlock.GetConstRef(Control);
            const auto srcIndicesRef = srcBlock.GetConstRef(fold.Indices);
            const auto dstBlock = dstBlocks.Slices[blockIdx];
            const TIndexType splitWeight = 1 << (curDepth - 1);
            SetElements(
                srcControlRef,
                srcBlock.GetConstRef(TVector<TIndexType>()),
                [=](const TIndexType*, size_t i) {return srcIndicesRef[i] | splitWeight;},
                dstBlock.GetRef(Indices),
                &ignored
            );
            SetElements(
                srcControlRef,
                srcBlock.GetConstRef(fold.IndexInFold),
                GetElement<ui32>,
                dstBlock.GetRef(IndexInFold),
                &ignored
            );
            SelectBlockFromFold(fold, srcBlock, dstBlock);
        },
        0,
        blockCount,
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
    SetPermutationBlockSizeAndCalcStatsRanges(FoldPermutationBlockSizeNotSet, FoldPermutationBlockSizeNotSet);
}

static void CalcCumulativeOffsets(const TVector<ui32>& counts, TVector<ui32>* offsets, ui32 startOffset = 0) {
    offsets->yresize(counts.size());
    TArrayRef<ui32> offsetsRef(*offsets);
    offsetsRef[0] = startOffset;
    for (auto i : xrange(static_cast<size_t>(1), counts.size())) {
        offsetsRef[i] = offsetsRef[i - 1] + counts[i - 1];
    }
}

// primarily for sampling per tree level
void TCalcScoreFold::SortFoldByLeafIndex(ui32 leafCount, NPar::TLocalExecutor* localExecutor) {
    if (leafCount == 1) {
        LeavesCount = 1;
        LeavesBounds.assign(1, {0, static_cast<ui32>(DocCount)});
        LeavesIndices.assign(1, 0);
        return;
    }

    LeavesCount = leafCount;
    Y_ASSERT(GetBodyTailCount() == 1);
    TBodyTail& bt = BodyTailArr[0];
    TIndexedSubset<ui32>& indexedSubset = LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>();

    TUnsizedVector<float> newSampleWeights;
    TUnsizedVector<ui32> newIndexedSubset;
    TUnsizedVector<ui32> newIndexInFold;
    TUnsizedVector<TUnsizedVector<double>> newSampleWeightedDerivatives;
    TUnsizedVector<TIndexType> newIndices;

    // take capacity because of unsized vectors
    size_t capacity = Indices.capacity();
    newSampleWeights.yresize(capacity);
    newIndexedSubset.yresize(capacity);
    newIndexInFold.yresize(capacity);
    newSampleWeightedDerivatives.resize(ApproxDimension);
    for (auto dim : xrange(ApproxDimension)) {
        newSampleWeightedDerivatives[dim].yresize(capacity);
    }
    newIndices.yresize(capacity);

    const int blockSize = CeilDiv(DocCount, localExecutor->GetThreadCount() + 1);
    TSimpleIndexRangesGenerator<int> indexRangesGenerator(TIndexRange<int>(DocCount), blockSize);
    const int blockCount = indexRangesGenerator.RangesCount();

    // count docs for each pair (block, leaf)
    TVector<TVector<ui32>> docsInLeaf(blockCount, TVector<ui32>(LeavesCount));
    localExecutor->ExecRange(
        [&](int blockIdx) {
            TConstArrayRef<TIndexType> indicesRef(Indices.data(), DocCount);
            TArrayRef<ui32> blockDocsInLeaf(docsInLeaf[blockIdx].data(), LeavesCount);
            for (auto doc : indexRangesGenerator.GetRange(blockIdx).Iter()) {
                ++blockDocsInLeaf[indicesRef[doc]];
            }
        },
        NPar::TLocalExecutor::TExecRangeParams(0, blockCount),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    // count total docs for each leaf
    TVector<ui32> totalDocsInLeaf(LeavesCount);
    for (const auto& docsInBlock: docsInLeaf) {
        for (auto leaf : xrange(LeavesCount)) {
            totalDocsInLeaf[leaf] += docsInBlock[leaf];
        }
    }

    // calc offsets for each pair (block, leaf)
    TVector<TVector<ui32>> docsOffsets(blockCount);
    CalcCumulativeOffsets(totalDocsInLeaf, &docsOffsets[0]);
    for (auto blockIdx : xrange(1, blockCount)) {
        docsOffsets[blockIdx].yresize(LeavesCount);
        for (auto leaf : xrange(LeavesCount)) {
            docsOffsets[blockIdx][leaf] = docsOffsets[blockIdx - 1][leaf] + docsInLeaf[blockIdx - 1][leaf];
        }
    }

    // copy data to new positions
    localExecutor->ExecRange(
        [&](int blockIdx) {
            // creating ArrayRefs for speedup
            TConstArrayRef<TIndexType> curIndicesRef(Indices.data(), DocCount);
            TConstArrayRef<float> curSampleWeightsRef(SampleWeights.data(), DocCount);
            TConstArrayRef<ui32> curIndexedSubsetRef(indexedSubset.data(), DocCount);
            TConstArrayRef<ui32> curIndexInFoldRef(IndexInFold.data(), DocCount);
            TVector<TConstArrayRef<double>> curSampleWeightedDerivativesRef;
            for (auto dim : xrange(ApproxDimension)) {
                curSampleWeightedDerivativesRef.emplace_back(bt.SampleWeightedDerivatives[dim].data(), DocCount);
            }

            TArrayRef<TIndexType> newIndicesRef(newIndices.data(), DocCount);
            TArrayRef<float> newSampleWeightsRef(newSampleWeights.data(), DocCount);
            TArrayRef<ui32> newIndexedSubsetRef(newIndexedSubset.data(), DocCount);
            TArrayRef<ui32> newIndexInFoldRef(newIndexInFold.data(), DocCount);
            TVector<TArrayRef<double>> newSampleWeightedDerivativesRef;
            for (auto dim : xrange(ApproxDimension)) {
                newSampleWeightedDerivativesRef.emplace_back(newSampleWeightedDerivatives[dim].data(), DocCount);
            }

            TArrayRef<ui32> blockDocsOffsetsRef(docsOffsets[blockIdx].data(), LeavesCount);
            for (auto doc : indexRangesGenerator.GetRange(blockIdx).Iter()) {
                ui32 newIdx = blockDocsOffsetsRef[curIndicesRef[doc]]++;
                newIndicesRef[newIdx] = curIndicesRef[doc];
                newSampleWeightsRef[newIdx] = curSampleWeightsRef[doc];
                newIndexedSubsetRef[newIdx] = curIndexedSubsetRef[doc];
                newIndexInFoldRef[newIdx] = curIndexInFoldRef[doc];
                for (auto dim : xrange(ApproxDimension)) {
                    newSampleWeightedDerivativesRef[dim][newIdx] = curSampleWeightedDerivativesRef[dim][doc];
                }
            }
        },
        NPar::TLocalExecutor::TExecRangeParams(0, blockCount),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    SampleWeights = std::move(newSampleWeights);
    indexedSubset = std::move(newIndexedSubset);
    IndexInFold = std::move(newIndexInFold);
    bt.SampleWeightedDerivatives = std::move(newSampleWeightedDerivatives);
    Indices = std::move(newIndices);

    LeavesBounds.yresize(LeavesCount);
    LeavesBounds[0] = {0, totalDocsInLeaf[0]};
    for (auto leaf : xrange(ui32(1), LeavesCount)) {
        LeavesBounds[leaf] = {LeavesBounds[leaf - 1].End, LeavesBounds[leaf - 1].End + totalDocsInLeaf[leaf]};
    }
    LeavesIndices.yresize(LeavesCount);
    Iota(LeavesIndices.begin(), LeavesIndices.end(), 0);
}

void TCalcScoreFold::Sample(
    const TFold& fold,
    ESamplingUnit samplingUnit,
    const TVector<TIndexType>& indices,
    TRestorableFastRng64* rand,
    NPar::TLocalExecutor* localExecutor,
    bool performRandomChoice,
    bool shouldSortByLeaf,
    ui32 leavesCount
) {
    if (performRandomChoice) {
        SetSampledControl(indices.ysize(), samplingUnit, fold.LearnQueriesInfo, rand);
    } else {
        BernoulliSampleRate = 0.0f;
        Y_ASSERT(samplingUnit == ESamplingUnit::Object);
        SetControlNoZeroWeighted(indices.ysize(), fold.SampleWeights.data());
    }

    TVectorSlicing srcBlocks;
    TVectorSlicing dstBlocks;
    int blockCount = 0;

    CreateBlocksAndUpdateQueriesInfoByControl(
        localExecutor,
        indices.ysize(),
        fold.LearnQueriesInfo,
        &blockCount,
        &srcBlocks,
        &dstBlocks,
        &LearnQueriesInfo
    );

    DocCount = dstBlocks.Total;
    LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>().yresize(DocCount);
    ClearBodyTail();
    BodyTailCount = fold.BodyTailArr.ysize();
    localExecutor->ExecRange(
        [&](int blockIdx) {
            const auto srcBlock = srcBlocks.Slices[blockIdx];
            const auto srcControlRef = srcBlock.GetConstRef(Control);
            const auto dstBlock = dstBlocks.Slices[blockIdx];
            int ignored;
            SetElements(
                srcControlRef,
                srcBlock.GetConstRef(indices),
                GetElement<TIndexType>,
                dstBlock.GetRef(Indices),
                &ignored
            );
            SetElements(
                srcControlRef,
                srcBlock.GetConstRef(TVector<size_t>()),
                [=](const size_t*, size_t j) {return ui32(srcBlock.Offset + j);},
                dstBlock.GetRef(IndexInFold),
                &ignored
            );
            SelectBlockFromFold(fold, srcBlock, dstBlock);
        },
        0,
        blockCount,
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
    SetPermutationBlockSizeAndCalcStatsRanges(
        (BernoulliSampleRate == 1.0f || IsPairwiseScoring) ? fold.PermutationBlockSize :
            FoldPermutationBlockSizeNotSet,
        (BernoulliSampleRate == 1.0f || IsPairwiseScoring) ? DocCount : FoldPermutationBlockSizeNotSet
    );
    if (shouldSortByLeaf) {
        Y_ASSERT(leavesCount > 0);
        SortFoldByLeafIndex(leavesCount, localExecutor);
    }
}

void TCalcScoreFold::UpdateIndices(const TVector<TIndexType>& indices, NPar::TLocalExecutor* localExecutor) {
    NPar::TLocalExecutor::TExecRangeParams blockParams(0, indices.ysize());
    blockParams.SetBlockSize(2000);
    const int blockCount = blockParams.GetBlockCount();
    TVectorSlicing srcBlocks;
    srcBlocks.Create(blockParams);

    TVectorSlicing dstBlocks;
    if (BernoulliSampleRate < 1.0f && !IsPairwiseScoring) {
        dstBlocks.CreateByControl(blockParams, Control, localExecutor);
    } else {
        dstBlocks = srcBlocks;
    }

    DocCount = dstBlocks.Total;
    localExecutor->ExecRange(
        [&](int blockIdx) {
            const auto srcBlock = srcBlocks.Slices[blockIdx];
            const auto dstBlock = dstBlocks.Slices[blockIdx];
            int ignored;
            const auto srcControlRef = srcBlock.GetConstRef(Control);
            SetElements(
                srcControlRef,
                srcBlock.GetConstRef(indices),
                GetElement<TIndexType>,
                dstBlock.GetRef(Indices),
                &ignored
            );
        },
        0,
        blockCount,
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
}

// only for sampling per tree
void TCalcScoreFold::UpdateIndicesInLeafwiseSortedFold(const TVector<TIndexType>& indices, NPar::TLocalExecutor* localExecutor) {
    Y_ASSERT(GetBodyTailCount() == 1);
    Y_UNUSED(localExecutor);

    localExecutor->ExecRange(
        [&](int doc) {
            Indices[doc] = indices[IndexInFold[doc]];
        },
        NPar::TLocalExecutor::TExecRangeParams(0, DocCount).SetBlockCountToThreadCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    ui32 oldLeafCount = LeavesCount;
    ui32 newLeafCount = LeavesCount *= 2;

    TUnsizedVector<float> newSampleWeights;
    TUnsizedVector<ui32> newIndexedSubset;
    TUnsizedVector<ui32> newIndexInFold;
    TUnsizedVector<TUnsizedVector<double>> newSampleWeightedDerivatives;
    TUnsizedVector<TIndexType> newIndices;

    // take capacity because of unsized vectors
    size_t capacity = Indices.capacity();
    newSampleWeights.yresize(capacity);
    newIndexedSubset.yresize(capacity);
    newIndexInFold.yresize(capacity);
    newSampleWeightedDerivatives.resize(ApproxDimension);
    for (auto dim : xrange(ApproxDimension)) {
        newSampleWeightedDerivatives[dim].yresize(capacity);
    }
    newIndices.yresize(capacity);

    TVector<TIndexRange<ui32>> newLeavesBounds;
    TVector<ui32> newLeavesIndices;
    newLeavesBounds.yresize(newLeafCount);
    newLeavesIndices.yresize(newLeafCount);

    TBodyTail& bt = BodyTailArr[0];
    TIndexedSubset<ui32>& indexedSubset = LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>();

    ui32 rightLeafIndexShift = oldLeafCount;
    localExecutor->ExecRange(
        [&](ui32 leaf) {
            const ui32 begin = LeavesBounds[leaf].Begin;
            const ui32 end = LeavesBounds[leaf].End;
            if (begin == end) {
                newLeavesBounds[leaf << 1u] = {begin, end};
                newLeavesBounds[(leaf << 1u) | 1u] = {begin, end};
                newLeavesIndices[leaf << 1u] = LeavesIndices[leaf];
                newLeavesIndices[(leaf << 1u) | 1u] = LeavesIndices[leaf] + rightLeafIndexShift;
                return;
            }

            const TIndexType leftIndex = LeavesIndices[leaf];

            const int blockSize = Max(CeilDiv(DocCount, localExecutor->GetThreadCount() + 1), 1000);
            TSimpleIndexRangesGenerator<int> indexRangesGenerator(TIndexRange<int>(begin, end), blockSize);
            const int blockCount = indexRangesGenerator.RangesCount();

            // count left and right docs for each block
            TVector<ui32> leftDocsCount(blockCount);
            TVector<ui32> rightDocsCount(blockCount);
            localExecutor->ExecRange(
                [&, leftIndex](int blockId) {
                    ui32 leftCount = 0;
                    TConstArrayRef<ui32> oldIndicesRef(Indices.data(), DocCount);
                    for (auto doc : indexRangesGenerator.GetRange(blockId).Iter()) {
                        leftCount += (oldIndicesRef[doc] == leftIndex);
                    }
                    leftDocsCount[blockId] = leftCount;
                    rightDocsCount[blockId] = indexRangesGenerator.GetRange(blockId).GetSize() - leftCount;
                },
                0,
                blockCount,
                NPar::TLocalExecutor::WAIT_COMPLETE);

            // calc offsets for each block
            TVector<ui32> leftDocsOffset, rightDocsOffset;
            CalcCumulativeOffsets(leftDocsCount, &leftDocsOffset, begin);
            CalcCumulativeOffsets(rightDocsCount, &rightDocsOffset, leftDocsOffset.back() + leftDocsCount.back());

            newLeavesBounds[leaf << 1u] = {begin, rightDocsOffset[0]};
            newLeavesBounds[(leaf << 1u) | 1u] = {rightDocsOffset[0], end};
            newLeavesIndices[leaf << 1u] = leftIndex;
            newLeavesIndices[(leaf << 1u) | 1u] = leftIndex + rightLeafIndexShift;

            // copy data to new positions
            localExecutor->ExecRange(
                [&](int blockId) {
                    TConstArrayRef<ui32> oldIndicesRef(Indices.data(), DocCount);
                    TConstArrayRef<float> oldSampleWeightsRef(SampleWeights.data(), DocCount);
                    TConstArrayRef<ui32> oldIndexedSubsetRef(indexedSubset.data(), DocCount);
                    TConstArrayRef<ui32> oldIndexInFoldRef(IndexInFold.data(), DocCount);

                    TArrayRef<ui32> newIndicesRef(newIndices.data(), DocCount);
                    TArrayRef<float> newSampleWeightsRef(newSampleWeights.data(), DocCount);
                    TArrayRef<ui32> newIndexedSubsetRef(newIndexedSubset.data(), DocCount);
                    TArrayRef<ui32> newIndexInFoldRef(newIndexInFold.data(), DocCount);

                    ui32 leftOffset = leftDocsOffset[blockId];
                    ui32 rightOffset = rightDocsOffset[blockId];
                    for (auto doc : indexRangesGenerator.GetRange(blockId).Iter()) {
                        int newIdx = (oldIndicesRef[doc] == leftIndex) ? (leftOffset++) : (rightOffset++);
                        newIndicesRef[newIdx] = oldIndicesRef[doc];
                        newSampleWeightsRef[newIdx] = oldSampleWeightsRef[doc];
                        newIndexedSubsetRef[newIdx] = oldIndexedSubsetRef[doc];
                        newIndexInFoldRef[newIdx] = oldIndexInFoldRef[doc];
                    }

                    for (auto dim : xrange(ApproxDimension)) {
                        leftOffset = leftDocsOffset[blockId];
                        rightOffset = rightDocsOffset[blockId];
                        TConstArrayRef<double> oldDerivativesRef(bt.SampleWeightedDerivatives[dim].data(), DocCount);
                        TArrayRef<double> newDerivativesRef(newSampleWeightedDerivatives[dim].data(), DocCount);
                        for (auto doc : indexRangesGenerator.GetRange(blockId).Iter()) {
                            int newIdx = (oldIndicesRef[doc] == leftIndex) ? (leftOffset++) : (rightOffset++);
                            newDerivativesRef[newIdx] = oldDerivativesRef[doc];
                        }
                    }
                },
                0,
                blockCount,
                NPar::TLocalExecutor::WAIT_COMPLETE);
        },
        0,
        LeavesBounds.size(),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    SampleWeights = std::move(newSampleWeights);
    indexedSubset = std::move(newIndexedSubset);
    IndexInFold = std::move(newIndexInFold);
    bt.SampleWeightedDerivatives = std::move(newSampleWeightedDerivatives);
    Indices = std::move(newIndices);

    LeavesBounds = std::move(newLeavesBounds);
    LeavesIndices = std::move(newLeavesIndices);
}

int TCalcScoreFold::GetApproxDimension() const {
    return ApproxDimension;
}

int TCalcScoreFold::GetDocCount() const {
    return DocCount;
}

int TCalcScoreFold::GetBodyTailCount() const {
    return BodyTailCount;
}

bool TCalcScoreFold::HasQueryInfo() const {
    return LearnQueriesInfo.size() > 1;
}

const NCB::IIndexRangesGenerator<int>& TCalcScoreFold::GetCalcStatsIndexRanges() const {
    return *CalcStatsIndexRanges;
}

void TCalcScoreFold::SetSmallestSideControl(
    int curDepth,
    int docCount,
    const TUnsizedVector<TIndexType>& indices,
    NPar::TLocalExecutor* localExecutor
) {
    Y_ASSERT(curDepth > 0);

    NPar::TLocalExecutor::TExecRangeParams blockParams(0, docCount);
    blockParams.SetBlockSize(4000);
    const int blockCount = blockParams.GetBlockCount();

    TVector<int> blockSize(blockCount, 0);
    const TIndexType* indicesData = GetDataPtr(indices);
    localExecutor->ExecRange(
        [=, &blockSize](int blockIdx) {
            int size = 0;
            NPar::TLocalExecutor::BlockedLoopBody(
                blockParams,
                [=, &size](int docIdx) {
                    size += indicesData[docIdx] >> (curDepth - 1);
                }
            )(blockIdx);
            blockSize[blockIdx] = size;
        },
        0,
        blockCount,
        NPar::TLocalExecutor::WAIT_COMPLETE
    );

    int trueCount = 0;
    for (int size : blockSize) {
        trueCount += size;
    }
    const TIndexType splitWeight = 1 << (curDepth - 1);
    bool* controlData = GetDataPtr(Control);
    if (trueCount * 2 > docCount) {
        SmallestSplitSideValue = false;
        localExecutor->ExecRange(
            [=](int docIdx) {
                controlData[docIdx] = indicesData[docIdx] < splitWeight;
            },
            blockParams,
            NPar::TLocalExecutor::WAIT_COMPLETE
        );
    } else {
        SmallestSplitSideValue = true;
        localExecutor->ExecRange(
            [=](int docIdx) {
                controlData[docIdx] = indicesData[docIdx] > splitWeight - 1;
            },
            blockParams,
            NPar::TLocalExecutor::WAIT_COMPLETE
        );
    }
}

void TCalcScoreFold::SetSampledControl(
    int docCount,
    ESamplingUnit samplingUnit,
    const TVector<TQueryInfo>& queriesInfo,
    TRestorableFastRng64* rand
) {
    if (BernoulliSampleRate == 1.0f || IsPairwiseScoring) {
        Fill(Control.begin(), Control.end(), true);
        return;
    }
    ui32 sampleSize = 0;
    if (samplingUnit == ESamplingUnit::Group) {
        for (auto& queryInfo : queriesInfo) {
            auto itBegin = GetDataPtr(Control, queryInfo.Begin);
            auto itEnd = GetDataPtr(Control, queryInfo.End);
            auto isTaken = rand->GenRandReal1() < BernoulliSampleRate;
            sampleSize += isTaken;
            Fill(itBegin, itEnd, isTaken);
        }
    } else {
        for (int docIdx = 0; docIdx < docCount; ++docIdx) {
            Control[docIdx] = rand->GenRandReal1() < BernoulliSampleRate;
            sampleSize += Control[docIdx];
        }
    }
}

void TCalcScoreFold::SetControlNoZeroWeighted(
    int docCount,
    const float* sampleWeights
) {
    constexpr float EPS = std::numeric_limits<float>::epsilon();
    ui32 sampleSize = 0;
    for (int docIdx = 0; docIdx < docCount; ++docIdx) {
        Control[docIdx] = sampleWeights[docIdx] > EPS;
        sampleSize += Control[docIdx];
    }
}

void TCalcScoreFold::CreateBlocksAndUpdateQueriesInfoByControl(
    NPar::TLocalExecutor* localExecutor,
    int srcDocCount,
    const TVector<TQueryInfo>& srcQueriesInfo,
    int* blockCount,
    TVectorSlicing* srcBlocks,
    TVectorSlicing* dstBlocks,
    TVector<TQueryInfo>* dstQueriesInfo
) {
    if ((srcDocCount > 0) && (srcQueriesInfo.size() > 1)) {
        NPar::TLocalExecutor::TExecRangeParams queryBlockParams(0, srcQueriesInfo.size());
        queryBlockParams.SetBlockSize(Max((int)(2000 * i64(srcQueriesInfo.ysize()) / srcDocCount), 1));
        *blockCount = queryBlockParams.GetBlockCount();

        srcBlocks->CreateByQueriesInfo(srcQueriesInfo, queryBlockParams);
        dstBlocks->CreateByQueriesInfoAndControl(
            srcQueriesInfo,
            queryBlockParams,
            Control,
            IsPairwiseScoring,
            localExecutor,
            dstQueriesInfo
        );
    } else {
        NPar::TLocalExecutor::TExecRangeParams docBlockParams(0, srcDocCount);
        docBlockParams.SetBlockSize(2000);
        *blockCount = docBlockParams.GetBlockCount();

        srcBlocks->Create(docBlockParams);
        dstBlocks->CreateByControl(docBlockParams, Control, localExecutor);
    }
}

static bool HasPairs(const TVector<TQueryInfo>& learnQueriesInfo) {
    for (const auto& query : learnQueriesInfo) {
        if (query.Competitors.empty()) {
            continue;
        }
        const int begin = query.Begin;
        const int end = query.End;
        for (int winnerId = begin; winnerId < end; ++winnerId) {
            if (query.Competitors[winnerId - begin].ysize() > 0) {
                return true;
            }
        }
    }
    return false;
}

void TCalcScoreFold::SetPermutationBlockSizeAndCalcStatsRanges(
    int nonCtrDataPermutationBlockSize,
    int ctrDataPermutationBlockSize
) {
    CB_ENSURE(nonCtrDataPermutationBlockSize >= 0, "Negative nonCtrDataPermutationBlockSize");
    CB_ENSURE(ctrDataPermutationBlockSize >= 0, "Negative ctrDataPermutationBlockSize");

    NonCtrDataPermutationBlockSize = nonCtrDataPermutationBlockSize;
    CtrDataPermutationBlockSize = ctrDataPermutationBlockSize;

    const auto docCount = GetDocCount();

    if ((NonCtrDataPermutationBlockSize == FoldPermutationBlockSizeNotSet)
        || (NonCtrDataPermutationBlockSize == 1)
        || (NonCtrDataPermutationBlockSize == docCount))
    {
        int rangeEnd = 0;
        int blockSize = DefaultCalcStatsObjBlockSize;
        if (docCount && HasQueryInfo()) {
            if (HasPairs(LearnQueriesInfo)) {
                rangeEnd = CeilDiv(docCount, DefaultCalcStatsObjBlockSize);
                blockSize = 1;
            } else {
                rangeEnd = LearnQueriesInfo.ysize();
                CB_ENSURE(rangeEnd > 0, "non-positive query count");
                blockSize = Max(
                    int(
                        Min<i64>(
                            DefaultCalcStatsObjBlockSize,
                            i64(DefaultCalcStatsObjBlockSize) * rangeEnd / docCount
                        )
                    ),
                    1
                );
            }
        } else {
            rangeEnd = docCount;
        }
        CalcStatsIndexRanges = MakeHolder<NCB::TSimpleIndexRangesGenerator<int>>(
            NCB::TIndexRange<int>(rangeEnd),
            blockSize
        );
    } else { // non-trivial permutation
        CB_ENSURE(!HasQueryInfo(), "Queries not supported if permutation block size is non-trivial");
        TVector<NCB::TIndexRange<int>> indexRanges;

        const int permutedBlockCount = CeilDiv(docCount, NonCtrDataPermutationBlockSize);
        const int permutedBlocksPerCalcScoreBlock =
            CeilDiv(DefaultCalcStatsObjBlockSize, NonCtrDataPermutationBlockSize);

        int calcStatsBlockStart = 0;
        int blockStart = 0;
        for (int blockIdx : xrange(permutedBlockCount)) {
            const int permutedBlockIdx = (
                int(LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>()[blockStart]
                    - FeaturesSubsetBegin)
                / NonCtrDataPermutationBlockSize
            );
            const int nextBlockStart = blockStart +
               (permutedBlockIdx + 1 == permutedBlockCount ?
                  docCount - permutedBlockIdx * NonCtrDataPermutationBlockSize
                : NonCtrDataPermutationBlockSize);
            if ((blockIdx + 1) % permutedBlocksPerCalcScoreBlock == 0) {
                indexRanges.push_back(NCB::TIndexRange<int>(calcStatsBlockStart, nextBlockStart));
                calcStatsBlockStart = nextBlockStart;
            }
            blockStart = nextBlockStart;
        }
        if (calcStatsBlockStart != blockStart) {
            indexRanges.push_back(NCB::TIndexRange<int>(calcStatsBlockStart, blockStart));
        }

        CalcStatsIndexRanges.Reset(new NCB::TSavedIndexRanges<int>(std::move(indexRanges)));
    }
}

void TStats3D::Add(const TStats3D& stats3D) {
    CB_ENSURE(
        stats3D.BucketCount == BucketCount
        && stats3D.MaxLeafCount == MaxLeafCount
        && stats3D.Stats.ysize() == Stats.ysize()
        && stats3D.SplitEnsembleSpec == SplitEnsembleSpec,
        "SplitEnsembleSpec, SplitType, Leaf, bucket, dimension, and fold counts must match"
    );
    for (int statIdx = 0; statIdx < Stats.ysize(); ++statIdx) {
        Stats[statIdx].Add(stats3D.Stats[statIdx]);
    }
}
