/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "foedus/sssp/sssp_client.hpp"

#include <glog/logging.h>

#include <algorithm>
#include <cstring>

#include "foedus/assert_nd.hpp"
#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/debugging/stop_watch.hpp"
#include "foedus/memory/numa_core_memory.hpp"
#include "foedus/memory/numa_node_memory.hpp"
#include "foedus/soc/shared_memory_repo.hpp"
#include "foedus/soc/soc_manager.hpp"
#include "foedus/sssp/sssp_hashtable.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/thread/thread_pool.hpp"
#include "foedus/thread/thread_ref.hpp"
#include "foedus/xct/xct_manager.hpp"

namespace foedus {
namespace sssp {


ErrorStack SsspClientTask::run_impl_analytic() {
  ASSERT_ND(inputs_.buddy_index_ < inputs_.analytic_stripe_size_);
  outputs_->init();
  outputs_->analytic_buddy_index_ = inputs_.buddy_index_;
  WRAP_ERROR_CODE(hashtable_.create_memory(context_->get_numa_node()));
  for (uint32_t i = 0; i < kNodesPerBlock; ++i) {
    analytic_tmp_nodes_addresses_[i] = analytic_tmp_nodes_ + i;
  }

  // announce my thread_id
  ASSERT_ND(channel_->analytic_thread_ids_[inputs_.buddy_index_] == 0);
  channel_->analytic_thread_ids_[inputs_.buddy_index_] = context_->get_thread_id();
  channel_->analytic_thread_ids_setup_.fetch_add(1U);
  const uint32_t total_buddies = inputs_.analytic_stripe_size_;
  while (true) {
    uint32_t count = channel_->analytic_thread_ids_setup_.load(std::memory_order_acquire);
    ASSERT_ND(count <= total_buddies);
    if (count == total_buddies) {
      break;
    }
  }

  // Now everyone set the analytic_thread_ids_. Let's get buddies' Outputs address.
  thread::ThreadPool* thread_pool = engine_->get_thread_pool();
  for (uint32_t buddy = 0; buddy < total_buddies; ++buddy) {
    thread::ThreadId thread_id = channel_->analytic_thread_ids_[buddy];
    thread::ThreadRef* buddy_ref = thread_pool->get_thread_ref(thread_id);
    void* buddy_output = buddy_ref->get_task_output_memory();
    ASSERT_ND(buddy_output);
    analytic_other_outputs_[buddy] = reinterpret_cast<Outputs*>(buddy_output);
    ASSERT_ND(analytic_other_outputs_[buddy]->analytic_buddy_index_ == buddy);
  }

  channel_->start_rendezvous_.wait();
  LOG(INFO) << "SSSP Client-" << get_worker_id() << " started processing analytic queries "
    << " buddy_index=" << inputs_.buddy_index_ << "/" << inputs_.analytic_stripe_size_;

  CHECK_ERROR(do_analytic());

  hashtable_.release_memory();
  return kRetOk;
}

void SsspClientTask::analytic_prepare_query() {
  // Clear L1/L2 version counters
  outputs_->init_analytic(inputs_.analytic_stripe_count_);

  // This worker is done with preparation. Let others know and wait.
  uint32_t before = channel_->analytic_prepared_clients_.fetch_add(1U);
  if (before + 1U == inputs_.analytic_stripe_size_) {
    // ok, I'm the last one.
    channel_->analytic_state_.store(SsspClientChannel::kAnalyticStarted);
  } else {
    while (channel_->analytic_state_.load(std::memory_order_acquire)
              == SsspClientChannel::kAnalyticPreparing) {
      continue;
    }
  }
}

ErrorCode SsspClientTask::analytic_wait_for_task() {
  const uint32_t stripes_per_l1 = inputs_.analytic_stripe_size_;
  while (true) {
    // Has the query ended or stop requested? Check it for each iteration
    if (channel_->analytic_state_.load(std::memory_order_acquire)
        != SsspClientChannel::kAnalyticStarted) {
      return kErrorCodeOk;
    }

    // Check all L1 counters
    for (uint32_t i1 = 0; i1 < kL1VersionFactors; ++i1) {
      if (UNLIKELY(outputs_->analytic_l1_versions_[i1].check_update())) {
        // whoa, there might be some update!
        for (uint32_t i2 = 0; i2 < stripes_per_l1; ++i2) {
          uint32_t stripe_index = i1 * stripes_per_l1 + i2;
          if (UNLIKELY(outputs_->analytic_l2_versions_[stripe_index].check_update())) {
            // Yes, this block might contain update.
            CHECK_ERROR_CODE(analytic_relax_block(stripe_index));
          }
        }
      }
    }
  }

  return kErrorCodeOk;
}

ErrorCode SsspClientTask::analytic_relax_block_retrieve_topology() {
  CHECK_ERROR_CODE(xct_manager_->begin_xct(context_, xct::kSerializable));
  CHECK_ERROR_CODE(storages_.vertex_data_.get_record_payload_batch(
    context_,
    kNodesPerBlock,
    analytic_tmp_node_ids_,
    analytic_tmp_nodes_addresses_));

  // In this experiment, no race abort is expected because there is no concurrent write
  // on vertex_data_. In general, we should check the result and retry if aborted.
  Epoch commit_epoch;
  CHECK_ERROR_CODE(xct_manager_->precommit_xct(context_, &commit_epoch));
  return kErrorCodeOk;
}

ErrorCode SsspClientTask::analytic_relax_block(uint32_t stripe) {
  ASSERT_ND(stripe < inputs_.analytic_stripe_count_);
  const uint64_t block = stripe * inputs_.analytic_stripe_size_ + inputs_.buddy_index_;
  ASSERT_ND(block < kBlocksPerPartition * inputs_.max_px_ * inputs_.max_py_);
  const NodeId node_id_offset = block * kNodesPerBlock;

  for (uint32_t n = 0; n < kNodesPerBlock; ++n) {
    analytic_tmp_node_ids_[n] = n + node_id_offset;
  }

  // First, retrieve all nodes' topology in this block in one shot from vertex_data.
  CHECK_ERROR_CODE(analytic_relax_block_retrieve_topology());

  // Second, check the current state of them from vertex_bf.
  // These data are at least as of or after the timing this worker picked up the task.
  // If another worker updates some of them now, he will surely notify us in the ver counter.
  // With this protocol, false positive (we check it again) is possible, but no false negative.
  Epoch commit_epoch;
  while (true) {  // in case we get race-retry
    CHECK_ERROR_CODE(xct_manager_->begin_xct(context_, xct::kSerializable));
    ASSERT_ND(sizeof(VertexBfData) == sizeof(uint64_t));
    CHECK_ERROR_CODE(storages_.vertex_bf_.get_record_primitive_batch<uint64_t>(
      context_,
      0,
      kNodesPerBlock,
      analytic_tmp_node_ids_,
      reinterpret_cast<uint64_t*>(analytic_tmp_bf_records_)));
    ErrorCode ret = xct_manager_->precommit_xct(context_, &commit_epoch);
    if (ret == kErrorCodeOk) {
      break;
    } else {
      // If someone else has just changed it, retry. this should be rare.
      DVLOG(0) << "Abort-retry in second step";
      ASSERT_ND(ret == kErrorCodeXctRaceAbort);
      CHECK_ERROR_CODE(xct_manager_->abort_xct(context_));
    }
  }

  // Third, calculate shortest path based on the info so far.
  // To remember info for other blocks, we reuse the hashtable in nav queries.
  hashtable_.clean();
  for (uint32_t n = 0; n < kNodesPerBlock; ++n) {
    if (analytic_tmp_bf_records_[n].distance_ != 0) {
      analytic_relax_node_recurse(n, node_id_offset);
    }
  }

  // Finally, we apply the updated info.
  // Let's do our own block first. No need to notify ourselves.
  CHECK_ERROR_CODE(analytic_apply_own_block());
  // Then foreign block
  CHECK_ERROR_CODE(analytic_apply_foreign_blocks());
  return kErrorCodeOk;
}

ErrorCode SsspClientTask::analytic_apply_own_block() {
  Epoch commit_epoch;
  while (true) {  // in case we get race-retry
    CHECK_ERROR_CODE(xct_manager_->begin_xct(context_, xct::kSerializable));
    for (uint32_t n = 0; n < kNodesPerBlock; ++n) {
      const VertexBfData* new_data = analytic_tmp_bf_records_ + n;
      if (new_data->distance_ == 0) {
        continue;
      }
      uint32_t node_id = analytic_tmp_node_ids_[n];
      uint32_t cur_distance;
      CHECK_ERROR_CODE(storages_.vertex_bf_.get_record_primitive<uint32_t>(
        context_,
        node_id,
        &cur_distance,
        offsetof(VertexBfData, distance_)));
      if (cur_distance > new_data->distance_) {
        CHECK_ERROR_CODE(storages_.vertex_bf_.overwrite_record(
          context_,
          node_id,
          new_data));
      }
    }

    ErrorCode ret = xct_manager_->precommit_xct(context_, &commit_epoch);
    if (ret == kErrorCodeOk) {
      break;
    } else {
      // If someone else has just changed it, retry. this should be rare.
      DVLOG(0) << "Abort-retry in own-apply step";
      ASSERT_ND(ret == kErrorCodeXctRaceAbort);
      CHECK_ERROR_CODE(xct_manager_->abort_xct(context_));
    }
  }
  return kErrorCodeOk;
}

ErrorCode SsspClientTask::analytic_apply_foreign_blocks() {
  const uint32_t key_count = hashtable_.get_inserted_key_count();
  if (key_count == 0) {
    return kErrorCodeOk;
  }
  NodeId* node_ids = hashtable_.get_inserted_keys();
  // Batch-apply and batch-notify the propagations. Remember the following order:
  //  1. Apply new distance to vertex_bf_
  //  2. Increment version counter in L2
  //  3. Increment version counter in L1
  // These protocols guarantee that there is no false negative.

  // Just for efficient batching below, order by IDs.
  std::sort(node_ids, node_ids + key_count);

  // Then, process by block.
  uint32_t index = 0;
  while (index < key_count) {
    const uint32_t block = node_ids[index] / kNodesPerBlock;
    // how many nodes for this block?
    uint32_t count = 0;
    while (index + count <= key_count) {
      ASSERT_ND(node_ids[index + count] > node_ids[index]);
      if (node_ids[index + count] / kNodesPerBlock != block)  {
        break;
      }
      ++count;
    }
    ASSERT_ND(count <= kNodesPerBlock);
    ASSERT_ND(index == 0 || (node_ids[index - 1] / kNodesPerBlock) != block);

    while (true) {  // in case we get race-retry
      CHECK_ERROR_CODE(xct_manager_->begin_xct(context_, xct::kSerializable));
      for (uint32_t n = 0; n < count; ++n) {
        NodeId key = node_ids[index + n];
        const DijkstraHashtable::Record* record = hashtable_.get(key);
        ASSERT_ND(record->value_.distance_ > 0);
        uint32_t cur_distance;
        CHECK_ERROR_CODE(storages_.vertex_bf_.get_record_primitive<uint32_t>(
          context_,
          key,
          &cur_distance,
          offsetof(VertexBfData, distance_)));
        if (cur_distance > record->value_.distance_) {
          VertexBfData new_data;
          new_data.distance_ = record->value_.distance_;
          new_data.pred_node_ = record->value_.previous_;
          CHECK_ERROR_CODE(storages_.vertex_bf_.overwrite_record(
            context_,
            key,
            &new_data));
        }
      }

      Epoch commit_epoch;
      ErrorCode ret = xct_manager_->precommit_xct(context_, &commit_epoch);
      if (ret == kErrorCodeOk) {
        break;
      } else {
        // This might happen often.
        DVLOG(0) << "Abort-retry in foreign-apply step";
        ASSERT_ND(ret == kErrorCodeXctRaceAbort);
        CHECK_ERROR_CODE(xct_manager_->abort_xct(context_));
      }
    }

    // Notify the block. who owns it?
    const uint32_t target_stripe = block / inputs_.analytic_stripe_size_;
    ASSERT_ND(target_stripe < inputs_.analytic_stripe_count_);
    const uint32_t target_owner_buddy_index = block % inputs_.analytic_stripe_size_;
    Outputs* foreign_output = analytic_other_outputs_[target_owner_buddy_index];
    // Let him know that we changed something.
    foreign_output->increment_l2_then_l1(target_stripe, inputs_.analytic_stripe_size_);

    // all done. go on to next block
    index += count;
  }

  ASSERT_ND(index == key_count);
  return kErrorCodeOk;
}

void SsspClientTask::analytic_relax_node_recurse(uint32_t n, NodeId node_id_offset) {
  // This recursion is upto kNodesPerBlock depth, and not much stack variables,
  // so it should'd cause stackoverflow.
  ASSERT_ND(n < kNodesPerBlock);
  const VertexBfData* my_data = analytic_tmp_bf_records_ + n;
  ASSERT_ND(my_data->distance_ != 0);
  const Node* my_node = analytic_tmp_nodes_ + n;
  const NodeId my_id = n + node_id_offset;
  ASSERT_ND(my_node->id_ == my_id);
  for (uint32_t e = 0; e < my_node->edge_count_; ++e) {
    const Edge* edge = my_node->edges_ + e;
    const uint32_t new_distance = edge->mileage_ + my_data->distance_;
    if (edge->to_ >= node_id_offset && edge->to_ < node_id_offset + kNodesPerBlock) {
      const uint32_t another_n = edge->to_ - node_id_offset;
      VertexBfData* another_data = analytic_tmp_bf_records_ + another_n;
      if (another_data->distance_ == 0 || another_data->distance_ > new_distance) {
        another_data->distance_ = new_distance;
        another_data->pred_node_ = my_id;
        analytic_relax_node_recurse(another_n, node_id_offset);
      }
    } else {
      // Pointing to foreign block. Check with hashtable
      DijkstraHashtable::Record* record = hashtable_.get_or_create(edge->to_);
      if (record->value_.distance_ == 0 || record->value_.distance_ > new_distance) {
        record->value_.distance_ = new_distance;
        record->value_.previous_ = my_id;
      }
    }
  }
}

ErrorStack SsspClientTask::do_analytic() {
  outputs_->init_analytic(inputs_.analytic_stripe_count_);
  uint32_t before = channel_->analytic_prepared_clients_.fetch_add(1U);
  if (before + 1U == inputs_.analytic_stripe_size_) {
    // ok, I'm the last one.
    channel_->analytic_state_.store(SsspClientChannel::kAnalyticStarted);
  } else {
    while (channel_->analytic_state_.load(std::memory_order_acquire)
              == SsspClientChannel::kAnalyticPreparing) {
      continue;
    }
  }
  return kRetOk;
}

void SsspClientTask::Outputs::init_analytic(uint32_t stripe_count) {
  std::memset(analytic_l1_versions_, 0, sizeof(analytic_l1_versions_));
  std::memset(analytic_l2_versions_, 0, sizeof(VersionCounter) * stripe_count);
}

void SsspClientTask::Outputs::increment_l2_then_l1(uint32_t stripe, uint32_t stripes_per_l1) {
  analytic_l2_versions_[stripe].on_update();
  uint32_t l1_index = stripe / stripes_per_l1;
  ASSERT_ND(l1_index < kL1VersionFactors);
  analytic_l1_versions_[l1_index].on_update();
}

}  // namespace sssp
}  // namespace foedus