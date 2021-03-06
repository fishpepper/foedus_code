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
#ifndef FOEDUS_THREAD_THREAD_HPP_
#define FOEDUS_THREAD_THREAD_HPP_
#include <iosfwd>

#include "foedus/fwd.hpp"
#include "foedus/initializable.hpp"
#include "foedus/assorted/uniform_random.hpp"
#include "foedus/log/fwd.hpp"
#include "foedus/memory/fwd.hpp"
#include "foedus/memory/page_resolver.hpp"
#include "foedus/storage/fwd.hpp"
#include "foedus/storage/storage_id.hpp"
#include "foedus/thread/fwd.hpp"
#include "foedus/thread/thread_id.hpp"
#include "foedus/xct/fwd.hpp"
#include "foedus/xct/xct_id.hpp"

namespace foedus {
namespace thread {
/**
 * @brief Represents one thread running on one NUMA core.
 * @ingroup THREAD
 * @details
 * @section THREAD_MCS MCS-Locking
 * SILO uses a simple spin lock with atomic CAS, but we observed a HUUUGE bottleneck
 * with it on big machines (8 sockets or 16 sockets) while it was totally fine up to 4 sockets.
 * It causes a cache invalidation storm even with exponential backoff.
 * The best solution is MCS locking with \e local spins. We implemented it with advices from
 * HLINUX team.
 */
class Thread CXX11_FINAL : public virtual Initializable {
 public:
  enum Constants {
    /**
     * Max size for find_or_read_snapshot_pages_batch() etc.
     * This must be same or less than CacheHashtable::kMaxFindBatchSize.
     */
    kMaxFindPagesBatch = 32,
  };
  Thread() CXX11_FUNC_DELETE;
  Thread(Engine* engine, ThreadId id, ThreadGlobalOrdinal global_ordinal);
  ~Thread();
  ErrorStack  initialize() CXX11_OVERRIDE;
  bool        is_initialized() const CXX11_OVERRIDE;
  ErrorStack  uninitialize() CXX11_OVERRIDE;

  Engine*     get_engine() const;
  ThreadId    get_thread_id() const;
  ThreadGroupId get_numa_node() const { return decompose_numa_node(get_thread_id()); }
  ThreadGlobalOrdinal get_thread_global_ordinal() const;

  /**
   * Returns the transaction that is currently running on this thread.
   */
  xct::Xct&   get_current_xct();
  /** Returns if this thread is running an active transaction. */
  bool        is_running_xct() const;

  /** Returns the private memory repository of this thread. */
  memory::NumaCoreMemory* get_thread_memory() const;
  /** Returns the node-shared memory repository of the NUMA node this thread belongs to. */
  memory::NumaNodeMemory* get_node_memory() const;

  /**
   * @brief Returns the private log buffer for this thread.
   */
  log::ThreadLogBuffer&   get_thread_log_buffer();

  /**
   * Returns the page resolver to convert page ID to page pointer.
   * Just a shorthand for get_engine()->get_memory_manager()->get_global_volatile_page_resolver().
   */
  const memory::GlobalVolatilePageResolver& get_global_volatile_page_resolver() const;
  /** Returns page resolver to convert only local page ID to page pointer. */
  const memory::LocalPageResolver& get_local_volatile_page_resolver() const;

  /** [statistics] count of cache hits in snapshot caches */
  uint64_t      get_snapshot_cache_hits() const;
  /** [statistics] count of cache misses in snapshot caches */
  uint64_t      get_snapshot_cache_misses() const;
  /** [statistics] resets the above two */
  void          reset_snapshot_cache_counts() const;

  /** Shorthand for get_global_volatile_page_resolver.resolve_offset() */
  storage::Page* resolve(storage::VolatilePagePointer ptr) const;
  /** Shorthand for get_global_volatile_page_resolver.resolve_offset_newpage() */
  storage::Page* resolve_newpage(storage::VolatilePagePointer ptr) const;
  /** Shorthand for get_local_volatile_page_resolver.resolve_offset() */
  storage::Page* resolve(memory::PagePoolOffset offset) const;
  /** Shorthand for get_local_volatile_page_resolver.resolve_offset_newpage() */
  storage::Page* resolve_newpage(memory::PagePoolOffset offset) const;
  /** resolve() plus reinterpret_cast */
  template <typename P> P* resolve_cast(storage::VolatilePagePointer ptr) const {
    return reinterpret_cast<P*>(resolve(ptr));
  }
  template <typename P> P* resolve_newpage_cast(storage::VolatilePagePointer ptr) const {
    return reinterpret_cast<P*>(resolve_newpage(ptr));
  }
  template <typename P> P* resolve_cast(memory::PagePoolOffset offset) const {
    return reinterpret_cast<P*>(resolve(offset));
  }
  template <typename P> P* resolve_newpage_cast(memory::PagePoolOffset offset) const {
    return reinterpret_cast<P*>(resolve_newpage(offset));
  }


