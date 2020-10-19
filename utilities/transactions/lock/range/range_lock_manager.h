//
// Generic definitions for a Range-based Lock Manager
//
#pragma once
#ifndef ROCKSDB_LITE

#include "utilities/transactions/lock/lock_manager.h"

namespace ROCKSDB_NAMESPACE {

/*
  A base class for all Range-based lock managers
*/
class RangeLockManagerBase : public LockManager {
 public:
  // Geting a point lock is reduced to getting a range lock on a single-point
  // range
  using LockManager::TryLock;
  Status TryLock(PessimisticTransaction* txn, ColumnFamilyId column_family_id,
                 const std::string& key, Env* env, bool exclusive) override{
    Endpoint endp(key.data(), key.size(), false);
    return TryLock(txn, column_family_id, endp, endp, env, exclusive);
  }

  // Release all locks the transaction is holding.
  // locktree has an optimized implementation for this, for the STO-mode.
  // TODO: check if this API call is needed.
  virtual void UnLockAll(const PessimisticTransaction* txn, Env* env) = 0;
};

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
