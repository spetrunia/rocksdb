//  Copyright (c) 2020-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "utilities/transactions/transaction_lock_mgr.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/utilities/transaction_db.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "utilities/transactions/transaction_db_mutex_impl.h"

namespace ROCKSDB_NAMESPACE {

class TransactionLockMgrTest : public testing::Test {
 public:
  void SetUp() override {
    env_ = Env::Default();
    db_dir_ = test::PerThreadDBPath("transaction_lock_mgr_test");
    ASSERT_OK(env_->CreateDir(db_dir_));
    mutex_factory_ = std::make_shared<TransactionDBMutexFactoryImpl>();

    Options opt;
    opt.create_if_missing = true;
    TransactionDBOptions txn_opt;
    txn_opt.transaction_lock_timeout = 0;

    if (use_range_locking) {
      /*
        With Range Locking, we must use the same lock manager object that the
        TransactionDB is using.
        Create it here and pass it to the database through lock_mgr_handle.
      */
      locker_.reset(new RangeLockMgr(mutex_factory_));
      range_lock_mgr = std::dynamic_pointer_cast<RangeLockMgrHandle>(locker_);
      txn_opt.lock_mgr_handle = range_lock_mgr;
    }

    ASSERT_OK(TransactionDB::Open(opt, txn_opt, db_dir_, &db_));

    if (!use_range_locking) {
      // If not using range locking, this test creates a separate lock manager
      // object (right, NOT the one TransactionDB is using!), and runs tests on
      // that.
      locker_ = std::shared_ptr<TransactionLockMgr>(
                    new TransactionLockMgr(db_, txn_opt.num_stripes,
                                           txn_opt.max_num_locks,
                                           txn_opt.max_num_deadlocks,
                                           mutex_factory_));
    }
  }

  void TearDown() override {
    delete db_;
    EXPECT_OK(test::DestroyDir(env_, db_dir_));
  }

  PessimisticTransaction* NewTxn(
      TransactionOptions txn_opt = TransactionOptions()) {
    Transaction* txn = db_->BeginTransaction(WriteOptions(), txn_opt);
    return reinterpret_cast<PessimisticTransaction*>(txn);
  }

 protected:
  Env* env_;
  std::shared_ptr<BaseLockMgr> locker_;
  bool use_range_locking;
  std::shared_ptr<rocksdb::RangeLockMgrHandle> range_lock_mgr;

  std::string key_value(const std::string &s) {
    if (use_range_locking)
      return s.substr(1);
    else
      return s;
  }

 private:
  std::string db_dir_;
  std::shared_ptr<TransactionDBMutexFactory> mutex_factory_;
  TransactionDB* db_;
};

class AnyLockManagerTest : public TransactionLockMgrTest,
                           public testing::WithParamInterface<bool> {
public:
  AnyLockManagerTest() {
    use_range_locking = GetParam();
  }
};


class MockColumnFamily : public ColumnFamilyHandle {
  uint32_t id_;
  std::string name;
 public:
  ~MockColumnFamily() {}
  MockColumnFamily(uint32_t id_arg) : id_(id_arg) {}
  uint32_t GetID() const override { return id_; }

  const std::string& GetName() const override { return name; }

  Status GetDescriptor(ColumnFamilyDescriptor* ) override {
    assert(0);
    return Status::NotSupported();
  }

