/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_LOG_THREAD_LOG_BUFFER_IMPL_HPP_
#define FOEDUS_LOG_THREAD_LOG_BUFFER_IMPL_HPP_
#include <foedus/fwd.hpp>
#include <foedus/initializable.hpp>
#include <foedus/log/fwd.hpp>
#include <foedus/memory/aligned_memory.hpp>
#include <foedus/thread/thread_id.hpp>
#include <foedus/xct/epoch.hpp>
#include <stdint.h>
#include <list>
#include <mutex>
namespace foedus {
namespace log {
/**
 * @brief A thread-local log buffer.
 * @ingroup LOG
 * @details
 * This is a private implementation-details of \ref LOG, thus file name ends with _impl.
 * Do not include this header from a client program unless you know what you are doing.
 *
 * @section CIR Circular Log Buffer
 * This class forms a circular buffer used by log appender (Thread), log writer (Logger),
 * and log gleaner (LogGleaner). We maintain four offsets on the buffer.
 * <table>
 * <tr><th>Marker</th><th>Read by</th><th>Written by</th><th>Description</th></tr>
 * <tr><td> get_offset_head() </td><td>Thread</td><td>Thread, LogGleaner</td>
 *   <td> @copydoc get_offset_head() </td></tr>
 * <tr><td> get_offset_durable() </td><td>Thread, LogGleaner</td><td>Logger</td>
 *   <td> @copydoc get_offset_durable() </td></tr>
 * <tr><td> get_offset_current_xct_begin() </td><td>Logger</td><td>Thread</td>
 *   <td> @copydoc get_offset_current_xct_begin() </td></tr>
 * <tr><td> get_offset_tail() </td><td>Thread</td><td>Thread</td>
 *   <td> @copydoc get_offset_tail() </td></tr>
 * </table>
 *
 * @section MARKER Epoch Marker
 * @copydoc ThreadEpockMark
 */
class ThreadLogBuffer final : public DefaultInitializable {
 public:
    friend class Logger;
    /** Subtract operator, considering wrapping around. */
    static uint64_t distance(uint64_t buffer_size, uint64_t from, uint64_t to) ALWAYS_INLINE {
        ASSERT_ND(from < buffer_size);
        ASSERT_ND(to < buffer_size);
        if (to >= from) {
            return from - to;
        } else {
            return from + buffer_size - to;
        }
    }
    /** Addition operator, considering wrapping around. */
    static void advance(uint64_t buffer_size, uint64_t *target, uint64_t advance) ALWAYS_INLINE {
        ASSERT_ND(*target < buffer_size);
        ASSERT_ND(advance < buffer_size);
        *target += advance;
        if (*target >= buffer_size) {
            *target -= buffer_size;
        }
    }
    /**
     * @brief Indicates where this thread switched an epoch.
     * @details
     * When the thread publishes a commited log with new epoch, it adds this mark for logger.
     * Unlike logger's epoch mark, we don't write out actual log entry for this.
     * Epoch mark is stored for only non-durable regions. Thus, the logger doesn't have to
     * worry about whether the marked offset is still valid or not.
     */
    struct ThreadEpockMark {
        /**
         * The value of the thread's current_epoch_ before the switch.
         * This is not currently used except sanity checks.
         */
        xct::Epoch  old_epoch_;
        /**
         * The value of the thread's current_epoch_ after the switch.
         */
        xct::Epoch  new_epoch_;
        /**
         * Where the new epoch starts.
         * @invariant offset_durable_ <= offset_epoch_begin_ < offset_current_xct_begin_.
         */
        uint64_t    offset_epoch_begin_;
    };

    ThreadLogBuffer(Engine* engine, thread::ThreadId thread_id);
    ErrorStack  initialize_once() override;
    ErrorStack  uninitialize_once() override;

    ThreadLogBuffer() = delete;
    ThreadLogBuffer(const ThreadLogBuffer &other) = delete;
    ThreadLogBuffer& operator=(const ThreadLogBuffer &other) = delete;

    void        assert_consistent_offsets() const;
    thread::ThreadId get_thread_id() const { return thread_id_; }

