#pragma once

#include <cstddef>
#include <iterator>
#include <utility>

namespace sds {

template <typename V>
class cstr_hash_map {
private:
    static constexpr float kLoadFactor = 0.75F;

    struct node {
        const char* key;
        V           value;
        node*       next;

        node(const char* node_key, V node_value)
            : key(node_key), value(std::move(node_value)), next(nullptr) {}
    };

    node**      buckets_;
    std::size_t capacity_;
    std::size_t size_;

    static std::size_t next_power_of_two(std::size_t value) noexcept {
        if (value <= 1) return 1;
        --value;
        for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
            value |= value >> shift;
        }
        return value + 1;
    }

    static int cstr_cmp(const char* lhs, const char* rhs) noexcept {
        if (lhs == rhs) return 0;
        while (*lhs != '\0' && *lhs == *rhs) {
            ++lhs;
            ++rhs;
        }
        return static_cast<unsigned char>(*lhs) - static_cast<unsigned char>(*rhs);
    }

    static std::size_t hash_func(const char* key) noexcept {
        std::size_t hash = 5381;
        while (*key != '\0') {
            hash = ((hash << 5) + hash) + static_cast<unsigned char>(*key);
            ++key;
        }
        return hash;
    }

    std::size_t bucket_index(std::size_t hash) const noexcept {
        return hash & (capacity_ - 1);
    }

    void rehash_to(std::size_t new_capacity) noexcept {
        new_capacity = next_power_of_two(new_capacity);
        node** new_buckets = new node*[new_capacity]();

        for (std::size_t i = 0; i < capacity_; ++i) {
            node* current = buckets_[i];
            while (current != nullptr) {
                node* next = current->next;
                const std::size_t index = hash_func(current->key) & (new_capacity - 1);
                current->next = new_buckets[index];
                new_buckets[index] = current;
                current = next;
            }
        }

        delete[] buckets_;
        buckets_ = new_buckets;
        capacity_ = new_capacity;
    }

    void rehash_if_needed_for_insert() noexcept {
        if (capacity_ == 0) {
            rehash_to(64);
            return;
        }
        if (size_ + 1 > static_cast<std::size_t>(static_cast<float>(capacity_) * kLoadFactor)) {
            rehash_to(capacity_ * 2);
        }
    }

public:
    class iterator {
    private:
        node*       node_;
        node**      buckets_;
        std::size_t capacity_;
        std::size_t bucket_index_;

    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type   = std::ptrdiff_t;
        using value_type        = std::pair<const char*, V>;

        struct reference {
            const char* first;
            V&          second;

            const char* key() const noexcept { return first; }
            V&          value() noexcept { return second; }
            const V&    value() const noexcept { return second; }
        };

        class pointer {
        private:
            reference ref_;

        public:
            explicit pointer(node* current) noexcept
                : ref_{current->key, current->value} {}

            reference* operator->() noexcept { return &ref_; }
            const reference* operator->() const noexcept { return &ref_; }
        };

        iterator() noexcept
            : node_(nullptr), buckets_(nullptr), capacity_(0), bucket_index_(0) {}

        iterator(node* current, node** buckets, std::size_t capacity,
                 std::size_t bucket_index) noexcept
            : node_(current), buckets_(buckets), capacity_(capacity),
              bucket_index_(bucket_index) {}

        const char* key() const noexcept { return node_->key; }
        V&          value() noexcept { return node_->value; }
        const V&    value() const noexcept { return node_->value; }

        reference operator*() noexcept {
            return {node_->key, node_->value};
        }

        reference operator*() const noexcept {
            return {node_->key, node_->value};
        }

        pointer operator->() noexcept { return pointer{node_}; }
        pointer operator->() const noexcept { return pointer{node_}; }

        iterator& operator++() noexcept {
            if (node_ != nullptr && node_->next != nullptr) {
                node_ = node_->next;
                return *this;
            }

            for (++bucket_index_; bucket_index_ < capacity_; ++bucket_index_) {
                if (buckets_[bucket_index_] != nullptr) {
                    node_ = buckets_[bucket_index_];
                    return *this;
                }
            }

            node_ = nullptr;
            bucket_index_ = capacity_;
            return *this;
        }

        iterator operator++(int) noexcept {
            iterator previous = *this;
            ++(*this);
            return previous;
        }

        bool operator==(const iterator& other) const noexcept {
            return node_ == other.node_;
        }

        bool operator!=(const iterator& other) const noexcept {
            return node_ != other.node_;
        }
    };

    class const_iterator {
    private:
        const node*  node_;
        node* const* buckets_;
        std::size_t capacity_;
        std::size_t bucket_index_;

    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type   = std::ptrdiff_t;
        using value_type        = std::pair<const char*, V>;

        struct reference {
            const char* first;
            const V&    second;

            const char* key() const noexcept { return first; }
            const V&    value() const noexcept { return second; }
        };

        class pointer {
        private:
            reference ref_;

        public:
            explicit pointer(const node* current) noexcept
                : ref_{current->key, current->value} {}

            const reference* operator->() const noexcept { return &ref_; }
        };

        const_iterator() noexcept
            : node_(nullptr), buckets_(nullptr), capacity_(0), bucket_index_(0) {}

        const_iterator(const node* current, node* const* buckets, std::size_t capacity,
                       std::size_t bucket_index) noexcept
            : node_(current), buckets_(buckets), capacity_(capacity),
              bucket_index_(bucket_index) {}

        const char* key() const noexcept { return node_->key; }
        const V&    value() const noexcept { return node_->value; }

