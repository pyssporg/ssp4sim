#include "utils/primitives/ring_buffer.hpp"

#include <sstream>
#include <stdexcept>

namespace ssp4sim::utils
{
    // There are almost no bound checks in this class
    // use with care...

    /**
     * @brief Constructor: allocate a fixed-capacity ring of byte slots.
     *
     * Sizes the timestamp/populated vectors to \p capacity and allocates one
     * contiguous aligned block of `capacity * buffer_size` bytes via
     * AlignedBufferPool. Every slot starts unused (no data written yet).
     *
     * @param capacity    number of slots
     * @param buffer_size bytes per slot
     * @throws std::runtime_error if capacity == 0
     */
    RingBuffer::RingBuffer(size_t capacity, size_t buffer_size)
        : log(ssp4cpp::utils::log::make_logger("ssp4sim.utils.RingBuffer")),
          timestamps(capacity),
          populated(capacity)
    {
        LOG_TRACE_L2(log, "[{func}] Constructor", __func__);
        if (capacity == 0)
        {
            throw std::runtime_error("[RingBuffer] buffer_size != 0");
        }
        this->capacity = capacity;
        
        buffers = AlignedBufferPool(buffer_size, this->capacity);
        
        for (size_t i = 0; i < this->capacity; i++)
        {
            populated[i] = false;
        }
    }

    /**
     * @brief Advance to the next slot, marking it populated, without a timestamp.
     *
     * Increments write_count, wraps `head = write_count % capacity`, marks that
     * slot as populated, and returns the new head index.
     */
    std::size_t RingBuffer::push()
    {
        IF_LOG({
            LOG_TRACE_L2(log, "[{func}] init", __func__);
        });

        write_count += 1;
        head = write_count % capacity;
        populated[head] = true;

        return head;
    }

    /**
     * @brief Advance to the next slot and stamp it with \p time.
     *
     * Writes \p time into the slot ABOUT to become head *before* advancing
     * head. Doing it the other way round lets head briefly point at a very old
     * timestamp, causing find_latest_valid_index to return a stale area.
     */
    std::size_t RingBuffer::push(std::uint64_t time)
    {
        // assign new time before increasing write_count and head
        // doing the opposite gives a chance that head points to a very old time that will fullfill the find_latest_valid_index and return something that is wrong
        // timestamps[write_count + 1% capacity] = time;
        timestamps[(write_count+1) % capacity] = time;
        auto head = push();
        return head;
    }

    void RingBuffer::serialize(serialize::Writer &out) const
    {
        serialize(out, {});
    }

    void RingBuffer::serialize(
        serialize::Writer &out,
        std::span<const serialize::ByteRange> zero_ranges) const
    {
        serialize::write_size(out, buffers.original_size);
        serialize::write_size(out, buffers.aligned_size);
        serialize::write_size(out, capacity);
        serialize::write_size(out, head);
        serialize::write_size(out, write_count);

        serialize::write_u64_vector(out, timestamps);
        serialize::write_bool_vector(out, populated);

        const std::span<const std::byte> data =
            buffers.total_size > 0 && buffers.data
                ? std::span<const std::byte>(buffers.data.get(), buffers.total_size)
                : std::span<const std::byte>{};

        serialize::write_masked_blob(out, data, zero_ranges);
    }

    void RingBuffer::deserialize(serialize::Reader &in)
    {
        deserialize(in, {});
    }

    void RingBuffer::deserialize(
        serialize::Reader &in,
        std::span<const serialize::ByteRange> skip_ranges)
    {
        const std::size_t serialized_original_size = serialize::read_size(in);
        const std::size_t serialized_aligned_size = serialize::read_size(in);
        const std::size_t serialized_capacity = serialize::read_size(in);
        const std::size_t serialized_head = serialize::read_size(in);
        const std::size_t serialized_write_count = serialize::read_size(in);

        std::vector<std::uint64_t> serialized_timestamps =
            serialize::read_u64_vector(in);
        const std::vector<bool> serialized_populated =
            serialize::read_bool_vector(in);

        serialize::require_equal("ring capacity", capacity, serialized_capacity);
        serialize::require_equal("ring original_size", buffers.original_size,
                                 serialized_original_size);
        serialize::require_equal("ring aligned_size", buffers.aligned_size,
                                 serialized_aligned_size);
        serialize::require_equal("ring timestamps.size()", capacity,
                                 serialized_timestamps.size());
        serialize::require_equal("ring populated.size()", capacity,
                                 serialized_populated.size());

        std::span<std::byte> target =
            buffers.total_size > 0 && buffers.data
                ? std::span<std::byte>(buffers.data.get(), buffers.total_size)
                : std::span<std::byte>{};
        serialize::read_masked_blob(in, target, skip_ranges);

        head = serialized_head;
        write_count = serialized_write_count;
        timestamps = std::move(serialized_timestamps);
        populated = serialized_populated;
    }