    /**
     * @brief The in-memory log buffer given to this thread.
     * @details
     * This forms a circular buffer to which \e this thread (the owner of this buffer)
     * will append log entries, and from which log writer will read from head.
     * This is a piece of NumaNodeMemory#thread_buffer_memory_.
     */
    char*       get_buffer() { return buffer_; }

    /** Size of the buffer assigned to this thread. */
    uint64_t    get_buffer_size() const { return buffer_size_; }

    /**
     * @brief buffer_size_ - 64.
     * @details
     * We always leave some \e hole between offset_tail_ and offset_head_
     * to avoid the case offset_tail_ == offset_head_ (log empty? or log full?).
     * One classic way to handle this case is to store \e count rather than offsets, but
     * it makes synchronization between log writer and this thread expensive.
     * Rather, we sacrifice a negligible space.
     */
    uint64_t    get_buffer_size_safe() const { return buffer_size_safe_; }

    /**
     * @brief Reserves a space for a new (uncommitted) log entry at the tail.
     * @param[in] log_length byte size of the log. You have to give it beforehand.
     * @details
     * If the circular buffer's tail reaches the head, this method might block.
     * But it will be rare as we release a large region of buffer at each time.
     */
    char*       reserve_new_log(uint16_t log_length) ALWAYS_INLINE {
        if (UNLIKELY(
            distance(buffer_size_, offset_tail_, offset_head_) + log_length >= buffer_size_safe_)) {
            wait_for_space(log_length);
        }
        ASSERT_ND(distance(buffer_size_, offset_tail_, offset_head_) + log_length
            < buffer_size_safe_);
        char *buffer = buffer_ + offset_tail_;
        advance(buffer_size_, &offset_tail_, log_length);
        return buffer;
    }

    /**
     * Called when the current transaction is successfully committed.
     */
    void        publish_current_xct_log(const xct::Epoch &commit_epoch) ALWAYS_INLINE {
        ASSERT_ND(commit_epoch >= current_epoch_);
        if (UNLIKELY(commit_epoch > current_epoch_)) {
            add_thread_epock_mark(commit_epoch);  // epoch switches!
        }
        offset_current_xct_begin_ = offset_tail_;
    }
    /** Called when the current transaction aborts. */
    void        discard_current_xct_log() {
        offset_tail_ = offset_current_xct_begin_;
    }

    /** Called when we have to wait till offset_head_ advances so that we can put new logs. */
    void        wait_for_space(uint16_t required_space);

    /**
     * @brief This marks the position where log entries start.
     * @details
     * This private log buffer is a circular buffer where the \e head is eaten by log gleaner.
     * However, log gleaner is okay to get behind, reading from log file instead (but slower).
     * Thus, offset_head_ is advanced either by log gleaner or this thread.
     * If the latter happens, log gleaner has to give up using in-memory logs and instead
     * read from log files.
     */
    uint64_t    get_offset_head() const { return offset_head_; }
    /** @copydoc get_offset_head() */
    void        set_offset_head(uint64_t value) { offset_head_ = value; }

    /**
     * @brief This marks the position upto which the log writer durably wrote out to log files.
     * @details
     * Everything after this position must not be discarded because they are not yet durable.
     * When the log writer reads log entries after here, writes them to log file, and calls fsync,
     * this variable is advanced by the log writer.
     * This variable is read by this thread to check the end of the circular buffer.
     */
    uint64_t    get_offset_durable() const { return offset_durable_; }
    /** @copydoc get_offset_durable() */
    void        set_offset_durable(uint64_t value) { offset_durable_ = value; }

    /**
     * @brief The beginning of logs for current transaction.
     * @details
     * Log writers can safely read log entries and write them to log files up to this place.
     * When the transaction commits, this value is advanced by the thread.
     * The only possible update pattern to this variable is \b advance by this thread.
     * Thus, the log writer can safely read this variable without any fence or lock
     * thanks to regular (either old value or new value, never garbage) read of 64-bit.
     */
    uint64_t    get_offset_current_xct_begin() const { return offset_current_xct_begin_; }

    /**
     * @brief The current cursor to which next log will be written.
     * @details
     * This is the location the current transaction of this thread is writing to \b before commit.
     * When the transaction commits, offset_current_xct_begin_ catches up with this.
     * When the transaction aborts, this value rolls back to offset_current_xct_begin_.
     * Only this thread reads/writes to this variable. No other threads access this.
     */
    uint64_t    get_offset_tail() const { return offset_tail_; }