  const Comparator* GetComparator() const override {
    return BytewiseComparator();
  };
};

MockColumnFamily cf_1024(1024);
MockColumnFamily cf_2048(2048);

MockColumnFamily cf_1(1);

// This test is not applicable for Range Lock manager as Range Lock Manager
// operates on Column Families, not their ids.
TEST_F(TransactionLockMgrTest, LockNonExistingColumnFamily) {
  locker_->RemoveColumnFamily(&cf_1024);
  auto txn = NewTxn();
  auto s = locker_->TryLock(txn, 1024, "k", env_, true);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STREQ(s.getState(), "Column family id not found: 1024");
  delete txn;
}


TEST_P(AnyLockManagerTest, LockStatus) {
  locker_->AddColumnFamily(&cf_1024);
  locker_->AddColumnFamily(&cf_2048);

  auto txn1 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn1, 1024, "k1", env_, true));
  ASSERT_OK(locker_->TryLock(txn1, 2048, "k1", env_, true));

  auto txn2 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn2, 1024, "k2", env_, false));
  ASSERT_OK(locker_->TryLock(txn2, 2048, "k2", env_, false));

  auto s = locker_->GetLockStatusData();
  ASSERT_EQ(s.size(), 4u);
  for (uint32_t cf_id : {1024, 2048}) {
    ASSERT_EQ(s.count(cf_id), 2u);
    auto range = s.equal_range(cf_id);
    for (auto it = range.first; it != range.second; it++) {
      ASSERT_TRUE(key_value(it->second.key) == "k1" ||
                  key_value(it->second.key) == "k2");
      if (key_value(it->second.key) == "k1") {
        ASSERT_EQ(it->second.exclusive, true);
        ASSERT_EQ(it->second.ids.size(), 1u);
        ASSERT_EQ(it->second.ids[0], txn1->GetID());
      } else if (key_value(it->second.key) == "k2") {
        ASSERT_EQ(it->second.exclusive, false);
        ASSERT_EQ(it->second.ids.size(), 1u);
        ASSERT_EQ(it->second.ids[0], txn2->GetID());
      }
    }
  }

  // Cleanup
  locker_->UnLock(txn1, 1024, "k1", env_);
  locker_->UnLock(txn1, 2048, "k1", env_);
  locker_->UnLock(txn2, 1024, "k2", env_);
  locker_->UnLock(txn2, 2048, "k2", env_);

  delete txn1;
  delete txn2;
}

TEST_P(AnyLockManagerTest, UnlockExclusive) {
  locker_->AddColumnFamily(&cf_1);

  auto txn1 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn1, 1, "k", env_, true));
  locker_->UnLock(txn1, 1, "k", env_);

  auto txn2 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn2, 1, "k", env_, true));

  // Cleanup
  locker_->UnLock(txn2, 1, "k", env_);

  delete txn1;
  delete txn2;
}

TEST_P(AnyLockManagerTest, UnlockShared) {
  locker_->AddColumnFamily(&cf_1);

  auto txn1 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn1, 1, "k", env_, false));
  locker_->UnLock(txn1, 1, "k", env_);

  auto txn2 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn2, 1, "k", env_, true));

  // Cleanup
  locker_->UnLock(txn2, 1, "k", env_);

  delete txn1;
  delete txn2;
}

TEST_P(AnyLockManagerTest, ReentrantExclusiveLock) {
  // Tests that a txn can acquire exclusive lock on the same key repeatedly.
  locker_->AddColumnFamily(&cf_1);
  auto txn = NewTxn();
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, true));
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, true));

  // Cleanup
  locker_->UnLock(txn, 1, "k", env_);

  delete txn;
}

TEST_P(AnyLockManagerTest, ReentrantSharedLock) {
  // Tests that a txn can acquire shared lock on the same key repeatedly.
  locker_->AddColumnFamily(&cf_1);
  auto txn = NewTxn();
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, false));
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, false));

  // Cleanup
  locker_->UnLock(txn, 1, "k", env_);

  delete txn;
}

TEST_P(AnyLockManagerTest, LockUpgrade) {
  // Tests that a txn can upgrade from a shared lock to an exclusive lock.
  locker_->AddColumnFamily(&cf_1);
  auto txn = NewTxn();
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, false));
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, true));

  // Cleanup
  locker_->UnLock(txn, 1, "k", env_);
  delete txn;
}

TEST_P(AnyLockManagerTest, LockDowngrade) {
  // Tests that a txn can acquire a shared lock after acquiring an exclusive
  // lock on the same key.
  locker_->AddColumnFamily(&cf_1);
  auto txn = NewTxn();
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, true));
  ASSERT_OK(locker_->TryLock(txn, 1, "k", env_, false));

  // Cleanup
  locker_->UnLock(txn, 1, "k", env_);
  delete txn;
}