        reference operator*() const noexcept {
            return {node_->key, node_->value};
        }

        pointer operator->() const noexcept { return pointer{node_}; }

        const_iterator& operator++() noexcept {
            if (node_ != nullptr && node_->next != nullptr) {
                node_ = node_->next;
                return *this;
            }

            for (++bucket_index_; bucket_index_ < capacity_; ++bucket_index_) {
                if (buckets_[bucket_index_] != nullptr) {
                    node_ = buckets_[bucket_index_];
                    return *this;
                }
            }

            node_ = nullptr;
            bucket_index_ = capacity_;
            return *this;
        }

        const_iterator operator++(int) noexcept {
            const_iterator previous = *this;
            ++(*this);
            return previous;
        }

        bool operator==(const const_iterator& other) const noexcept {
            return node_ == other.node_;
        }

        bool operator!=(const const_iterator& other) const noexcept {
            return node_ != other.node_;
        }
    };

    explicit cstr_hash_map(std::size_t capacity = 64)
        : buckets_(new node*[next_power_of_two(capacity)]()),
          capacity_(next_power_of_two(capacity)),
          size_(0) {}

    ~cstr_hash_map() noexcept {
        clear();
        delete[] buckets_;
    }

    cstr_hash_map(const cstr_hash_map&)            = delete;
    cstr_hash_map& operator=(const cstr_hash_map&) = delete;

    cstr_hash_map(cstr_hash_map&& other) noexcept
        : buckets_(other.buckets_), capacity_(other.capacity_), size_(other.size_) {
        other.buckets_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
    }

    cstr_hash_map& operator=(cstr_hash_map&& other) noexcept {
        if (this == &other) return *this;

        clear();
        delete[] buckets_;

        buckets_ = other.buckets_;
        capacity_ = other.capacity_;
        size_ = other.size_;

        other.buckets_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    float       load_factor() const noexcept {
        if (capacity_ == 0) return 0.0F;
        return static_cast<float>(size_) / static_cast<float>(capacity_);
    }
    std::size_t capacity() const noexcept { return capacity_; }

    void reserve(std::size_t new_capacity) noexcept {
        if (new_capacity > capacity_) rehash_to(new_capacity);
    }

    iterator begin() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (buckets_[i] != nullptr) return iterator(buckets_[i], buckets_, capacity_, i);
        }
        return end();
    }

    iterator end() noexcept {
        return iterator(nullptr, buckets_, capacity_, capacity_);
    }

    const_iterator begin() const noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (buckets_[i] != nullptr) return const_iterator(buckets_[i], buckets_, capacity_, i);
        }
        return end();
    }

    const_iterator end() const noexcept {
        return const_iterator(nullptr, buckets_, capacity_, capacity_);
    }

    iterator find(const char* key) noexcept {
        if (capacity_ == 0) return end();

        const std::size_t index = bucket_index(hash_func(key));
        node* current = buckets_[index];
        while (current != nullptr) {
            if (cstr_cmp(current->key, key) == 0) return iterator(current, buckets_, capacity_, index);
            current = current->next;
        }
        return end();
    }

    const_iterator find(const char* key) const noexcept {
        if (capacity_ == 0) return end();

        const std::size_t index = bucket_index(hash_func(key));
        const node* current = buckets_[index];
        while (current != nullptr) {
            if (cstr_cmp(current->key, key) == 0) return const_iterator(current, buckets_, capacity_, index);
            current = current->next;
        }
        return end();
    }

    bool contains(const char* key) const noexcept {
        return find(key) != end();
    }

    void insert(const char* key, V value) noexcept {
        const std::size_t hash = hash_func(key);
        if (capacity_ == 0) rehash_to(64);

        std::size_t index = bucket_index(hash);
        node* current = buckets_[index];

        while (current != nullptr) {
            if (cstr_cmp(current->key, key) == 0) {
                current->value = std::move(value);
                return;
            }
            current = current->next;
        }

        rehash_if_needed_for_insert();
        index = bucket_index(hash);

        node* new_node = new node(key, std::move(value));
        new_node->next = buckets_[index];
        buckets_[index] = new_node;
        ++size_;
    }

    V& operator[](const char* key) noexcept {
        const std::size_t hash = hash_func(key);
        if (capacity_ == 0) rehash_to(64);

        std::size_t index = bucket_index(hash);
        node* current = buckets_[index];

        while (current != nullptr) {
            if (cstr_cmp(current->key, key) == 0) return current->value;
            current = current->next;
        }

        rehash_if_needed_for_insert();
        index = bucket_index(hash);

        node* new_node = new node(key, V{});
        new_node->next = buckets_[index];
        buckets_[index] = new_node;
        ++size_;
        return new_node->value;
    }

    void erase(const char* key) noexcept {
        if (capacity_ == 0) return;

        const std::size_t index = bucket_index(hash_func(key));
        node* current = buckets_[index];
        node* previous = nullptr;

        while (current != nullptr) {
            if (cstr_cmp(current->key, key) == 0) {
                if (previous == nullptr) {
                    buckets_[index] = current->next;
                } else {
                    previous->next = current->next;
                }
                delete current;
                --size_;
                return;
            }

            previous = current;
            current = current->next;
        }
    }

    void clear() noexcept {
        if (buckets_ == nullptr) return;

        for (std::size_t i = 0; i < capacity_; ++i) {
            node* current = buckets_[i];
            while (current != nullptr) {
                node* next = current->next;
                delete current;
                current = next;
            }
            buckets_[i] = nullptr;
        }
        size_ = 0;
    }
};

}  // namespace sds
