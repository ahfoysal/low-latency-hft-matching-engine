cmake_minimum_required(VERSION 3.20)
project(hft_mvp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-DNDEBUG>
)

add_library(order_book
  src/order_book.cpp
  src/engine.cpp
)
target_include_directories(order_book PUBLIC src)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE order_book)

enable_testing()
add_executable(test_book test/test_book.cpp)
target_link_libraries(test_book PRIVATE order_book)
add_test(NAME order_book_tests COMMAND test_book)