TEST_P(AnyLockManagerTest, LockConflict) {
  // Tests that lock conflicts lead to lock timeout.
  locker_->AddColumnFamily(&cf_1);
  auto txn1 = NewTxn();
  auto txn2 = NewTxn();

  {
    // exclusive-exclusive conflict.
    ASSERT_OK(locker_->TryLock(txn1, 1, "k1", env_, true));
    auto s = locker_->TryLock(txn2, 1, "k1", env_, true);
    ASSERT_TRUE(s.IsTimedOut());
  }

  {
    // exclusive-shared conflict.
    ASSERT_OK(locker_->TryLock(txn1, 1, "k2", env_, true));
    auto s = locker_->TryLock(txn2, 1, "k2", env_, false);
    ASSERT_TRUE(s.IsTimedOut());
  }

  {
    // shared-exclusive conflict.
    ASSERT_OK(locker_->TryLock(txn1, 1, "k2", env_, false));
    auto s = locker_->TryLock(txn2, 1, "k2", env_, true);
    ASSERT_TRUE(s.IsTimedOut());
  }

  // Cleanup
  locker_->UnLock(txn1, 1, "k1", env_);
  locker_->UnLock(txn1, 1, "k2", env_);

  delete txn1;
  delete txn2;
}

port::Thread BlockUntilWaitingTxn(bool use_range_locking,
                                  std::function<void()> f) {
  std::atomic<bool> reached(false);
  const char* sync_point_name =
   use_range_locking? "RangeLockMgr::TryRangeLock:WaitingTxn":
                      "TransactionLockMgr::AcquireWithTimeout:WaitingTxn";
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      sync_point_name,
      [&](void* /*arg*/) { reached.store(true); });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  port::Thread t(f);

  while (!reached.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();

  return t;
}

TEST_P(AnyLockManagerTest, SharedLocks) {
  // Tests that shared locks can be concurrently held by multiple transactions.
  locker_->AddColumnFamily(&cf_1);
  auto txn1 = NewTxn();
  auto txn2 = NewTxn();
  ASSERT_OK(locker_->TryLock(txn1, 1, "k", env_, false));
  ASSERT_OK(locker_->TryLock(txn2, 1, "k", env_, false));
  delete txn1;
  delete txn2;

  // Cleanup
  locker_->UnLock(txn1, 1, "k", env_);
  locker_->UnLock(txn2, 1, "k", env_);

}

TEST_P(AnyLockManagerTest, Deadlock) {
  // Tests that deadlock can be detected.
  // Deadlock scenario:
  // txn1 exclusively locks k1, and wants to lock k2;
  // txn2 exclusively locks k2, and wants to lock k1.
  locker_->AddColumnFamily(&cf_1);
  TransactionOptions txn_opt;
  txn_opt.deadlock_detect = true;
  txn_opt.lock_timeout = 1000000;
  auto txn1 = NewTxn(txn_opt);
  auto txn2 = NewTxn(txn_opt);

  ASSERT_OK(locker_->TryLock(txn1, 1, "k1", env_, true));
  ASSERT_OK(locker_->TryLock(txn2, 1, "k2", env_, true));

  // txn1 tries to lock k2, will block forever.
  port::Thread t = BlockUntilWaitingTxn(use_range_locking, [&]() {
    // block because txn2 is holding a lock on k2.
    locker_->TryLock(txn1, 1, "k2", env_, true);
  });

  auto s = locker_->TryLock(txn2, 1, "k1", env_, true);
  ASSERT_TRUE(s.IsBusy());
  ASSERT_EQ(s.subcode(), Status::SubCode::kDeadlock);

  std::vector<DeadlockPath> deadlock_paths = locker_->GetDeadlockInfoBuffer();
  ASSERT_EQ(deadlock_paths.size(), 1u);
  ASSERT_FALSE(deadlock_paths[0].limit_exceeded);

  std::vector<DeadlockInfo> deadlocks = deadlock_paths[0].path;
  ASSERT_EQ(deadlocks.size(), 2u);

  ASSERT_EQ(deadlocks[0].m_txn_id, txn1->GetID());
  ASSERT_EQ(deadlocks[0].m_cf_id, 1u);
  ASSERT_TRUE(deadlocks[0].m_exclusive);
  ASSERT_EQ(key_value(deadlocks[0].m_waiting_key), "k2");

  ASSERT_EQ(deadlocks[1].m_txn_id, txn2->GetID());
  ASSERT_EQ(deadlocks[1].m_cf_id, 1u);
  ASSERT_TRUE(deadlocks[1].m_exclusive);
  ASSERT_EQ(key_value(deadlocks[1].m_waiting_key), "k1");

  locker_->UnLock(txn2, 1, "k2", env_);
  t.join();

  // Cleanup
  locker_->UnLock(txn1, 1, "k1", env_);
  locker_->UnLock(txn1, 1, "k2", env_);
  delete txn2;
  delete txn1;
}