  /**
   * Find the given page in snapshot cache, reading it if not found.
   */
  ErrorCode     find_or_read_a_snapshot_page(
    storage::SnapshotPagePointer page_id,
    storage::Page** out);
  /**
   * @brief Batched version of find_or_read_a_snapshot_page().
   * @param[in] batch_size Batch size. Must be kMaxFindPagesBatch or less.
   * @param[in] page_ids Array of Page IDs to look for, size=batch_size
   * @param[out] out Output
   * @details
   * This might perform much faster because of parallel prefetching, SIMD-ized hash
   * calculattion (planned, not implemented yet) etc.
   */
  ErrorCode     find_or_read_snapshot_pages_batch(
    uint16_t batch_size,
    const storage::SnapshotPagePointer* page_ids,
    storage::Page** out);

  /**
   * Read a snapshot page using the thread-local file descriptor set.
   * @attention this method always READs, so no caching done. Actually, this method is used
   * from caching module when cache miss happens. To utilize cache,
   * use find_or_read_a_snapshot_page().
   */
  ErrorCode     read_a_snapshot_page(storage::SnapshotPagePointer page_id, storage::Page* buffer);

  /** Read contiguous pages in one shot. Other than that same as read_a_snapshot_page(). */
  ErrorCode     read_snapshot_pages(
    storage::SnapshotPagePointer page_id_begin,
    uint32_t page_count,
    storage::Page* buffer);

  /**
   * @brief Installs a volatile page to the given dual pointer as a copy of the snapshot page.
   * @param[in,out] pointer dual pointer. volatile pointer will be modified.
   * @param[out] installed_page physical pointer to the installed volatile page. This might point
   * to a page installed by a concurrent thread.
   * @pre pointer->snapshot_pointer_ != 0 (this method is for a page that already has snapshot)
   * @pre pointer->volatile_pointer.components.offset == 0 (but not mandatory because
   * concurrent threads might have installed it right now)
   * @details
   * This is called when a dual pointer has only a snapshot pointer, in other words it is "clean",
   * to create a volatile version for modification.
   */
  ErrorCode     install_a_volatile_page(
    storage::DualPagePointer* pointer,
    storage::Page** installed_page);

  /**
   * @brief A general method to follow (read) a page pointer.
   * @param[in] page_initializer callback function in case we need to initialize a new volatile
   * page. null if it never happens (eg tolerate_null_pointer is false).
   * @param[in] tolerate_null_pointer when true and when both the volatile and snapshot pointers
   * seem null, we return null page rather than creating a new volatile page.
   * @param[in] will_modify if true, we always return a non-null volatile page. This is true
   * when we are to modify the page, such as insert/delete.
   * @param[in] take_ptr_set_snapshot if true, we add the address of volatile page pointer
   * to ptr set when we do not follow a volatile pointer (null or volatile). This is usually true
   * to make sure we get aware of new page installment by concurrent threads.
   * If the isolation level is not serializable, we don't take ptr set anyways.
   * @param[in,out] pointer the page pointer.
   * @param[out] page the read page.
   * @param[in] parent the parent page that contains a pointer to the page.
   * @param[in] index_in_parent Some index (meaning depends on page type) of pointer in
   * parent page to the page.
   * @pre !tolerate_null_pointer || !will_modify (if we are modifying the page, tolerating null
   * pointer doesn't make sense. we should always initialize a new volatile page)
   * @details
   * This is the primary way to retrieve a page pointed by a pointer in various places.
   * Depending on the current transaction's isolation level and storage type (represented by
   * the various arguments), this does a whole lots of things to comply with our commit protocol.
   *
   * Remember that DualPagePointer maintains volatile and snapshot pointers.
   * We sometimes have to install a new volatile page or add the pointer to ptr set
   * for serializability. That logic is a bit too lengthy method to duplicate in each page
   * type, so generalize it here.
   */
  ErrorCode     follow_page_pointer(
    storage::VolatilePageInit page_initializer,
    bool tolerate_null_pointer,
    bool will_modify,
    bool take_ptr_set_snapshot,
    storage::DualPagePointer* pointer,
    storage::Page** page,
    const storage::Page* parent,
    uint16_t index_in_parent);

