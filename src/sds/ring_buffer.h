#pragma once

#include <cassert>
#include <cstddef>

namespace sds {

class ring_buffer {
protected:
    char*       buffer_;
    std::size_t capacity_;
    std::size_t head_;
    std::size_t tail_;

public:
    ring_buffer() = delete;
    explicit ring_buffer(std::size_t buffer_capacity);
    ~ring_buffer();

    ring_buffer(const ring_buffer&)            = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;
    ring_buffer(ring_buffer&&)                 = delete;
    ring_buffer& operator=(ring_buffer&&)      = delete;

protected:
    void resize_buffer(std::size_t new_capacity) noexcept;

public:
    inline bool is_empty() const noexcept { return head_ == tail_; }
    inline bool is_full()  const noexcept { return ((tail_ + 1) % capacity_) == head_; }

    inline std::size_t head_index() const noexcept { return head_; }
    inline std::size_t tail_index() const noexcept { return tail_; }
    inline std::size_t capacity()   const noexcept { return capacity_; }

    inline std::size_t used_size() const noexcept {
        if (tail_ >= head_) return tail_ - head_;
        return capacity_ - head_ + tail_;
    }
    inline std::size_t free_size() const noexcept { return capacity_ - used_size() - 1; }

    inline std::size_t move_head(std::size_t offset) noexcept {
        assert(offset <= used_size());
        head_ = (head_ + offset) % capacity_;
        return offset;
    }
    inline std::size_t move_tail(std::size_t offset) noexcept {
        assert(offset <= free_size());
        tail_ = (tail_ + offset) % capacity_;
        return offset;
    }

    inline std::size_t direct_enqueue_size() const noexcept {
        if (is_full()) return 0;
        if (tail_ >= head_) return (capacity_ - tail_) - ((head_ == 0) ? 1 : 0);
        return head_ - tail_ - 1;
    }

    inline std::size_t direct_dequeue_size() const noexcept {
        return (tail_ < head_) ? capacity_ - head_ : tail_ - head_;
    }

    std::size_t enqueue(const char* src, std::size_t size) noexcept;
    std::size_t dequeue(char* dst, std::size_t size) noexcept;
    std::size_t peek(char* dst, std::size_t size) const noexcept;
};

}  // namespace sds
