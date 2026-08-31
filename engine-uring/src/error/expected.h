#pragma once

#include <tl/expected.hpp>

template <class T, class E>
using expected = tl::expected<T, E>;

template <class E>
using unexpected = tl::unexpected<E>;
