// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>
#include <filesystem>

namespace stellar
{

/**
 * Helper class that points to an output tempfile. Absorbs BucketEntries and
 * hashes them while writing to either destination. Produces a Bucket when done.
 */
template <typename BucketT>
BucketOutputIterator<BucketT>::BucketOutputIterator(std::string const& tmpDir,
                                                    bool keepTombstoneEntries,
                                                    BucketMetadata const& meta,
                                                    MergeCounters& mc,
                                                    asio::io_context& ctx,
                                                    bool doFsync)
    : mFilename(BucketBase::randomBucketName(tmpDir))
    , mOut(ctx, doFsync)
    , mCtx(ctx)
    , mBuf(nullptr)
    , mKeepTombstoneEntries(keepTombstoneEntries)
    , mMeta(meta)
    , mMergeCounters(mc)
{
    ZoneScoped;
    CLOG_TRACE(Bucket, "BucketOutputIterator opening file to write: {}",
               mFilename);
    // Will throw if unable to open the file
    mOut.open(mFilename.string());

    if (protocolVersionStartsFrom(
            meta.ledgerVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
    {

        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            BucketEntry bme;
            bme.type(METAENTRY);
            bme.metaEntry() = mMeta;
            put(bme);
        }
        else
        {
            static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                          "unexpected bucket type");
            releaseAssertOrThrow(protocolVersionStartsFrom(
                meta.ledgerVersion,
                BucketBase::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));

            HotArchiveBucketEntry bme;
            bme.type(HOT_ARCHIVE_METAENTRY);
            bme.metaEntry() = mMeta;
            put(bme);
        }

        mPutMeta = true;
    }
}

template <typename BucketT>
void
BucketOutputIterator<BucketT>::put(typename BucketT::EntryT const& e)
{
    ZoneScoped;

    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        LiveBucket::checkProtocolLegality(e, mMeta.ledgerVersion);
        if (e.type() == METAENTRY)
        {
            if (mPutMeta)
            {
                throw std::runtime_error(
                    "putting META entry in bucket after initial entry");
            }
        }

        if (!mKeepTombstoneEntries && BucketT::isTombstoneEntry(e))
        {
            ++mMergeCounters.mOutputIteratorTombstoneElisions;
            return;
        }
    }
    else
    {
        static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                      "unexpected bucket type");
        if (e.type() == HOT_ARCHIVE_METAENTRY)
        {
            if (mPutMeta)
            {
                throw std::runtime_error(
                    "putting META entry in bucket after initial entry");
            }
        }
        else
        {
            if (e.type() == HOT_ARCHIVE_ARCHIVED)
            {
                if (!isSorobanEntry(e.archivedEntry().data))
                {
                    throw std::runtime_error(
                        "putting non-soroban entry in hot archive bucket");
                }
            }
            else
            {
                if (!isSorobanEntry(e.key()))
                {
                    throw std::runtime_error(
                        "putting non-soroban entry in hot archive bucket");
                }
            }
        }

        // HOT_ARCHIVE_LIVE entries are dropped in the last bucket level
        // (similar to DEADENTRY) on live BucketLists
        if (!mKeepTombstoneEntries && BucketT::isTombstoneEntry(e))
        {
            ++mMergeCounters.mOutputIteratorTombstoneElisions;
            return;
        }
    }

    // Check to see if there's an existing buffered entry.
    if (mBuf)
    {
        // mCmp(e, *mBuf) means e < *mBuf; this should never be true since
        // it would mean that we're getting entries out of order.
        releaseAssert(!mCmp(e, *mBuf));

        // Check to see if the new entry should flush (greater identity), or
        // merely replace (same identity), the buffered entry.
        if (mCmp(*mBuf, e))
        {
            ++mMergeCounters.mOutputIteratorActualWrites;
            mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
            mObjectsPut++;
        }
    }
    else
    {
        mBuf = std::make_unique<typename BucketT::EntryT>();
    }

    // If BucketT is a live bucket, and this is the lowest level of the
    // bucketlist, we also want to convert each LIVEENTRY to an INITENTRY.
    // This is because each level of the bucket list contains only one entry
    // per key, and per CAP-0020, INITENTRY implies that no entry with
    // the same ledger key exists in an older bucket. Therefore, all entries
    // of type LIVEENTRY in the lowest level should be of type INITENTRY.
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        if (!mKeepTombstoneEntries /* lowest level */ &&
            e.type() == LIVEENTRY &&
            protocolVersionStartsFrom(
                mMeta.ledgerVersion,
                LiveBucket::
                    FIRST_PROTOCOL_CONVERTING_BOTTOM_LEVEL_LIVE_TO_INIT))
        {
            ++mMergeCounters.mOutputIteratorLiveToInitRewrites;
            ++mMergeCounters.mOutputIteratorBufferUpdates;
            auto eCopy = e;
            eCopy.type(INITENTRY);
            *mBuf = eCopy;
            return;
        }
    }

    // In any case, replace *mBuf with e.
    ++mMergeCounters.mOutputIteratorBufferUpdates;
    *mBuf = e;
}

template <typename BucketT>
std::shared_ptr<BucketT>
BucketOutputIterator<BucketT>::getBucket(BucketManager& bucketManager,
                                         bool shouldSynchronouslyIndex,
                                         MergeKey* mergeKey)
{
    ZoneScoped;
    if (mBuf)
    {
        mOut.writeOne(*mBuf, &mHasher, &mBytesPut);
        mObjectsPut++;
        mBuf.reset();
    }

    mOut.close();
    if (mObjectsPut == 0 || mBytesPut == 0)
    {
        releaseAssert(mObjectsPut == 0);
        releaseAssert(mBytesPut == 0);
        CLOG_DEBUG(Bucket, "Deleting empty bucket file {}", mFilename);
        std::filesystem::remove(mFilename);
        if (mergeKey)
        {
            bucketManager.noteEmptyMergeOutput<BucketT>(*mergeKey);
        }
        return std::make_shared<BucketT>();
    }

    auto hash = mHasher.finish();
    std::unique_ptr<BucketIndex const> index{};

    // If this bucket needs to be indexed and is not already indexed
    if (shouldSynchronouslyIndex)
    {
        // either it's a new bucket or we just reconstructed a bucket
        // we already have, in any case ensure we have an index
        if (auto b = bucketManager.getBucketIfExists<BucketT>(hash);
            !b || !b->isIndexed())
        {
            index = BucketIndex::createIndex<BucketT>(bucketManager, mFilename,
                                                      hash, mCtx);
        }
    }

    return bucketManager.adoptFileAsBucket<BucketT>(mFilename.string(), hash,
                                                    mergeKey, std::move(index));
}

template class BucketOutputIterator<LiveBucket>;
template class BucketOutputIterator<HotArchiveBucket>;
}
