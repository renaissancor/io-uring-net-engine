#include "sds/ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace sds {

ring_buffer::ring_buffer(std::size_t buffer_capacity)
    : buffer_(nullptr), capacity_(buffer_capacity), head_(0), tail_(0) {
    if (capacity_ < 4) capacity_ = 4;
    buffer_ = new char[capacity_];
}

ring_buffer::~ring_buffer() {
    delete[] buffer_;
}

void ring_buffer::resize_buffer(std::size_t new_capacity) noexcept {
    if (new_capacity <= capacity_) return;
    char* new_buffer = new char[new_capacity];  // bad_alloc → terminate
    std::size_t used = used_size();
    peek(new_buffer, used);
    delete[] buffer_;
    buffer_ = new_buffer;
    capacity_ = new_capacity;
    head_ = 0;
    tail_ = used;
}

std::size_t ring_buffer::enqueue(const char* src, std::size_t bytes) noexcept {
    if (bytes == 0) return 0;
    if (bytes > free_size()) {
        std::size_t new_capacity = capacity_ * 2;
        while (new_capacity - used_size() - 1 < bytes) new_capacity *= 2;
        resize_buffer(new_capacity);
    }
    std::size_t first = std::min(bytes, direct_enqueue_size());
    std::memcpy(buffer_ + tail_, src, first);
    std::size_t remaining = bytes - first;
    if (remaining == 0) {
        tail_ = (tail_ + first) % capacity_;
    } else {
        std::memcpy(buffer_, src + first, remaining);
        tail_ = remaining;
    }
    return bytes;
}

std::size_t ring_buffer::dequeue(char* dst, std::size_t bytes) noexcept {
    if (bytes == 0) return 0;
    if (bytes > used_size()) bytes = used_size();
    std::size_t first = std::min(bytes, capacity_ - head_);
    std::memcpy(dst, buffer_ + head_, first);
    std::size_t remaining = bytes - first;
    if (remaining == 0) {
        head_ = (head_ + first) % capacity_;
    } else {
        std::memcpy(dst + first, buffer_, remaining);
        head_ = remaining;
    }
    return bytes;
}

std::size_t ring_buffer::peek(char* dst, std::size_t bytes) const noexcept {
    if (bytes == 0) return 0;
    if (bytes > used_size()) bytes = used_size();
    std::size_t first = std::min(bytes, capacity_ - head_);
    std::memcpy(dst, buffer_ + head_, first);
    std::size_t remaining = bytes - first;
    if (remaining > 0) {
        std::memcpy(dst + first, buffer_, remaining);
    }
    return bytes;
}

}  // namespace sds
