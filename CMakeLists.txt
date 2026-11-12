cmake_minimum_required(VERSION 3.20)
project(cpp20_lockfree_mpmc_latency_lab VERSION 1.0.0 LANGUAGES CXX)

# Enforce modern C++20 standard without vendor extensions
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict industrial compiler warning gates
add_compile_options(-Wall -Wextra -Werror -pedantic)

# Sanitizer & Optimization switches
option(ENABLE_TSAN "Enable ThreadSanitizer for data race detection" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer & UBSan for memory safety" OFF)

if(ENABLE_TSAN)
    add_compile_options(-O2 -g -fsanitize=thread)
    add_link_options(-fsanitize=thread)
elseif(ENABLE_ASAN)
    add_compile_options(-O2 -g -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
else()
    add_compile_options(-O3)
endif()

find_package(Threads REQUIRED)

# Header-only library target
add_library(latency_lab INTERFACE)
target_include_directories(latency_lab INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(latency_lab INTERFACE Threads::Threads)

# 16M-operation concurrency & latency stress test executable
add_executable(stress_test tests/stress_test.cpp)
target_link_libraries(stress_test PRIVATE latency_lab)
