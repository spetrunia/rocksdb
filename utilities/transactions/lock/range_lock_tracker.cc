#include "utilities/transactions/lock/range_lock_tracker.h"

namespace ROCKSDB_NAMESPACE {

RangeLockList *RangeLockTracker::getOrCreateList() {
  RangeLockList *res;
  if ((res = getList()))
    return res;

  // Doesn't exist, create
  range_list.reset(new RangeLockList());
  return getList();
}


void RangeLockTracker::Track(const PointLockRequest&) {
  // TODO: don't do anything here as we've already saved the lock?
}

PointLockStatus RangeLockTracker::GetPointLockStatus(ColumnFamilyId
/*column_family_id*/,
                                     const std::string& /*key*/) const {
  // TODO: what to do here if we are holding a range lock that is embedding the
  // point?
  return PointLockStatus();
}

void RangeLockTracker::Clear() {
  // TODO: if we are the "primary" then clear.
}

}  // namespace ROCKSDB_NAMESPACE
