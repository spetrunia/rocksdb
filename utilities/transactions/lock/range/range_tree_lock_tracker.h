// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "util/mutexlock.h"
#include "utilities/transactions/lock/lock_tracker.h"

// Range Locking:
#include "locktree/lock_request.h"
#include "locktree/locktree.h"

namespace ROCKSDB_NAMESPACE {

class RangeLockList;

/*
  Storage for locks that are currently held by a transaction.

  Locks are kept in toku::range_buffer because toku::locktree::release_locks()
  accepts that as an argument.

  Note: the list of locks may differ slighly from the contents of the lock
  tree, due to concurrency between lock acquisition, lock release, and lock
  escalation. See MDEV-18227 and RangeTreeLockManager::UnLockAll for details.
  This property is currently harmless.
*/
class RangeLockList /*: public PessimisticTransaction::Lock--Storage */ {
 public:
  virtual ~RangeLockList() { clear(); }

  void clear() {
    for (auto it : buffers_) {
      it.second->destroy();
    }
    buffers_.clear();
  }

  RangeLockList() : releasing_locks_(false) {}

  void append(uint32_t cf_id, const DBT* left_key, const DBT* right_key) {
    MutexLock l(&mutex_);
    // there's only one thread that calls this function.
    // the same thread will do lock release.
    assert(!releasing_locks_);
    auto it = buffers_.find(cf_id);
    if (it == buffers_.end()) {
      // create a new one
      it = buffers_
               .emplace(cf_id, std::shared_ptr<toku::range_buffer>(
                                   new toku::range_buffer()))
               .first;
      it->second->create();
    }
    it->second->append(left_key, right_key);
  }

  std::unordered_map<uint32_t, std::shared_ptr<toku::range_buffer>> buffers_;

  // Synchronization. See RangeTreeLockManager::UnLockAll for details
  port::Mutex mutex_;
  bool releasing_locks_;
};

// Tracks range locks

class RangeTreeLockTracker : public LockTracker {
 public:
  RangeTreeLockTracker() : range_list(nullptr) {}

  RangeTreeLockTracker(const RangeTreeLockTracker&) = delete;
  RangeTreeLockTracker& operator=(const RangeTreeLockTracker&) = delete;

  void Track(const PointLockRequest&) override;
  void Track(const RangeLockRequest&) override;

  bool IsPointLockSupported() const override { return false; }
  bool IsRangeLockSupported() const override { return true; }

  // a Not-supported dummy implementation.
  UntrackStatus Untrack(const RangeLockRequest& /*lock_request*/) override {
    return UntrackStatus::NOT_TRACKED;
  }

  UntrackStatus Untrack(const PointLockRequest& /*lock_request*/) override {
    return UntrackStatus::NOT_TRACKED;
  }

  // "If this method is not supported, leave it as a no-op."
  void Merge(const LockTracker&) override {}

  // "If this method is not supported, leave it as a no-op."
  void Subtract(const LockTracker&) override {}

  void Clear() override;

  // "If this method is not supported, returns nullptr."
  virtual LockTracker* GetTrackedLocksSinceSavePoint(
      const LockTracker&) const override {
    return nullptr;
  }

  PointLockStatus GetPointLockStatus(ColumnFamilyId column_family_id,
                                     const std::string& key) const override;

  // The return value is only used for tests
  uint64_t GetNumPointLocks() const override { return 0; }

  ColumnFamilyIterator* GetColumnFamilyIterator() const override {
    return nullptr;
  }

  KeyIterator* GetKeyIterator(
      ColumnFamilyId /*column_family_id*/) const override {
    return nullptr;
  }

  // Non-override (TODO: make private!)
  RangeLockList* getList() { return range_list.get(); }
  RangeLockList* getOrCreateList();

 private:
  std::shared_ptr<RangeLockList> range_list;
};

class RangeTreeLockTrackerFactory : public LockTrackerFactory {
 public:
  LockTracker* Create() const override { return new RangeTreeLockTracker; }

  static RangeTreeLockTrackerFactory instance;
};

}  // namespace ROCKSDB_NAMESPACE