// This test doesn't work with Range Lock Manager, because Range Lock Manager
// doesn't support deadlock_detect_depth.

TEST_F(TransactionLockMgrTest, DeadlockDepthExceeded) {
  // Tests that when detecting deadlock, if the detection depth is exceeded,
  // it's also viewed as deadlock.
  locker_->AddColumnFamily(&cf_1);
  TransactionOptions txn_opt;
  txn_opt.deadlock_detect = true;
  txn_opt.deadlock_detect_depth = 1;
  txn_opt.lock_timeout = 1000000;
  auto txn1 = NewTxn(txn_opt);
  auto txn2 = NewTxn(txn_opt);
  auto txn3 = NewTxn(txn_opt);
  auto txn4 = NewTxn(txn_opt);
  // "a ->(k) b" means transaction a is waiting for transaction b to release
  // the held lock on key k.
  // txn4 ->(k3) -> txn3 ->(k2) txn2 ->(k1) txn1
  // txn3's deadlock detection will exceed the detection depth 1,
  // which will be viewed as a deadlock.
  // NOTE:
  // txn4 ->(k3) -> txn3 must be set up before
  // txn3 ->(k2) -> txn2, because to trigger deadlock detection for txn3,
  // it must have another txn waiting on it, which is txn4 in this case.
  ASSERT_OK(locker_->TryLock(txn1, 1, "k1", env_, true));

  port::Thread t1 = BlockUntilWaitingTxn(false, [&]() {
    ASSERT_OK(locker_->TryLock(txn2, 1, "k2", env_, true));
    // block because txn1 is holding a lock on k1.
    locker_->TryLock(txn2, 1, "k1", env_, true);
  });

  ASSERT_OK(locker_->TryLock(txn3, 1, "k3", env_, true));

  port::Thread t2 = BlockUntilWaitingTxn(false, [&]() {
    // block because txn3 is holding a lock on k1.
    locker_->TryLock(txn4, 1, "k3", env_, true);
  });

  auto s = locker_->TryLock(txn3, 1, "k2", env_, true);
  ASSERT_TRUE(s.IsBusy());
  ASSERT_EQ(s.subcode(), Status::SubCode::kDeadlock);

  std::vector<DeadlockPath> deadlock_paths = locker_->GetDeadlockInfoBuffer();
  ASSERT_EQ(deadlock_paths.size(), 1u);
  ASSERT_TRUE(deadlock_paths[0].limit_exceeded);

  locker_->UnLock(txn1, 1, "k1", env_);
  locker_->UnLock(txn3, 1, "k3", env_);
  t1.join();
  t2.join();

  delete txn4;
  delete txn3;
  delete txn2;
  delete txn1;
}

INSTANTIATE_TEST_CASE_P(AnyLockManager, AnyLockManagerTest, ::testing::Bool());

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED because Transactions are not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
