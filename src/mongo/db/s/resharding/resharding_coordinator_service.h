/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/future.h"

namespace mongo {

class ServiceContext;
class OperationContext;

constexpr StringData kReshardingCoordinatorServiceName = "ReshardingCoordinatorService"_sd;

class ReshardingCoordinatorService final : public repl::PrimaryOnlyService {
public:
    explicit ReshardingCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingCoordinatorService() = default;

    StringData getServiceName() const override {
        return kReshardingCoordinatorServiceName;
    }
    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingCoordinatorService thread pool.
        return ThreadPool::Limits();
    }
    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override {
        return std::make_shared<ReshardingCoordinatorService::ReshardingCoordinator>(
            std::move(initialState));
    }

    class ReshardingCoordinator final
        : public PrimaryOnlyService::TypedInstance<ReshardingCoordinator> {
    public:
        explicit ReshardingCoordinator(const BSONObj& state);

        SharedSemiFuture<void> getCompletionPromise() {
            return _completionPromise.getFuture();
        }

        void run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept override;

        void setInitialChunksAndZones(std::vector<ChunkType> initialChunks,
                                      std::vector<TagsType> newZones);

    private:
        struct ChunksAndZones {
            std::vector<ChunkType> initialChunks;
            std::vector<TagsType> newZones;
        };

        /**
         * Does the following writes:
         * 1. Inserts coordinator state document into config.reshardingOperations
         * 2. Adds reshardingFields to the config.collections entry for the original collection
         * 3. Inserts an entry into config.collections for the temporary collection
         * 4. Inserts entries into config.chunks for ranges based on the new shard key
         * 5. Upserts entries into config.tags for any zones associated with the new shard key
         *
         * Transitions to 'kInitialized'.
         */
        ExecutorFuture<void> _init(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all recipients have created the
         * temporary collection. Transitions to 'kPreparingToDonate'.
         */
        ExecutorFuture<void> _awaitAllRecipientsCreatedCollection(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all donors have picked a
         * minFetchTimestamp and are ready to donate. Transitions to 'kCloning'.
         */
        ExecutorFuture<void> _awaitAllDonorsReadyToDonate(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all recipients have finished
         * cloning. Transitions to 'kMirroring'.
         */
        ExecutorFuture<void> _awaitAllRecipientsFinishedCloning(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all recipients have entered
         * strict-consistency.
         */
        SharedSemiFuture<void> _awaitAllRecipientsInStrictConsistency(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Does the following writes:
         * 1. Updates the config.collections entry for the new sharded collection
         * 2. Updates config.chunks entries for the new sharded collection
         * 3. Updates config.tags for the new sharded collection
         *
         * Transitions to 'kCommitted'.
         */
        Future<void> _commit();

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all recipients have renamed the
         * temporary collection to the original collection namespace. Transitions to 'kDropping'.
         */
        ExecutorFuture<void> _awaitAllRecipientsRenamedCollection(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Waits on _reshardingCoordinatorObserver to notify that all donors have dropped the
         * original collection. Transitions to 'kDone'.
         */
        ExecutorFuture<void> _awaitAllDonorsDroppedOriginalCollection(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * Updates the entry for this resharding operation in config.reshardingOperations and the
         * catalog entries for the original and temporary namespaces in config.collections.
         */
        void _runUpdates(CoordinatorStateEnum nextState,
                         boost::optional<Timestamp> fetchTimestamp = boost::none);

        /**
         * Marks the state doc as garbage collectable so that it can be cleaned up by the TTL
         * monitor.
         */
        void _markCoordinatorStateDocAsGarbageCollectable();

        /**
         * Removes the 'reshardingFields' from the config.collections entry.
         */
        void _removeReshardingFields();

        /**
         * Sends 'flushRoutingTableCacheUpdates' for the temporary namespace to all recipient
         * shards.
         */
        void _tellAllRecipientsToRefresh();

        /**
         * Sends 'flushRoutingTableCacheUpdates' for the original namespace to all donor shards.
         */
        void _tellAllDonorsToRefresh();

        // The unique key for a given resharding operation. InstanceID is an alias for BSONObj. The
        // value of this is the UUID that will be used as the collection UUID for the new sharded
        // collection. The object looks like: {_id: 'reshardingUUID'}
        const InstanceID _id;

        // Promise containing the initial chunks and new zones based on the new shard key. These are
        // not a part of the state document, so must be set by configsvrReshardCollection after
        // construction.
        SharedPromise<ChunksAndZones> _initialChunksAndZonesPromise;

        // Observes writes that indicate state changes for this resharding operation and notifies
        // 'this' when all donors/recipients have entered some state so that 'this' can transition
        // states.
        std::shared_ptr<ReshardingCoordinatorObserver> _reshardingCoordinatorObserver;

        // The updated coordinator state document.
        ReshardingCoordinatorDocument _stateDoc;

        // Fulfilled only after transitioned to kDone or kError
        SharedPromise<void> _completionPromise;
    };
};

}  // namespace mongo
