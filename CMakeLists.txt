cmake_minimum_required(VERSION 3.10)
project(memory_pool CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 可选：ASan + UBSan。需工具链自带 sanitizer 运行时（Linux/GCC、Clang）；
# MSYS2 ucrt64 的 g++ 缺少 libasan/libubsan，开启后会链接失败。
# 用法：cmake -DENABLE_SANITIZERS=ON ..
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

# 可选：ThreadSanitizer（并发数据竞争）。与 ASan 互斥，须单独构建。
# 用法：cmake -DENABLE_TSAN=ON ..
# 注：池内存来自 mmap/VirtualAlloc，绕过 ASan 分配器，故 ASan 抓不到池内越界；
#     本项目的并发正确性主要靠 TSan 验证。
option(ENABLE_TSAN "Build with ThreadSanitizer (mutually exclusive with ENABLE_SANITIZERS)" OFF)
if(ENABLE_SANITIZERS AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_SANITIZERS 与 ENABLE_TSAN 不能同时开启（ASan 与 TSan 互斥）")
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(CORE_SOURCES
  src/page_cache.cpp
  src/central_cache.cpp
  src/memory_pool.cpp
  src/operator_new.cpp
)

find_package(Threads REQUIRED)

add_executable(test_size_class tests/test_size_class.cpp)
add_executable(test_page_map   tests/test_page_map.cpp)
add_executable(test_merge      tests/test_merge.cpp ${CORE_SOURCES})
add_executable(test_alloc_api  tests/test_alloc_api.cpp ${CORE_SOURCES})
add_executable(test_release_os tests/test_release_os.cpp ${CORE_SOURCES})
add_executable(test_sanity     tests/test_sanity.cpp ${CORE_SOURCES})
add_executable(test_interleave tests/test_interleave.cpp ${CORE_SOURCES})
add_executable(test_thread     tests/test_thread.cpp ${CORE_SOURCES})
add_executable(bench           benchmark/bench.cpp ${CORE_SOURCES})
# 带对齐的 operator new 是 C++17 特性，单独用 C++17 编译验证
add_executable(test_aligned_new tests/test_aligned_new.cpp ${CORE_SOURCES})
set_target_properties(test_aligned_new PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_link_libraries(test_interleave Threads::Threads)
target_link_libraries(test_thread     Threads::Threads)
target_link_libraries(bench           Threads::Threads)

if(WIN32)
  target_link_libraries(bench psapi)
endif()

# 注册测试，供 ctest / CI 统一驱动（bench 是基准，不计入测试）
enable_testing()
set(ALL_TESTS
  test_size_class
  test_page_map
  test_merge
  test_alloc_api
  test_release_os
  test_sanity
  test_interleave
  test_thread
  test_aligned_new
)
foreach(t ${ALL_TESTS})
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# 并发测试较重（尤其 TSan 下），放宽超时
set_tests_properties(test_thread test_interleave PROPERTIES TIMEOUT 600)