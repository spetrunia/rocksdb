#include "utilities/transactions/lock/range/range_lock_tracker.h"

#include "utilities/transactions/lock/range/range_lock_mgr.h"

namespace ROCKSDB_NAMESPACE {

RangeLockTrackerFactory RangeLockTrackerFactory::instance;

RangeLockList *RangeLockTracker::getOrCreateList() {
  RangeLockList *res;
  if ((res = getList())) return res;

  // Doesn't exist, create
  range_list.reset(new RangeLockList());
  return getList();
}

void RangeLockTracker::Track(const PointLockRequest &lock_req) {
  DBT key_dbt;
  std::string key;
  serialize_endpoint(Endpoint(lock_req.key, false), &key);
  toku_fill_dbt(&key_dbt, key.data(), key.size());
  RangeLockList *rl = getOrCreateList();
  rl->append(lock_req.column_family_id, &key_dbt, &key_dbt);
}

void RangeLockTracker::Track(const RangeLockRequest &lock_req) {
  DBT start_dbt, end_dbt;
  std::string start_key, end_key;

  serialize_endpoint(lock_req.start_endp, &start_key);
  serialize_endpoint(lock_req.end_endp, &end_key);

  toku_fill_dbt(&start_dbt, start_key.data(), start_key.size());
  toku_fill_dbt(&end_dbt, end_key.data(), end_key.size());

  RangeLockList *rl = getOrCreateList();
  rl->append(lock_req.column_family_id, &start_dbt, &end_dbt);
}

PointLockStatus RangeLockTracker::GetPointLockStatus(
    ColumnFamilyId /*cf_id*/, const std::string & /*key*/) const {
  // TODO: what to do here if we are holding a range lock that is embedding the
  // point?

  // "Cheat" and return the status which says the point is not locked.
  PointLockStatus p;
  p.locked = false;
  p.exclusive = true;
  p.seq = 0;
  return p;
}

void RangeLockTracker::Clear() {
  // This will delete the RangeLockList and cause a proper cleanup
  range_list = nullptr;
}

}  // namespace ROCKSDB_NAMESPACE