    /**
     * @brief Return the raw byte pointer for slot \p index.
     *
     * @param index slot to read/write
     * @param use_verification if true (default), reject unpopulated slots
     * @throws std::runtime_error if verification on and slot never written
     */
    std::byte *RingBuffer::get_item(std::size_t index, bool use_verification)
    {
        if (use_verification && !populated[index]) [[unlikely]]
        {
            LOG_ERROR(log, "[{func}] RingBuffer, index not populated: {index}", __func__, index);
            throw std::runtime_error("[RingBuffer][get_item] Index not populated");
        }
        return buffers.locations[index];
    }

    /**
     * @brief Timestamp recorded for slot \p index.
     * @throws std::runtime_error if the slot was never written.
     */
    std::uint64_t RingBuffer::get_time(std::size_t index)
    {
        if (!populated[index]) [[unlikely]]
        {
            LOG_ERROR(log, "[{func}] RingBuffer, index not populated: {index}", __func__, index);
            throw std::runtime_error("[RingBuffer][get_time] Index not populated");
        }
        return timestamps[index];
    }

    /**
     * @brief Exact timestamp match.
     * @return true and set \p index_found to the newest slot with time == \p time,
     *         else false.
     *
     * Scans newest-first; write_count is snapshotted locally so a concurrent
     * push cannot make the loop miss or double-process a slot.
     */
    bool RingBuffer::find_exact_index(uint64_t time, std::size_t &index_found)
    {
        // store local_write_count before iteration since there is a risk of it changing during the loop risking missing or processing an item twice
        auto local_write_count = write_count;
        for (std::size_t i = 0; i < local_write_count && i < capacity; ++i)
        {
            int pos = (local_write_count - i) % capacity;
            if (timestamps[pos] == time)
            {
                IF_LOG({
                    LOG_TRACE_L1(log, "[{func}] found valid index, {index}", __func__, pos);
                });

                index_found = pos;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Newest slot whose time is <= \p time.
     * @return true and set \p index_found, else false if none qualifies.
     */
    bool RingBuffer::find_latest_valid_index(uint64_t time, std::size_t &index_found)
    {
        // store local_write_count before iteration since there is a risk of it changing during the loop risking missing or processing an item twice

        auto local_write_count = write_count;
        for (std::size_t i = 0; i < local_write_count && i < capacity; ++i)
        {
            int pos = (local_write_count - i) % capacity;
            if (timestamps[pos] <= time)
            {
                IF_LOG({
                    LOG_TRACE_L1(log, "[{func}] found valid area, {}", __func__, pos);
                });

                index_found = pos;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Map a logical backwards position to a physical slot.
     * position 0 == head (newest), 1 == just before head, 2 == two back, ...
     */
    std::size_t RingBuffer::index_back_from_head(std::size_t position)
    {
        return (write_count - position) % capacity;
    }

    /** @brief True if no slot has ever been written. */
    bool RingBuffer::is_empty()
    {
        return write_count == 0;
    }

    /** @brief True once the buffer has wrapped at least once. */
    bool RingBuffer::is_full()
    {
        return write_count >= capacity;
    }

    /** @brief True if physical slot \p index has been written at least once. */
    bool RingBuffer::is_populated(std::size_t index)
    {
        if (index >= capacity)
        {
            return false;
        }
        return populated[index];
    }

    /** @brief Human-readable summary of capacity / inserts / head. */
    std::string RingBuffer::to_string() const
    {
        std::ostringstream oss;
        oss << "SignalStorage \n{\n"
            << ", capacity: " << capacity
            << "  write_count: " << write_count
            << ", head: " << head
            << "\n}";
        return oss.str();
    }


/**
     * @brief Newest slot whose time is strictly greater than \p time.
     * @return true and set \p index_found, else false if none qualifies.
     *
     * Searches newest-first; inverse of find_latest_valid_index.
     */
bool RingBuffer::find_next_valid_index(uint64_t time, std::size_t &index_found)
    {
        auto local_write_count = write_count;
        for (std::size_t i = 0; i < local_write_count && i < capacity; ++i)
        {
            int pos = (local_write_count - i) % capacity;
            if (timestamps[pos] > time)
            {
                IF_LOG({
                    LOG_TRACE_L1(log, "[{func}] found next valid index, {index}", __func__, pos);
                });

                index_found = pos;
                return true;
            }
        }
        return false;
    }
}

    