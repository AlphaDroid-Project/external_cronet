//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// WARNING: This test was generated by generate_feature_test_macro_components.py
// and should not be edited manually.
//
// clang-format off

// <memory_resource>

// Test the feature test macros defined by <memory_resource>

/*  Constant                           Value
    __cpp_lib_memory_resource          201603L [C++17]
    __cpp_lib_polymorphic_allocator    201902L [C++20]
*/

#include <memory_resource>
#include "test_macros.h"

#if TEST_STD_VER < 14

# ifdef __cpp_lib_memory_resource
#   error "__cpp_lib_memory_resource should not be defined before c++17"
# endif

# ifdef __cpp_lib_polymorphic_allocator
#   error "__cpp_lib_polymorphic_allocator should not be defined before c++20"
# endif

#elif TEST_STD_VER == 14

# ifdef __cpp_lib_memory_resource
#   error "__cpp_lib_memory_resource should not be defined before c++17"
# endif

# ifdef __cpp_lib_polymorphic_allocator
#   error "__cpp_lib_polymorphic_allocator should not be defined before c++20"
# endif

#elif TEST_STD_VER == 17

# ifndef __cpp_lib_memory_resource
#   error "__cpp_lib_memory_resource should be defined in c++17"
# endif
# if __cpp_lib_memory_resource != 201603L
#   error "__cpp_lib_memory_resource should have the value 201603L in c++17"
# endif

# ifdef __cpp_lib_polymorphic_allocator
#   error "__cpp_lib_polymorphic_allocator should not be defined before c++20"
# endif

#elif TEST_STD_VER == 20

# ifndef __cpp_lib_memory_resource
#   error "__cpp_lib_memory_resource should be defined in c++20"
# endif
# if __cpp_lib_memory_resource != 201603L
#   error "__cpp_lib_memory_resource should have the value 201603L in c++20"
# endif

# if !defined(_LIBCPP_VERSION)
#   ifndef __cpp_lib_polymorphic_allocator
#     error "__cpp_lib_polymorphic_allocator should be defined in c++20"
#   endif
#   if __cpp_lib_polymorphic_allocator != 201902L
#     error "__cpp_lib_polymorphic_allocator should have the value 201902L in c++20"
#   endif
# else // _LIBCPP_VERSION
#   ifdef __cpp_lib_polymorphic_allocator
#     error "__cpp_lib_polymorphic_allocator should not be defined because it is unimplemented in libc++!"
#   endif
# endif

#elif TEST_STD_VER > 20

# ifndef __cpp_lib_memory_resource
#   error "__cpp_lib_memory_resource should be defined in c++2b"
# endif
# if __cpp_lib_memory_resource != 201603L
#   error "__cpp_lib_memory_resource should have the value 201603L in c++2b"
# endif

# if !defined(_LIBCPP_VERSION)
#   ifndef __cpp_lib_polymorphic_allocator
#     error "__cpp_lib_polymorphic_allocator should be defined in c++2b"
#   endif
#   if __cpp_lib_polymorphic_allocator != 201902L
#     error "__cpp_lib_polymorphic_allocator should have the value 201902L in c++2b"
#   endif
# else // _LIBCPP_VERSION
#   ifdef __cpp_lib_polymorphic_allocator
#     error "__cpp_lib_polymorphic_allocator should not be defined because it is unimplemented in libc++!"
#   endif
# endif

#endif // TEST_STD_VER > 20
