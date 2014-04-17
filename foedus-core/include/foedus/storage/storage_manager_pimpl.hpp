/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_STORAGE_STORAGE_MANAGER_PIMPL_HPP_
#define FOEDUS_STORAGE_STORAGE_MANAGER_PIMPL_HPP_
#include <foedus/fwd.hpp>
#include <foedus/initializable.hpp>
#include <foedus/storage/fwd.hpp>
#include <foedus/storage/storage_id.hpp>
#include <mutex>
namespace foedus {
namespace storage {
/**
 * @brief Pimpl object of StorageManager.
 * @ingroup STORAGE
 * @details
 * A private pimpl object for StorageManager.
 * Do not include this header from a client program unless you know what you are doing.
 */
class StorageManagerPimpl : public DefaultInitializable {
 public:
    StorageManagerPimpl() = delete;
    explicit StorageManagerPimpl(Engine* engine);
    ErrorStack  initialize_once() override;
    ErrorStack  uninitialize_once() override;

    StorageId   issue_next_storage_id();
    Storage*    get_storage(StorageId id);
    ErrorStack  register_storage(Storage* storage);
    ErrorStack  remove_storage(StorageId id);
    ErrorStack  expand_storage_array(StorageId new_size);

    Engine* const           engine_;

    /**
     * In case there are multiple threads that add/delete/expand storages,
     * those threads take this lock.
     * Normal threads that only read storages_ don't have to take this.
     */
    std::mutex              mod_lock_;

    /**
     * The largest StorageId we so far observed.
     * This value +1 would be the ID of the storage created next.
     */
    StorageId               largest_storage_id_;

    /**
     * Pointers of all Storage objects in this engine.
     * If there is a hole, it contains a nullptr.
     */
    Storage**               storages_;
    /**
     * Capacity of storages_. When we need an expansion, we do RCU and switches the pointer.
     */
    size_t                  storages_capacity_;
};
}  // namespace storage
}  // namespace foedus
#endif  // FOEDUS_STORAGE_STORAGE_MANAGER_PIMPL_HPP_