// Copyright 2020 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#pragma once

#include <type_traits>

namespace Common {

// Check if type is like an STL container
template <typename T>
concept IsSTLContainer = requires(T t) {
    typename T::value_type;
    typename T::iterator;
    typename T::const_iterator;
    // TODO(ogniK): Replace below is std::same_as<void> when MSVC supports it.
    t.begin();
    t.end();
    t.cbegin();
    t.cend();
    t.data();
    t.size();
};

// TODO: Replace with std::derived_from when the <concepts> header
//       is available on all supported platforms.
template <typename Derived, typename Base>
concept DerivedFrom = requires {
    std::is_base_of_v<Base, Derived>;
    std::is_convertible_v<const volatile Derived*, const volatile Base*>;
};

// TODO: Replace with std::convertible_to when libc++ implements it.
template <typename From, typename To>
concept ConvertibleTo = std::is_convertible_v<From, To>;

} // namespace Common
