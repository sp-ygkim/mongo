/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/s/global_index_cache.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/decorable.h"

namespace mongo {

enum class CSRAcquisitionMode { kShared, kExclusive };

/**
 * See the comments for CollectionShardingState for more information on how this class fits in the
 * sharding architecture.
 */
class CollectionShardingRuntime final : public CollectionShardingState,
                                        public Decorable<CollectionShardingRuntime> {
    CollectionShardingRuntime(const CollectionShardingRuntime&) = delete;
    CollectionShardingRuntime& operator=(const CollectionShardingRuntime&) = delete;

public:
    CollectionShardingRuntime(ServiceContext* service,
                              NamespaceString nss,
                              std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor);

    /**
     * Obtains the sharding runtime for the specified collection, along with a resource lock
     * protecting it from concurrent modifications, which will be held until the object goes out of
     * scope.
     */
    class ScopedCollectionShardingRuntime {
    public:
        ScopedCollectionShardingRuntime(ScopedCollectionShardingRuntime&&) = default;

        CollectionShardingRuntime* operator->() const {
            return checked_cast<CollectionShardingRuntime*>(&*_scopedCss);
        }
        CollectionShardingRuntime& operator*() const {
            return checked_cast<CollectionShardingRuntime&>(*_scopedCss);
        }

    private:
        friend class CollectionShardingRuntime;

        ScopedCollectionShardingRuntime(ScopedCollectionShardingState&& scopedCss);

        ScopedCollectionShardingState _scopedCss;
    };
    static ScopedCollectionShardingRuntime assertCollectionLockedAndAcquire(
        OperationContext* opCtx, const NamespaceString& nss, CSRAcquisitionMode mode);
    static ScopedCollectionShardingState acquire(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 CSRAcquisitionMode mode) = delete;

    const NamespaceString& nss() const override {
        return _nss;
    }

    ScopedCollectionDescription getCollectionDescription(OperationContext* opCtx) override;

    ScopedCollectionFilter getOwnershipFilter(OperationContext* opCtx,
                                              OrphanCleanupPolicy orphanCleanupPolicy,
                                              bool supportNonVersionedOperations) override;

    void checkShardVersionOrThrow(OperationContext* opCtx) override;

    void appendShardVersion(BSONObjBuilder* builder) override;

    size_t numberOfRangesScheduledForDeletion() const override;

    /**
     * Returns boost::none if the description for the collection is not known yet. Otherwise
     * returns the most recently refreshed from the config server metadata.
     *
     * This method do not check for the shard version that the operation requires and should only
     * be used for cases such as checking whether a particular config server update has taken
     * effect.
     */
    boost::optional<CollectionMetadata> getCurrentMetadataIfKnown();

    /**
     * Updates the collection's filtering metadata based on changes received from the config server
     * and also resolves the pending receives map in case some of these pending receives have
     * committed on the config server or have been abandoned by the donor shard.
     *
     * This method must be called with an exclusive collection lock and it does not acquire any
     * locks itself.
     */
    void setFilteringMetadata(OperationContext* opCtx, CollectionMetadata newMetadata);

    /**
     * Marks the collection's filtering metadata as UNKNOWN, meaning that all attempts to check for
     * shard version match will fail with StaleConfig errors in order to trigger an update.
     *
     * Interrupts any ongoing shard metadata refresh.
     *
     * It is safe to call this method with only an intent lock on the collection (as opposed to
     * setFilteringMetadata which requires exclusive).
     */
    void clearFilteringMetadata(OperationContext* opCtx);

    /**
     * Calls to clearFilteringMetadata + clears the _metadataManager object.
     */
    void clearFilteringMetadataForDroppedCollection(OperationContext* opCtx);

    /**
     * Methods to control the collection's critical section. Methods listed below must be called
     * with both the collection lock and CSRLock held in exclusive mode.
     *
     * In these methods, the CSRLock ensures concurrent access to the critical section.
     *
     * Entering into the Critical Section interrupts any ongoing filtering metadata refresh.
     */
    void enterCriticalSectionCatchUpPhase(const BSONObj& reason);
    void enterCriticalSectionCommitPhase(const BSONObj& reason);

    /**
     * It transitions the critical section back to the catch up phase.
     */
    void rollbackCriticalSectionCommitPhaseToCatchUpPhase(const BSONObj& reason);

    /**
     * Method to control the collection's critical section. Methods listed below must be called with
     * both the collection lock and CSR acquired in exclusive mode.
     */
    void exitCriticalSection(const BSONObj& reason);

    /**
     * Same semantics than 'exitCriticalSection' but without doing error-checking. Only meant to be
     * used when recovering the critical sections in the RecoverableCriticalSectionService.
     */
    void exitCriticalSectionNoChecks();

    /**
     * If the collection is currently in a critical section, returns the critical section signal to
     * be waited on. Otherwise, returns nullptr.
     */
    boost::optional<SharedSemiFuture<void>> getCriticalSectionSignal(
        OperationContext* opCtx, ShardingMigrationCriticalSection::Operation op);

    /**
     * Schedules documents in `range` for cleanup after any running queries that may depend on them
     * have terminated. Does not block. Fails if range overlaps any current local shard chunk.
     * Passed kDelayed, an additional delay (configured via server parameter orphanCleanupDelaySecs)
     * is added to permit (most) dependent queries on secondaries to complete, too.
     *
     * Returns a future that will be resolved when the deletion completes or fails. If that
     * succeeds, waitForClean can be called to ensure no other deletions are pending for the range.
     */
    enum CleanWhen { kNow, kDelayed };
    SharedSemiFuture<void> cleanUpRange(ChunkRange const& range, CleanWhen when);

    /**
     * Waits for all ranges deletion tasks with UUID 'collectionUuid' overlapping range
     * 'orphanRange' to be processed, even if the collection does not exist in the storage catalog.
     * It will block until the minimum of the operation context's timeout deadline or 'deadline' is
     * reached.
     */
    static Status waitForClean(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& collectionUuid,
                               ChunkRange orphanRange,
                               Date_t deadline);

    /**
     * Returns a future marked as ready when all the ongoing queries retaining the range complete
     */
    SharedSemiFuture<void> getOngoingQueriesCompletionFuture(const UUID& collectionUuid,
                                                             ChunkRange const& range);

    std::uint64_t getNumMetadataManagerChanges_forTest() {
        return _numMetadataManagerChanges;
    }

    /**
     * Initializes the shard version recover/refresh shared semifuture for other threads to wait on
     * it.
     *
     * To invoke this method, the criticalSectionSignal must not be hold by a different thread.
     */
    void setShardVersionRecoverRefreshFuture(SharedSemiFuture<void> future,
                                             CancellationSource cancellationSource);

    /**
     * If there an ongoing shard version recover/refresh, it returns the shared semifuture to be
     * waited on. Otherwise, returns boost::none.
     */
    boost::optional<SharedSemiFuture<void>> getShardVersionRecoverRefreshFuture(
        OperationContext* opCtx);

    /**
     * Resets the shard version recover/refresh shared semifuture to boost::none.
     */
    void resetShardVersionRecoverRefreshFuture();

    /**
     * Gets an index version under a lock.
     */
    boost::optional<CollectionIndexes> getCollectionIndexes(OperationContext* opCtx);

    /**
     * Gets the index list under a lock.
     */
    boost::optional<GlobalIndexesCache>& getIndexes(OperationContext* opCtx);

    /**
     * Add a new index to the shard-role index info under a lock.
     */
    void addIndex(OperationContext* opCtx,
                  const IndexCatalogType& index,
                  const CollectionIndexes& collectionIndexes);

    /**
     * Removes an index from the shard-role index info under a lock.
     */
    void removeIndex(OperationContext* opCtx,
                     const std::string& name,
                     const CollectionIndexes& collectionIndexes);

    /**
     * Clears the shard-role index info and set the collectionIndexes to boost::none.
     */
    void clearIndexes(OperationContext* opCtx);

    /**
     * Clears all the indexes and set the new indexes and index version.
     */
    void replaceIndexes(OperationContext* opCtx,
                        const std::vector<IndexCatalogType>& indexes,
                        const CollectionIndexes& collectionIndexes);

private:
    struct ShardVersionRecoverOrRefresh {
    public:
        ShardVersionRecoverOrRefresh(SharedSemiFuture<void> future,
                                     CancellationSource cancellationSource)
            : future(std::move(future)), cancellationSource(std::move(cancellationSource)){};

        // Tracks ongoing shard version recover/refresh.
        SharedSemiFuture<void> future;

        // Cancellation source to cancel the ongoing recover/refresh shard version.
        CancellationSource cancellationSource;
    };

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> _getCurrentMetadataIfKnown(
        const boost::optional<LogicalTime>& atClusterTime);

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified. Throws StaleConfigInfo if the shard version attached to the
     * operation context does not match the shard version on the active metadata object.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> _getMetadataWithVersionCheckAt(
        OperationContext* opCtx,
        const boost::optional<mongo::LogicalTime>& atClusterTime,
        bool supportNonVersionedOperations = false);

    /**
     * Auxiliary function used to implement the different flavours of clearFilteringMetadata.
     */
    void _clearFilteringMetadata(OperationContext* opCtx, bool collIsDropped);

    /**
     * This function cleans up some state associated with the current sharded metadata before it's
     * replaced by the new metadata.
     */
    void _cleanupBeforeInstallingNewCollectionMetadata(WithLock, OperationContext* opCtx);

    // The service context under which this instance runs
    ServiceContext* const _serviceContext;

    // Namespace this state belongs to.
    const NamespaceString _nss;

    // The executor used for deleting ranges of orphan chunks.
    std::shared_ptr<executor::TaskExecutor> _rangeDeleterExecutor;

    // Tracks the migration critical section state for this collection.
    ShardingMigrationCriticalSection _critSec;

    // Protects state around the metadata manager below
    mutable Mutex _metadataManagerLock =
        MONGO_MAKE_LATCH("CollectionShardingRuntime::_metadataManagerLock");

    // Tracks whether the filtering metadata is unknown, unsharded, or sharded
    enum class MetadataType { kUnknown, kUnsharded, kSharded } _metadataType;

    // If the collection state is known and is unsharded, this will be nullptr.
    //
    // If the collection state is known and is sharded, this will point to the metadata associated
    // with this collection.
    //
    // If the collection state is unknown:
    // - If the metadata had never been set yet, this will be nullptr.
    // - If the collection state was known and was sharded, this contains the metadata that
    // were known for the collection before the last invocation of clearFilteringMetadata().
    //
    // The following matrix enumerates the valid (Y) and invalid (X) scenarios.
    //                          _________________________________
    //                         | _metadataType (collection state)|
    //                         |_________________________________|
    //                         | UNKNOWN | UNSHARDED |  SHARDED  |
    //  _______________________|_________|___________|___________|
    // |_metadataManager unset |    Y    |     Y     |     X     |
    // |_______________________|_________|___________|___________|
    // |_metadataManager set   |    Y    |     X     |     Y     |
    // |_______________________|_________|___________|___________|
    std::shared_ptr<MetadataManager> _metadataManager;

    // Used for testing to check the number of times a new MetadataManager has been installed.
    std::uint64_t _numMetadataManagerChanges{0};

    // Tracks ongoing shard version recover/refresh. Eventually set to the semifuture to wait on and
    // a CancellationSource to cancel it
    boost::optional<ShardVersionRecoverOrRefresh> _shardVersionInRecoverOrRefresh;

    // Contains the global indexes for the collection. This will be boost::none if no global indexes
    // have ever been created for the collection.
    boost::optional<GlobalIndexesCache> _globalIndexesInfo;
};

/**
 * RAII-style class, which obtains a reference to the critical section for the specified collection.
 *
 *
 * Shard version recovery/refresh procedures always wait for the critical section to be released in
 * order to serialise with concurrent moveChunk/shardCollection commit operations.
 *
 * Entering the critical section doesn't serialise with concurrent recovery/refresh, because
 * causally such refreshes would have happened *before* the critical section was entered.
 */
class CollectionCriticalSection {
    CollectionCriticalSection(const CollectionCriticalSection&) = delete;
    CollectionCriticalSection& operator=(const CollectionCriticalSection&) = delete;

public:
    CollectionCriticalSection(OperationContext* opCtx, NamespaceString nss, BSONObj reason);
    ~CollectionCriticalSection();

    /**
     * Enters the commit phase of the critical section and blocks reads.
     */
    void enterCommitPhase();

private:
    OperationContext* const _opCtx;

    NamespaceString _nss;
    const BSONObj _reason;
};

}  // namespace mongo