  /**
   * @brief Batched version of follow_page_pointer with will_modify==false.
   * @param[in] batch_size Batch size. Must be kMaxFindPagesBatch or less.
   * @param[in] page_initializer callback function in case we need to initialize a new volatile
   * page. null if it never happens (eg tolerate_null_pointer is false).
   * @param[in] tolerate_null_pointer when true and when both the volatile and snapshot pointers
   * seem null, we return null page rather than creating a new volatile page.
   * @param[in] take_ptr_set_snapshot if true, we add the address of volatile page pointer
   * to ptr set when we do not follow a volatile pointer (null or volatile). This is usually true
   * to make sure we get aware of new page installment by concurrent threads.
   * If the isolation level is not serializable, we don't take ptr set anyways.
   * @param[in,out] pointers the page pointers.
   * @param[in] parents the parent page that contains a pointer to the page.
   * @param[in] index_in_parents Some index (meaning depends on page type) of pointer in
   * parent page to the page.
   * @param[in,out] followed_snapshots As input, must be same as parents[i]==followed_snapshots[i].
   * As output, same as out[i]->header().snapshot_. We receive/emit this to avoid accessing
   * page header.
   * @param[out] out the read page.
   * @note this method is guaranteed to work even if parents==out
   */
  ErrorCode follow_page_pointers_for_read_batch(
    uint16_t batch_size,
    storage::VolatilePageInit page_initializer,
    bool tolerate_null_pointer,
    bool take_ptr_set_snapshot,
    storage::DualPagePointer** pointers,
    storage::Page** parents,
    const uint16_t* index_in_parents,
    bool* followed_snapshots,
    storage::Page** out);

  /**
   * @brief Batched version of follow_page_pointer with will_modify==true and
   * tolerate_null_pointer==true.
   * @param[in] batch_size Batch size. Must be kMaxFindPagesBatch or less.
   * @param[in] page_initializer callback function in case we need to initialize a new volatile
   * page. null if it never happens (eg tolerate_null_pointer is false).
   * @param[in,out] pointers the page pointers.
   * @param[in] parents the parent page that contains a pointer to the page.
   * @param[in] index_in_parents Some index (meaning depends on page type) of pointer in
   * parent page to the page.
   * @param[out] out the read page.
   * @note this method is guaranteed to work even if parents==out
   */
  ErrorCode follow_page_pointers_for_write_batch(
    uint16_t batch_size,
    storage::VolatilePageInit page_initializer,
    storage::DualPagePointer** pointers,
    storage::Page** parents,
    const uint16_t* index_in_parents,
    storage::Page** out);

  /**
   * @brief Keeps the specified volatile page as retired as of the current epoch.
   * @param[in] ptr the volatile page that has been retired
   * @pre in the page ptr points to, is_retired()==true.
   * @details
   * This thread buffers such pages and returns to volatile page pool when it is safe to do so.
   */
  void          collect_retired_volatile_page(storage::VolatilePagePointer ptr);

  /** Unconditionally takes MCS lock on the given mcs_lock. */
  xct::McsRwSimpleBlock* get_mcs_rw_simple_blocks();
  xct::McsRwExtendedBlock* get_mcs_rw_extended_blocks();
  ///////////////////////////////////////////////////////////////////////////////
  ///
  ///  Methods related to Current Lock List (CLL)
  ///  These are the only interface in Thread to lock records. We previously had
  ///  methods to directly lock without CLL, but we now prohibit bypassing CLL.
  ///  CLL guarantees deadlock-free lock handling.
  ///  CLL only handle record locks. In FOEDUS, normal transactions never
  ///  take page lock. Only system transactions are allowed to take page locks.
  ///
  /** @copydoc foedus::xct::CurrentLockList::try_or_acquire_single_lock() */
  ErrorCode cll_try_or_acquire_single_lock(xct::LockListPosition pos);
  /** @copydoc foedus::xct::CurrentLockList::try_or_acquire_multiple_locks() */
  ErrorCode cll_try_or_acquire_multiple_locks(xct::LockListPosition upto_pos);
  /**
   * This \e gives-up locks in CLL that are not yet taken.
   * preferred mode will be set to either NoLock or same as taken_mode,
   * and all incomplete async locks will be cancelled.
   */
  void      cll_giveup_all_locks_after(xct::UniversalLockId address);
  void      cll_giveup_all_locks_at_and_after(xct::UniversalLockId address) {
    if (address == xct::kNullUniversalLockId) {
      cll_giveup_all_locks_after(xct::kNullUniversalLockId);
    } else {
      cll_giveup_all_locks_after(address - 1U);
    }
  }
  /**
   * Release all locks in CLL of this thread whose addresses are canonically ordered
   * before the parameter. This is used where we need to rule out the risk of deadlock.
   */
  void      cll_release_all_locks_after(xct::UniversalLockId address);
  /** same as mcs_release_all_current_locks_after(address - 1) */
  void      cll_release_all_locks_at_and_after(xct::UniversalLockId address) {
    if (address == xct::kNullUniversalLockId) {
      cll_release_all_locks_after(xct::kNullUniversalLockId);
    } else {
      cll_release_all_locks_after(address - 1U);
    }
  }
  void      cll_release_all_locks();

