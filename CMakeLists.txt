cmake_minimum_required(VERSION 3.20)
project(tbtools CXX)

# Plain C++20, no compiler-specific flags and no hardcoded toolchain paths:
# CMake picks whatever compiler the machine has (GCC here, but this must keep
# building on a Pi or a CI runner). Everything is standard library only - no
# gtest, no Eigen, no Boost - so `cmake -B build && cmake --build build` is the
# whole story: clone it, build it, run the tests. Nothing to install first.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # These are batch number-crunchers. An accidental unoptimised build is not a
  # small difference here - it is the entire reason the tools exist.
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(tb STATIC
  src/bars.cpp
  src/stats.cpp
  src/driver.cpp
)
target_include_directories(tb PUBLIC src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tb PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Link the GCC runtime statically. Without this a MinGW-built binary exits with
# code 127 and no message the moment it is run from a shell that does not have
# the compiler's own bin directory on PATH - which is every shell except the
# one that built it. These are tools meant to be run by hand and from scheduled
# tasks, so "it only works in my terminal" is not acceptable.
if(MINGW)
  # -static, not just -static-libgcc/-static-libstdc++: the thread support
  # pulls in libwinpthread-1.dll too, which is the one that actually bites.
  target_link_options(tb PUBLIC -static)
endif()

find_package(Threads REQUIRED)
target_link_libraries(tb PUBLIC Threads::Threads)

add_executable(coint_scan src/coint_scan.cpp)
target_link_libraries(coint_scan PRIVATE tb)

enable_testing()

add_executable(tb_tests
  tests/test_main.cpp
  tests/test_bars.cpp
  tests/test_stats.cpp
  tests/test_driver.cpp
)
target_link_libraries(tb_tests PRIVATE tb)
add_test(NAME tb_tests COMMAND tb_tests)
