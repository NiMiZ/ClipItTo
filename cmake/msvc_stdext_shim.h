// Force-included (C++ only) with MSVC 14.50 and later, see CMakeLists.txt.
//
// Those compilers dropped the `stdext` checked array iterators, which Qt 6.5's headers still
// use (QT_MAKE_CHECKED_ARRAY_ITERATOR in qcompilerdetection.h). Plain pointers do the same job.
#pragma once

#include <cstddef>

namespace stdext {

template <class T>
constexpr T* make_checked_array_iterator(T* p, std::size_t) noexcept
{
    return p;
}

template <class T>
constexpr T* make_unchecked_array_iterator(T* p) noexcept
{
    return p;
}

} // namespace stdext