  ///////////////////////////////////////////////////////////////////////////////
  ///
  ///  Methods related to System transactions (sysxct) nested under this thread.
  ///
  /**
   * @see foedus::xct::run_nested_sysxct_impl()
   */
  ErrorCode run_nested_sysxct(xct::SysxctFunctor* functor, uint32_t max_retries = 0);
  /**
   * Takes a lock for a sysxct running under this thread.
   * @pre sysxct_workspace->running_sysxct_
   * @pre the record lock must be within the page of the given ID
   */
  ErrorCode sysxct_record_lock(
    xct::SysxctWorkspace* sysxct_workspace,
    storage::VolatilePagePointer page_id,
    xct::RwLockableXctId* lock);
  /**
   * Takes a bunch of locks in the same page for a sysxct running under this thread.
   * @pre sysxct_workspace->running_sysxct_
   * @pre the record locks must be within the page of the given ID
   */
  ErrorCode sysxct_batch_record_locks(
    xct::SysxctWorkspace* sysxct_workspace,
    storage::VolatilePagePointer page_id,
    uint32_t lock_count,
    xct::RwLockableXctId** locks);
  /**
   * Takes a page lock in the same page for a sysxct running under this thread.
   * @pre sysxct_workspace->running_sysxct_
   */
  ErrorCode sysxct_page_lock(xct::SysxctWorkspace* sysxct_workspace, storage::Page* page);
  /**
   * Takes a bunch of page locks for a sysxct running under this thread.
   * @pre sysxct_workspace->running_sysxct_
   */
  ErrorCode sysxct_batch_page_locks(
    xct::SysxctWorkspace* sysxct_workspace,
    uint32_t lock_count,
    storage::Page** pages);
  /// Currently we don't have sysxct_release_locks() etc. All locks will be automatically
  /// released when the sysxct ends. Probably this is enough as sysxct should be short-living.

  /** @see foedus::xct::InCommitEpochGuard  */
  Epoch*        get_in_commit_epoch_address();

  /** Returns the pimpl of this object. Use it only when you know what you are doing. */
  ThreadPimpl*  get_pimpl() const { return pimpl_; }

  inline assorted::UniformRandom& get_lock_rnd() { return lock_rnd_; }

  /**
   * @returns whether the given page had enough aborts to justify pessimisitic locking.
   * @ingroup RLL
   */
  bool          is_hot_page(const storage::Page* page) const;

  friend std::ostream& operator<<(std::ostream& o, const Thread& v);

 private:
  ThreadPimpl*    pimpl_;

  // The following should be in pimpl. later.
  assorted::UniformRandom lock_rnd_;
};

/**
 * Obtains multiple free volatile pages at once and releases them automatically
 * when this object gets out of scope.
 * You can also dispatch some of the grabbed pages, which means they will NOT be
 * released.
 */
class GrabFreeVolatilePagesScope {
 public:
  GrabFreeVolatilePagesScope(Thread* context, memory::PagePoolOffset* offsets)
    : context_(context), offsets_(offsets), count_(0) {}
  ~GrabFreeVolatilePagesScope() {
    release();
  }

  /**
    * If this thread doesn't have enough free pages, no page is obtained,
    * returning kErrorCodeMemoryNoFreePages.
    */
  ErrorCode grab(uint32_t count);
  /** Idempotent. You can release it at any moment. */
  void      release();
  uint32_t  get_count() const { return count_; }

  /** Call this when the page is placed somewhere */
  void                    dispatch(uint32_t index) {
    ASSERT_ND(index < count_);
    offsets_[index] = 0;  // This entry will be skipped in release()
  }
  memory::PagePoolOffset  get(uint32_t index) const {
    ASSERT_ND(index < count_);
    return offsets_[index];
  }

 private:
  Thread* const                 context_;
  memory::PagePoolOffset* const offsets_;
  uint32_t                      count_;
};

}  // namespace thread
}  // namespace foedus
#endif  // FOEDUS_THREAD_THREAD_HPP_