 private:
    /** called from publish_current_xct_log when we have to switch the epoch. */
    void        add_thread_epock_mark(const xct::Epoch &commit_epoch);

    /**
     * @return whether any epoch mark was consumed.
     */
    bool        consume_epoch_mark();

    Engine*                         engine_;
    thread::ThreadId                thread_id_;

    memory::AlignedMemorySlice      buffer_memory_;
    /** @copydoc get_buffer() */
    char*                           buffer_;
    /** @copydoc get_buffer_size() */
    uint64_t                        buffer_size_;
    /** @copydoc get_buffer_size_safe() */
    uint64_t                        buffer_size_safe_;

    /** @copydoc get_offset_head() */
    uint64_t                        offset_head_;
    /** @copydoc get_offset_durable() */
    uint64_t                        offset_durable_;
    /** @copydoc get_offset_current_xct_begin() */
    uint64_t                        offset_current_xct_begin_;
    /** @copydoc get_offset_tail() */
    uint64_t                        offset_tail_;

    /**
     * The previous epoch the most recent transaction of \e this thread writes out logs.
     * So, it is probably older than the global current epoch.
     * This is only read/written by this thread.
     */
    xct::Epoch                      current_epoch_;

    /**
     * Upto what epoch the logger flushed logs in this buffer.
     * This is only read/written by the logger.
     */
    xct::Epoch                      durable_epoch_;

    /**
     * This is the epoch the logger is currently flushing.
     * The logger writes out the log entries in this epoch.
     * This value is 0 only when the logger has not visited this buffer.
     * This is only read/written by the logger and updated when the logger consumes ThreadEpockMark.
     * @invariant current_epoch_ >= logger_epoch_ > durable_epoch_ (except 0)
     */
    xct::Epoch                      logger_epoch_;
    /**
     * @brief Whether the logger is aware of where log entries for logger_epoch_ ends.
     * @details
     * For example, when the \e global current epoch is 3 and this thread has already written some
     * log in epoch-3, the logger will be aware of where log entries for epoch-2 end via the
     * epoch mark. However, the logger has no idea where log entries for epoch-3
     * will end because this thread will still write out more logs in the epoch!
     * In other words, this value is false if the logger is lagging behind, true if it's catching
     * up well.
     */
    bool                            logger_epoch_open_ended_;
    /**
     * The position where log entries for logger_epoch_ ends (exclusive).
     * The value is undefined when logger_epoch_open_ended_ is true.
     * @invariant current_epoch_ >= logger_epoch_ends_ > offset_durable_
     */
    uint64_t                        logger_epoch_ends_;

    /**
     * @brief Currently active epoch marks that are waiting to be consumed by the logger.
     * @details
     * The older marks come first. For example, it might be like this:
     * \li offset_head_=0, offset_durable_=128, current_epoch_=7, durable_epoch_=0.
     * \li Mark 0: Switched from epoch-3 to epoch-4 at offset=128.
     * \li Mark 1: Switched from epoch-4 to epoch-6 at offset=1024.
     * \li Mark 2: Switched from epoch-6 to epoch-7 at offset=4096.
     * Then, logger comes by and consumes/removes Mark-0, writes out until offset 1024, setting
     * offset_durable_=1024, durable_epoch_=4.
     *
     * In another example where the logger is well catching up with this thread, this vector
     * might be empty. In that case, logger_epoch_open_ended_ would be true.
     */
    std::list< ThreadEpockMark >    thread_epoch_marks_;
    /**
     * @brief Protects all accesses to thread_epoch_marks_.
     * @details
     * We don't have to access thread_epoch_marks_ so often; only when an epoch switches and
     * when a logger comes by, which handles a bulk of log entries at once. Thus, this mutex and
     * list above won't be a bottleneck.
     */
    std::mutex                      thread_epoch_marks_mutex_;
};
}  // namespace log
}  // namespace foedus
#endif  // FOEDUS_LOG_THREAD_LOG_BUFFER_IMPL_HPP_

