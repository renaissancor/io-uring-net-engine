#pragma once
// malloc_vector.h

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace sds {

template <typename T>
class malloc_vector {
private:
    static_assert(std::is_trivially_copyable_v<T>,
                  "malloc_vector stores trivially copyable types only");

    T*          _data;
    std::size_t _size;
    std::size_t _capacity;

public:
    class iterator {
    private:
        T* _ptr;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using difference_type   = std::ptrdiff_t;
        using value_type        = T;

        explicit iterator(T* ptr) noexcept : _ptr(ptr) {}

        T& operator*() noexcept { return *_ptr; }
        T* operator->() noexcept { return _ptr; }

        iterator& operator++() noexcept {
            ++_ptr;
            return *this;
        }

        iterator operator++(int) noexcept {
            iterator previous = *this;
            ++(*this);
            return previous;
        }

        iterator& operator--() noexcept {
            --_ptr;
            return *this;
        }

        iterator operator--(int) noexcept {
            iterator previous = *this;
            --(*this);
            return previous;
        }

        bool operator==(const iterator& other) const noexcept { return _ptr == other._ptr; }
        bool operator!=(const iterator& other) const noexcept { return _ptr != other._ptr; }
    };

    class const_iterator {
    private:
        const T* _ptr;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using difference_type   = std::ptrdiff_t;
        using value_type        = T;

        explicit const_iterator(const T* ptr) noexcept : _ptr(ptr) {}

        const T& operator*() const noexcept { return *_ptr; }
        const T* operator->() const noexcept { return _ptr; }

        const_iterator& operator++() noexcept {
            ++_ptr;
            return *this;
        }

        const_iterator operator++(int) noexcept {
            const_iterator previous = *this;
            ++(*this);
            return previous;
        }

        const_iterator& operator--() noexcept {
            --_ptr;
            return *this;
        }

        const_iterator operator--(int) noexcept {
            const_iterator previous = *this;
            --(*this);
            return previous;
        }

        bool operator==(const const_iterator& other) const noexcept {
            return _ptr == other._ptr;
        }

        bool operator!=(const const_iterator& other) const noexcept {
            return _ptr != other._ptr;
        }
    };

    explicit malloc_vector(std::size_t capacity = 4) noexcept
        : _data(nullptr), _size(0), _capacity(0) {
        if (capacity > 0) {
            _data = static_cast<T*>(std::malloc(sizeof(T) * capacity));
            if (_data != nullptr) _capacity = capacity;
        }
    }

    ~malloc_vector() noexcept {
        std::free(_data);
    }

    malloc_vector(const malloc_vector&)            = delete;
    malloc_vector& operator=(const malloc_vector&) = delete;

    malloc_vector(malloc_vector&& other) noexcept
        : _data(other._data), _size(other._size), _capacity(other._capacity) {
        other._data = nullptr;
        other._size = 0;
        other._capacity = 0;
    }

    malloc_vector& operator=(malloc_vector&& other) noexcept {
        if (this == &other) return *this;

        std::free(_data);
        _data = other._data;
        _size = other._size;
        _capacity = other._capacity;

        other._data = nullptr;
        other._size = 0;
        other._capacity = 0;
        return *this;
    }

    iterator begin() noexcept { return iterator(_data); }
    iterator end() noexcept { return iterator(_data == nullptr ? nullptr : _data + _size); }
    const_iterator begin() const noexcept { return const_iterator(_data); }
    const_iterator end() const noexcept {
        return const_iterator(_data == nullptr ? nullptr : _data + _size);
    }

    bool empty() const noexcept { return _size == 0; }
    std::size_t size() const noexcept { return _size; }
    std::size_t capacity() const noexcept { return _capacity; }

    void reserve(std::size_t capacity) noexcept {
        if (capacity <= _capacity) return;

        T* new_data = static_cast<T*>(std::malloc(sizeof(T) * capacity));
        if (new_data == nullptr) return;

        if (_data != nullptr) {
            std::memcpy(new_data, _data, sizeof(T) * _size);
            std::free(_data);
        }

        _data = new_data;
        _capacity = capacity;
    }

    void resize(std::size_t size) noexcept {
        if (size > _capacity) reserve(size);
        if (size <= _capacity) _size = size;
    }

    void push_back(const T& value) noexcept {
        if (_size >= _capacity) {
            const std::size_t new_capacity = _capacity > 0 ? _capacity * 2 : 4;
            reserve(new_capacity);
        }

        if (_size < _capacity) {
            _data[_size] = value;
            ++_size;
        }
    }

    void pop_back() noexcept {
        if (_size > 0) --_size;
    }

    void clear() noexcept { _size = 0; }

    iterator find(const T& value) noexcept {
        for (std::size_t i = 0; i < _size; ++i) {
            if (_data[i] == value) return iterator(&_data[i]);
        }
        return end();
    }

    iterator erase(iterator pos) noexcept {
        T* ptr = pos.operator->();
        const std::size_t index = static_cast<std::size_t>(ptr - _data);
        if (index >= _size) return end();

        for (std::size_t i = index; i + 1 < _size; ++i) {
            _data[i] = _data[i + 1];
        }
        --_size;
        return iterator(&_data[index]);
    }

    iterator insert(iterator pos, const T& value) noexcept {
        T* ptr = pos.operator->();
        const std::size_t index = static_cast<std::size_t>(ptr - _data);
        if (index > _size) return end();

        if (_size >= _capacity) {
            const std::size_t new_capacity = _capacity > 0 ? _capacity * 2 : 4;
            reserve(new_capacity);
            if (_size >= _capacity) return end();
        }

        for (std::size_t i = _size; i > index; --i) {
            _data[i] = _data[i - 1];
        }
        _data[index] = value;
        ++_size;
        return iterator(&_data[index]);
    }

    T& at(std::size_t index) noexcept { return _data[index]; }
    const T& at(std::size_t index) const noexcept { return _data[index]; }
    T& operator[](std::size_t index) noexcept { return _data[index]; }
    const T& operator[](std::size_t index) const noexcept { return _data[index]; }
    T& front() noexcept { return _data[0]; }
    const T& front() const noexcept { return _data[0]; }
    T& back() noexcept { return _data[_size - 1]; }
    const T& back() const noexcept { return _data[_size - 1]; }
    T* data() noexcept { return _data; }
    const T* data() const noexcept { return _data; }
};

}  // namespace sds
