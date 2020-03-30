/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. 
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

// PORT #include <toku_assert.h>

void concurrent_tree::create(const comparator *cmp) {
    // start with an empty root node. we do this instead of
    // setting m_root to null so there's always a root to lock
    m_root.create_root(cmp);
}

void concurrent_tree::destroy(void) {
    m_root.destroy_root();
}

bool concurrent_tree::is_empty(void) {
    return m_root.is_empty();
}

uint64_t concurrent_tree::get_insertion_memory_overhead(void) {
    return sizeof(treenode);
}

void concurrent_tree::locked_keyrange::prepare_(concurrent_tree *tree) {
    // the first step in acquiring a locked keyrange is locking the root
    treenode *const root = &tree->m_root;
    m_tree = tree;
    m_subtree = root;
    m_range = keyrange::get_infinite_range();
    root->mutex_lock();
    exclusive_prepare= true;
    m_subtree_locked = true;
    // Do not synchronize the RCU, yet. We will do it when we really need it.

    //TODO: we could also add fallback-to-non-locking-case here.
}

// void concurrent_tree::locked_keyrange::disable_rcu_if_needed() {
//     assert (m_subtree_locked);
//     assert (exclusive_prepare);
//     if (m_subtree == &m_tree->m_root) {
//         rcu_disabler dr(m_tree);
//         dr.disable_rcu();
//     }
// }


void concurrent_tree::locked_keyrange::prepare_no_lock(concurrent_tree *tree) {
    // the first step in acquiring a locked keyrange is locking the root
    treenode *const root = &tree->m_root;
    m_tree = tree;
    m_subtree = root;
    m_range = keyrange::get_infinite_range();
    exclusive_prepare= false;
    m_subtree_locked = false;
}


/*
   RCU locking policy:

   The following execution orders are allowed:
   
   Concurrent traverser:
     Get RCU read lock
         get a child mutex
     Release RCU read lock;
  
   Non-concurrent modifier:
     Get the root mutex;
     Get an RCU write lock (wait until readers are gone)
     get child mutex
     ...
     eventually enable back the RCU

   (Note1: one may not do "wait until readers are gone" step while holding a
   child mutex. This creates a deadlock as one of the readers might be trying to
   acquire the child mutex you're holding.)
  
*/
void concurrent_tree::locked_keyrange::acquire(const keyrange &range) {
    treenode *const root = &m_tree->m_root;
    assert(exclusive_prepare);
    assert(m_subtree_locked);
    
    // We will disable the RCU before doing any data-modifying operation.
    rcu_disabler disabler(m_tree);
    rcu_disabler *disabler_ptr= (m_tree->rcu_caching_enabled)? &disabler : nullptr;

    //psergey-todo:   root rebalance here.
    //psergey-sunday: root->maybe_rotate(m_tree, disabler_ptr);

    treenode *subtree;
    if (root->is_empty() || root->range_overlaps(range)) {
        subtree = root;
    } else {
        // we do not have a precomputed comparison hint, so pass null
        const keyrange::comparison *cmp_hint = nullptr;
        subtree = root->find_node_with_overlapping_child(range, cmp_hint, disabler_ptr);
    }

    // psergey-mar14:
    if (subtree == root && disabler_ptr) {
        if (disabler_ptr->disable_rcu())
            PERF_COUNTER_ADD(rangelock_disable_rcu_hit_root, 1);
    }

    // subtree is locked. it will be unlocked when this is release()'d
    invariant_notnull(subtree);
    m_range = range;
    m_subtree = subtree;
}

void concurrent_tree::locked_keyrange::add_shared_owner(const keyrange &range,
                                                        TXNID new_owner)
{
    m_subtree->insert(range, new_owner, /*is_shared*/ true);
}

void concurrent_tree::locked_keyrange::release(void) {
    /*
      If we are releasing the root node, enable the RCU.
    */
    if (m_tree->rcu_caching_enabled && m_subtree == &m_tree->m_root &&
        exclusive_prepare) {
        assert(m_subtree_locked);
        rcu_assign_pointer(m_tree->rcu_cache_usable, (void*)1);
        rangelock_rcu_enabled_counter_add(); // increment rangelock_rcu_enabled
    }
    if (m_subtree_locked)
        m_subtree->mutex_unlock();
}

template <class F>
void concurrent_tree::locked_keyrange::iterate(F *function) const {
    // if the subtree is non-empty, traverse it by calling the given
    // function on each range, txnid pair found that overlaps.
    if (!m_subtree->is_empty()) {
        m_subtree->traverse_overlaps(m_range, function);
    }
}

void concurrent_tree::locked_keyrange::insert(const keyrange &range,
                                              TXNID txnid, bool is_shared) {
    // empty means no children, and only the root should ever be empty
    if (m_subtree->is_empty()) {
        m_subtree->set_range_and_txnid(range, txnid, is_shared);
    } else {
        m_subtree->insert(range, txnid, is_shared);
    }
}

void concurrent_tree::locked_keyrange::remove(const keyrange &range, TXNID txnid) {
    invariant(!m_subtree->is_empty());
    treenode *new_subtree = m_subtree->remove(range, txnid);
    // if removing range changed the root of the subtree,
    // then the subtree must be the root of the entire tree.
    if (new_subtree == nullptr) {
        invariant(m_subtree->is_root());
        invariant(m_subtree->is_empty());
    }
}

void concurrent_tree::locked_keyrange::remove_all(void) {
    m_subtree->recursive_remove();
}
