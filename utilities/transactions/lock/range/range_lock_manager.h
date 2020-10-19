//
// Generic definitions for a Range-based Lock Manager
//
#pragma once
#ifndef ROCKSDB_LITE

#include "utilities/transactions/lock/lock_mgr.h"

namespace ROCKSDB_NAMESPACE {

/*
  A base class for all Range-based lock managers
*/
class RangeLockManagerBase : public BaseLockMgr {
 public:
  // Get a lock on a range
  // (TODO: TryLock accepts Env* parameter. Should this function do it, too?)
  virtual Status TryRangeLock(PessimisticTransaction* txn,
                              uint32_t column_family_id,
                              const Endpoint& start_endp,
                              const Endpoint& end_endp, bool exclusive) = 0;

  // Geting a point lock is reduced to getting a range lock on a single-point
  // range
  Status TryLock(PessimisticTransaction* txn, uint32_t column_family_id,
                 const std::string& key, Env*, bool exclusive) override {
    Endpoint endp(key.data(), key.size(), false);
    return TryRangeLock(txn, column_family_id, endp, endp, exclusive);
  }

  // Release all locks the transaction is holding
  virtual void UnLockAll(const PessimisticTransaction* txn, Env* env) = 0;
};

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
