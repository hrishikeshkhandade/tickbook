cmake_minimum_required(VERSION 3.25)
project(tickbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TICKBOOK_BUILD_TESTS "Build the unit and randomized tests" ON)
option(TICKBOOK_BUILD_BENCH "Build the benchmark" ON)
option(TICKBOOK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(TICKBOOK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The engine. No I/O, no threads, no clock: the same library is linked by the
# tests and the benchmark, and can later be compiled to WebAssembly unchanged.
add_library(tickbook src/order_book.cpp)
target_include_directories(tickbook PUBLIC include)
target_compile_options(tickbook PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

if(TICKBOOK_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  add_executable(tickbook_tests tests/order_book_test.cpp tests/fuzz_test.cpp)
  target_link_libraries(tickbook_tests PRIVATE tickbook GTest::gtest_main)
  target_compile_options(tickbook_tests PRIVATE -Wall -Wextra -Wpedantic)
  # A hang (for example an endless matching loop) counts as a failure.
  gtest_discover_tests(tickbook_tests PROPERTIES TIMEOUT 120)
endif()

if(TICKBOOK_BUILD_BENCH)
  add_executable(tickbook_bench bench/bench.cpp)
  target_link_libraries(tickbook_bench PRIVATE tickbook)
  target_compile_options(tickbook_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

# WebAssembly build: the same engine plus a thin C interface (wasm/), compiled
# with Emscripten into one .wasm file with no JavaScript glue. Only built when
# CMake is run through emcmake, so the native build is unaffected.
if(EMSCRIPTEN)
  add_executable(tickbook_wasm wasm/tickbook_wasm.cpp)
  target_link_libraries(tickbook_wasm PRIVATE tickbook)
  target_compile_options(tickbook_wasm PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  # A .wasm suffix tells emcc to write only the module, no .js loader.
  set_target_properties(tickbook_wasm PROPERTIES OUTPUT_NAME tickbook SUFFIX ".wasm")
  target_link_options(tickbook_wasm PRIVATE
    --no-entry                  # a library of functions, there is no main()
    -sSTANDALONE_WASM=1         # do not depend on Emscripten's JS runtime
    -sALLOW_MEMORY_GROWTH=1     # the book can grow past the starting memory
    "-sEXPORTED_FUNCTIONS=_tb_reset,_tb_out,_tb_add_limit,_tb_add_market,_tb_cancel,_tb_levels,_tb_resting,_tb_take_trades,_tb_order_count")
endif()
