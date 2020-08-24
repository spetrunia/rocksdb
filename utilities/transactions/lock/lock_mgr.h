//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#ifndef ROCKSDB_LITE

#include <string>
#include <unordered_map>
#include <memory>
#include <utility>
#include <vector>

#include "rocksdb/utilities/transaction.h"
#include "util/thread_local.h"
#include "utilities/transactions/lock/lock_tracker.h"
#include "utilities/transactions/pessimistic_transaction.h"

// This file contains a generic Lock Manager interface

namespace ROCKSDB_NAMESPACE {

class ColumnFamilyHandle;

struct DeadlockInfoBuffer {
 private:
  std::vector<DeadlockPath> paths_buffer_;
  uint32_t buffer_idx_;
  std::mutex paths_buffer_mutex_;
  std::vector<DeadlockPath> Normalize();

 public:
  explicit DeadlockInfoBuffer(uint32_t n_latest_dlocks)
      : paths_buffer_(n_latest_dlocks), buffer_idx_(0) {}
  void AddNewPath(DeadlockPath path);
  void Resize(uint32_t target_size);
  std::vector<DeadlockPath> PrepareBuffer();
};

//
// Base class for Point and Range-based lock manager.
//
class BaseLockMgr {
 public:
  virtual LockTrackerFactory* getLockTrackerFactory() = 0;

  virtual void AddColumnFamily(const ColumnFamilyHandle* cfh) = 0;
  virtual void RemoveColumnFamily(const ColumnFamilyHandle* cfh) = 0;

  virtual Status TryLock(PessimisticTransaction* txn, uint32_t column_family_id,
                         const std::string& key, Env* env, bool exclusive) = 0;
  virtual void UnLock(const PessimisticTransaction* txn,
                      const LockTracker& tracker, Env* env) = 0;
  virtual void UnLock(PessimisticTransaction* txn, uint32_t column_family_id,
                      const std::string& key, Env* env) = 0;

  // Resize the deadlock info buffer
  virtual void Resize(uint32_t) = 0;
  virtual std::vector<DeadlockPath> GetDeadlockInfoBuffer() = 0;

  // TransactionDB will call this at start
  virtual void init(TransactionDB*){};
  virtual ~BaseLockMgr() {}

  using LockStatusData = std::unordered_multimap<uint32_t, KeyLockInfo>;
  virtual LockStatusData GetLockStatusData() = 0;
};

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
