// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "utilities/transactions/lock/lock_tracker.h"

#include "utilities/transactions/lock/point_lock_tracker.h"

namespace ROCKSDB_NAMESPACE {

// psergey-todo: to make sure this is not created with a mis-matching lock
// manager, disallow and only allow explicit creator.

LockTracker* NewLockTracker() {
  // TODO: determine the lock tracker implementation based on configuration.
  return new PointLockTracker();
}

LockTrackerFactory::LockTrackerFactory() {
  CreateLockTracker = []() { return new PointLockTracker; };
}

}  // namespace ROCKSDB_NAMESPACE
