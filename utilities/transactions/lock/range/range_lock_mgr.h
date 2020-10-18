//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#ifndef ROCKSDB_LITE

#include "utilities/transactions/lock/lock_mgr.h"
#include "utilities/transactions/lock/range/range_lock_tracker.h"

// Lock Tree library:
#include <locktree/lock_request.h>
#include <locktree/locktree.h>

namespace ROCKSDB_NAMESPACE {

using namespace toku;

/*
  A basic class for all Range-based lock managers.
*/
class RangeLockManagerBase : public BaseLockMgr {
 public:

  // Get a lock on a range
  // (TODO: TryLock accepts Env* parameter. Should this function do it, too?)
  virtual Status TryRangeLock(PessimisticTransaction* txn,
                              uint32_t column_family_id,
                              const Endpoint& start_endp,
                              const Endpoint& end_endp, bool exclusive) = 0;

  // Get a point lock
  // This is the same as getting a range lock on a single-point range.
  Status TryLock(PessimisticTransaction* txn, uint32_t column_family_id,
                 const std::string& key, Env*, bool exclusive) override {
    Endpoint endp(key.data(), key.size(), false);
    return TryRangeLock(txn, column_family_id, endp, endp, exclusive);
  }

  // Release all locks the transaction is holding
  virtual void UnLockAll(const PessimisticTransaction* txn, Env* env) = 0;
};

/*
  A lock manager that supports Range-based locking.
*/
class RangeLockMgr : public RangeLockManagerBase, public RangeLockMgrHandle {
 public:
  LockTrackerFactory* getLockTrackerFactory() override {
    return &RangeLockTrackerFactory::instance;
  }
  BaseLockMgr* getLockManager() override { return this; }
  RangeLockMgr* getRangeLockMgr() override { return this; }

  void AddColumnFamily(const ColumnFamilyHandle* cfh) override;
  void RemoveColumnFamily(const ColumnFamilyHandle* cfh) override;

  // Resize the deadlock-info buffer, does nothing currently
  void Resize(uint32_t) override;
  std::vector<DeadlockPath> GetDeadlockInfoBuffer() override;

  // Get a lock on a range
  //  @note only exclusive locks are currently supported (requesting a
  //  non-exclusive lock will get an exclusive one)
  Status TryRangeLock(PessimisticTransaction* txn, uint32_t column_family_id,
                      const Endpoint& start_endp, const Endpoint& end_endp,
                      bool exclusive) override;

  void UnLock(const PessimisticTransaction* txn, const LockTracker& tracker,
              Env* env) override;
  // Release all locks the transaction is holding
  void UnLockAll(const PessimisticTransaction* txn, Env* env) override;
  void UnLock(PessimisticTransaction* txn, uint32_t column_family_id,
              const std::string& key, Env* env) override;

  RangeLockMgr(std::shared_ptr<TransactionDBMutexFactory> mutex_factory);

  ~RangeLockMgr();

  int set_max_lock_memory(size_t max_lock_memory) override {
    return ltm_.set_max_lock_memory(max_lock_memory);
  }

  size_t get_max_lock_memory() { return ltm_.get_max_lock_memory(); }

  Counters GetStatus() override;

  LockStatusData GetLockStatusData() override;

 private:
  toku::locktree_manager ltm_;

  std::shared_ptr<TransactionDBMutexFactory> mutex_factory_;

  // Map from cf_id to locktree*. Can only be accessed while holding the
  // ltree_map_mutex_.
  using LockTreeMap = std::unordered_map<uint32_t, locktree*>;
  LockTreeMap ltree_map_;

  InstrumentedMutex ltree_map_mutex_;

  // Per-thread cache of ltree_map_.
  // (uses the same approach as TransactionLockMgr::lock_maps_cache_)
  std::unique_ptr<ThreadLocalPtr> ltree_lookup_cache_;

  DeadlockInfoBuffer dlock_buffer_;

  // Get the lock tree which stores locks for Column Family with given cf_id
  toku::locktree* get_locktree_by_cfid(uint32_t cf_id);

  static int compare_dbt_endpoints(void* arg, const DBT* a_key,
                                   const DBT* b_key);

  // Callbacks
  static int on_create(locktree*, void*) { return 0; /* no error */ }
  static void on_destroy(locktree*) {}
  static void on_escalate(TXNID txnid, const locktree* lt,
                          const range_buffer& buffer, void* extra);
};

void serialize_endpoint(const Endpoint& endp, std::string* buf);

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
