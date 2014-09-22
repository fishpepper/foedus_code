/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include "foedus/soc/shared_memory_repo.hpp"

#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "foedus/assert_nd.hpp"
#include "foedus/assorted/assorted_func.hpp"

namespace foedus {
namespace soc {

std::string get_self_path() {
  pid_t pid = ::getpid();
  std::string pid_str = std::to_string(pid);
  return std::string("/tmp/libfoedus_shm_") + pid_str;
}
std::string get_master_path(uint64_t master_upid) {
  std::string pid_str = std::to_string(master_upid);
  return std::string("/tmp/libfoedus_shm_") + pid_str;
}

void NodeMemoryAnchors::allocate_arrays(const EngineOptions& options) {
  deallocate_arrays();
  logger_memories_ = new log::LoggerControlBlock*[options.log_.loggers_per_node_];
  thread_anchors_ = new ThreadMemoryAnchors[options.thread_.thread_count_per_group_];
}

void NodeMemoryAnchors::deallocate_arrays() {
  if (logger_memories_) {
    delete[] logger_memories_;
    logger_memories_ = nullptr;
  }
  if (thread_anchors_) {
    delete[] thread_anchors_;
    thread_anchors_ = nullptr;
  }
}

uint64_t align_4kb(uint64_t value) { return assorted::align< uint64_t, (1U << 12) >(value); }
uint64_t align_2mb(uint64_t value) { return assorted::align< uint64_t, (1U << 21) >(value); }

void SharedMemoryRepo::allocate_one_node(
  uint16_t node,
  uint64_t node_memory_size,
  uint64_t volatile_pool_size,
  SharedMemoryRepo* repo) {
  std::string node_memory_path = get_self_path() + std::string("_node_") + std::to_string(node);
  repo->node_memories_[node].alloc(node_memory_path, node_memory_size, node);
  std::string volatile_pool_path = get_self_path() + std::string("_vpool_") + std::to_string(node);
  repo->volatile_pools_[node].alloc(volatile_pool_path, volatile_pool_size, node);
}

ErrorStack SharedMemoryRepo::allocate_shared_memories(const EngineOptions& options) {
  deallocate_shared_memories();
  init_empty(options);

  // We place a serialized EngineOptions in the beginning of shared memory.
  std::stringstream options_stream;
  options.save_to_stream(&options_stream);
  std::string xml(options_stream.str());
  uint64_t xml_size = xml.size();

  // construct unique meta files using PID.
  uint64_t global_memory_size = align_2mb(calculate_global_memory_size(xml_size, options));
  std::string global_memory_path = get_self_path() + std::string("_global");
  global_memory_.alloc(global_memory_path, global_memory_size, 0);
  if (global_memory_.is_null()) {
    deallocate_shared_memories();
    std::cerr << "[FOEDUS] Failed to allocate global shared memory. os_error="
      << assorted::os_error() << std::endl;
    return ERROR_STACK(kErrorCodeSocShmAllocFailed);
  }

  set_global_memory_anchors(xml_size, options);
  global_memory_anchors_.master_status_memory_->status_code_ = MasterEngineStatus::kInitial;

  // copy the EngineOptions string into the beginning of the global memory
  std::memcpy(global_memory_.get_block(), &xml_size, sizeof(xml_size));
  std::memcpy(global_memory_.get_block() + sizeof(xml_size), xml.data(), xml_size);

  // the following is parallelized
  uint64_t node_memory_size = align_2mb(calculate_node_memory_size(options));
  uint64_t volatile_pool_size
    = static_cast<uint64_t>(options.memory_.page_pool_size_mb_per_node_) << 20;
  std::vector< std::thread > alloc_threads;
  for (uint16_t node = 0; node < soc_count_; ++node) {
    alloc_threads.emplace_back(std::thread(
      SharedMemoryRepo::allocate_one_node,
      node,
      node_memory_size,
      volatile_pool_size,
      this));
  }

  bool failed = false;
  for (uint16_t node = 0; node < soc_count_; ++node) {
    alloc_threads[node].join();
    if (node_memories_[node].is_null() || volatile_pools_[node].is_null()) {
      std::cerr << "[FOEDUS] Failed to allocate node shared memory. os_error="
        << assorted::os_error() << std::endl;
      failed = true;
    }
  }

  if (failed) {
    deallocate_shared_memories();
    return ERROR_STACK(kErrorCodeSocShmAllocFailed);
  }

  for (uint16_t node = 0; node < soc_count_; ++node) {
    set_node_memory_anchors(node, options);
  }

  return kRetOk;
}

ErrorStack SharedMemoryRepo::attach_shared_memories(
  uint64_t master_upid,
  SocId my_soc_id,
  EngineOptions* options) {
  deallocate_shared_memories();

  std::string base = get_master_path(master_upid);
  std::string global_memory_path = base + std::string("_global");
  global_memory_.attach(global_memory_path);
  if (global_memory_.is_null()) {
    deallocate_shared_memories();
    return ERROR_STACK(kErrorCodeSocShmAttachFailed);
  }

  // read the options from global_memory
  uint64_t xml_size = 0;
  std::memcpy(&xml_size, global_memory_.get_block(), sizeof(xml_size));
  ASSERT_ND(xml_size > 0);
  std::string xml(global_memory_.get_block() + sizeof(xml_size), xml_size);
  CHECK_ERROR(options->load_from_string(xml));

  my_soc_id_ = my_soc_id;
  init_empty(*options);
  set_global_memory_anchors(xml_size, *options);

  bool failed = false;
  for (uint16_t node = 0; node < soc_count_; ++node) {
    node_memories_[node].attach(base + std::string("_node_") + std::to_string(node));
    volatile_pools_[node].attach(base + std::string("_vpool_") + std::to_string(node));
    if (node_memories_[node].is_null() || volatile_pools_[node].is_null()) {
      failed = true;
    } else {
      set_node_memory_anchors(node, *options);
    }
  }

  if (failed) {
    if (!node_memories_[my_soc_id].is_null()) {
      // then we can at least notify the error via the shared memory
      change_child_status(my_soc_id, ChildEngineStatus::kFatalError);
    }
    deallocate_shared_memories();
    return ERROR_STACK(kErrorCodeSocShmAttachFailed);
  }
  return kRetOk;
}

void SharedMemoryRepo::mark_for_release() {
  // mark_for_release() is idempotent, so just do it on all of them
  global_memory_.mark_for_release();
  for (uint16_t i = 0; i < soc_count_; ++i) {
    if (node_memories_) {
      node_memories_[i].mark_for_release();
    }
    if (volatile_pools_) {
      volatile_pools_[i].mark_for_release();
    }
  }
}
void SharedMemoryRepo::deallocate_shared_memories() {
  mark_for_release();
  // release_block() is idempotent, so just do it on all of them
  global_memory_.release_block();
  global_memory_anchors_.clear();
  for (uint16_t i = 0; i < soc_count_; ++i) {
    if (node_memories_) {
      node_memories_[i].release_block();
    }
    if (volatile_pools_) {
      volatile_pools_[i].release_block();
    }
  }

  if (node_memories_) {
    delete[] node_memories_;
    node_memories_ = nullptr;
  }
  if (node_memory_anchors_) {
    delete[] node_memory_anchors_;
    node_memory_anchors_ = nullptr;
  }
  if (volatile_pools_) {
    delete[] volatile_pools_;
    volatile_pools_ = nullptr;
  }
  soc_count_ = 0;
}

void SharedMemoryRepo::init_empty(const EngineOptions& options) {
  soc_count_ = options.thread_.group_count_;
  node_memories_ = new memory::SharedMemory[soc_count_];
  node_memory_anchors_ = new NodeMemoryAnchors[soc_count_];
  volatile_pools_ = new memory::SharedMemory[soc_count_];
  for (uint16_t node = 0; node < soc_count_; ++node) {
    node_memory_anchors_[node].allocate_arrays(options);
  }
}

void SharedMemoryRepo::set_global_memory_anchors(uint64_t xml_size, const EngineOptions& options) {
  char* base = global_memory_.get_block();
  uint64_t total = 0;
  global_memory_anchors_.options_xml_length_ = xml_size;
  global_memory_anchors_.options_xml_ = base + sizeof(uint64_t);
  total += align_4kb(sizeof(uint64_t) + xml_size);

  global_memory_anchors_.master_status_memory_
    = reinterpret_cast<MasterEngineStatus*>(base + total);
  total += GlobalMemoryAnchors::kMasterStatusMemorySize;

  global_memory_anchors_.log_manager_memory_
    = reinterpret_cast<log::LogManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kLogManagerMemorySize;

  global_memory_anchors_.restart_manager_memory_
    = reinterpret_cast<restart::RestartManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kRestartManagerMemorySize;

  global_memory_anchors_.savepoint_manager_memory_
    = reinterpret_cast<savepoint::SavepointManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kSavepointManagerMemorySize;

  global_memory_anchors_.snapshot_manager_memory_
    = reinterpret_cast<snapshot::SnapshotManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kSnapshotManagerMemorySize;

  global_memory_anchors_.storage_manager_memory_
    = reinterpret_cast<storage::StorageManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kStorageManagerMemorySize;

  global_memory_anchors_.xct_manager_memory_
    = reinterpret_cast<xct::XctManagerControlBlock*>(base + total);
  total += GlobalMemoryAnchors::kXctManagerMemorySize;

  global_memory_anchors_.storage_name_sort_memory_
    = reinterpret_cast<storage::StorageId*>(base + total);
  total += align_4kb(sizeof(storage::StorageId) * options.storage_.max_storages_);

  global_memory_anchors_.storage_memories_
    = reinterpret_cast<storage::StorageControlBlock*>(base + total);
  total += static_cast<uint64_t>(GlobalMemoryAnchors::kStorageMemorySize)
    * options.storage_.max_storages_;

  global_memory_anchors_.user_memory_ = base + total;
  total += align_4kb(1024ULL * options.soc_.shared_user_memory_size_kb_);

  // we have to be super careful here. let's not use assertion.
  if (calculate_global_memory_size(xml_size, options) != total) {
    std::cerr << "[FOEDUS] global memory size doesn't match. bug?"
      << " allocated=" << calculate_global_memory_size(xml_size, options)
      << ", expected=" << total << std::endl;
  }
}

uint64_t SharedMemoryRepo::calculate_global_memory_size(
  uint64_t xml_size,
  const EngineOptions& options) {
  uint64_t total = 0;
  total += align_4kb(sizeof(xml_size) + xml_size);  // options_xml_
  total += GlobalMemoryAnchors::kMasterStatusMemorySize;
  total += GlobalMemoryAnchors::kLogManagerMemorySize;
  total += GlobalMemoryAnchors::kRestartManagerMemorySize;
  total += GlobalMemoryAnchors::kSavepointManagerMemorySize;
  total += GlobalMemoryAnchors::kSnapshotManagerMemorySize;
  total += GlobalMemoryAnchors::kStorageManagerMemorySize;
  total += GlobalMemoryAnchors::kXctManagerMemorySize;
  total += align_4kb(sizeof(storage::StorageId) * options.storage_.max_storages_);
  total += static_cast<uint64_t>(GlobalMemoryAnchors::kStorageMemorySize)
    * options.storage_.max_storages_;
  total += align_4kb(1024ULL * options.soc_.shared_user_memory_size_kb_);
  return total;
}

void SharedMemoryRepo::set_node_memory_anchors(SocId node, const EngineOptions& options) {
  char* base = node_memories_[node].get_block();
  NodeMemoryAnchors& anchor = node_memory_anchors_[node];
  uint64_t total = 0;
  anchor.child_status_memory_ = reinterpret_cast<ChildEngineStatus*>(base);
  total += NodeMemoryAnchors::kChildStatusMemorySize;
  anchor.volatile_pool_status_ = reinterpret_cast<memory::PagePoolControlBlock*>(base + total);
  total += NodeMemoryAnchors::kPagePoolMemorySize;

  anchor.proc_manager_memory_ = reinterpret_cast<proc::ProcManagerControlBlock*>(base + total);
  total += NodeMemoryAnchors::kProcManagerMemorySize;
  anchor.proc_memory_ = reinterpret_cast<proc::ProcAndName*>(base + total);
  total += align_4kb(sizeof(proc::ProcAndName) * options.proc_.max_proc_count_);
  anchor.proc_name_sort_memory_ = reinterpret_cast<proc::LocalProcId*>(base + total);
  total += align_4kb(sizeof(proc::LocalProcId) * options.proc_.max_proc_count_);

  for (uint16_t i = 0; i < options.log_.loggers_per_node_; ++i) {
    anchor.logger_memories_[i] = reinterpret_cast<log::LoggerControlBlock*>(base + total);
    total += NodeMemoryAnchors::kLoggerMemorySize;
  }

  for (uint16_t i = 0; i < options.thread_.thread_count_per_group_; ++i) {
    ThreadMemoryAnchors& thread_anchor = anchor.thread_anchors_[i];
    thread_anchor.thread_memory_ = reinterpret_cast<thread::ThreadControlBlock*>(base + total);
    total += ThreadMemoryAnchors::kThreadMemorySize;
    thread_anchor.task_input_memory_ = base + total;
    total += ThreadMemoryAnchors::kTaskInputMemorySize;
    thread_anchor.task_output_memory_ = base + total;
    total += ThreadMemoryAnchors::kTaskOutputMemorySize;
    thread_anchor.mcs_lock_memories_ = reinterpret_cast<xct::McsBlock*>(base + total);
    total += ThreadMemoryAnchors::kMcsLockMemorySize;
  }

  // we have to be super careful here. let's not use assertion.
  if (total != calculate_node_memory_size(options)) {
    std::cerr << "[FOEDUS] node memory size doesn't match. bug?"
      << " allocated=" << calculate_node_memory_size(options)
      << ", expected=" << total << std::endl;
  }
}

uint64_t SharedMemoryRepo::calculate_node_memory_size(const EngineOptions& options) {
  uint64_t total = 0;
  total += NodeMemoryAnchors::kChildStatusMemorySize;
  total += NodeMemoryAnchors::kPagePoolMemorySize;
  total += NodeMemoryAnchors::kProcManagerMemorySize;
  total += align_4kb(sizeof(proc::ProcAndName) * options.proc_.max_proc_count_);
  total += align_4kb(sizeof(proc::LocalProcId) * options.proc_.max_proc_count_);

  uint64_t loggers_per_node = options.log_.loggers_per_node_;
  total += loggers_per_node * NodeMemoryAnchors::kLoggerMemorySize;

  uint64_t threads_per_node = options.thread_.thread_count_per_group_;
  total += threads_per_node * ThreadMemoryAnchors::kThreadMemorySize;
  total += threads_per_node * ThreadMemoryAnchors::kTaskInputMemorySize;
  total += threads_per_node * ThreadMemoryAnchors::kTaskOutputMemorySize;
  total += threads_per_node * ThreadMemoryAnchors::kMcsLockMemorySize;
  return total;
}

void SharedMemoryRepo::change_master_status(MasterEngineStatus::StatusCode new_status) {
  global_memory_anchors_.master_status_memory_->change_status_atomic(new_status);
}

MasterEngineStatus::StatusCode SharedMemoryRepo::get_master_status() const {
  return global_memory_anchors_.master_status_memory_->read_status_atomic();
}

void SharedMemoryRepo::change_child_status(SocId node, ChildEngineStatus::StatusCode new_status) {
  node_memory_anchors_[node].child_status_memory_->change_status_atomic(new_status);
}

ChildEngineStatus::StatusCode SharedMemoryRepo::get_child_status(SocId node) const {
  return node_memory_anchors_[node].child_status_memory_->read_status_atomic();
}


}  // namespace soc
}  // namespace foedus