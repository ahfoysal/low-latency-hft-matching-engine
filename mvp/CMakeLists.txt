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
find_package(Threads REQUIRED)
# M5: hft::core::pin_thread_to_cpu needs pthread on Linux and Mach on macOS.
target_link_libraries(order_book PUBLIC Threads::Threads)

# M3: FIX gateway + UDP market data feed. Pulled into its own lib so the
# bench target stays lean (no sockets, no threads).
add_library(fix_gateway
  src/fix/parser.cpp
  src/fix/gateway.cpp
  src/md/feed.cpp
)
target_include_directories(fix_gateway PUBLIC src)
target_link_libraries(fix_gateway PUBLIC order_book Threads::Threads)

# M5: io_uring gateway. Linux-only native path; POSIX fallback compiles
# everywhere so macOS / CI stay green. Toggle real path with -DHFT_IOURING=ON.
option(HFT_IOURING "Enable liburing-backed FIX gateway (Linux only)" OFF)
add_library(uring_gateway src/iouring/uring_gateway.cpp)
target_include_directories(uring_gateway PUBLIC src)
target_link_libraries(uring_gateway PUBLIC order_book fix_gateway)
if(HFT_IOURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(uring_gateway PUBLIC HFT_IOURING)
  target_link_libraries(uring_gateway PUBLIC uring)
  message(STATUS "M5: io_uring path ENABLED (liburing)")
else()
  message(STATUS "M5: io_uring path disabled (POSIX fallback in use)")
endif()

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE order_book)

enable_testing()
add_executable(test_book test/test_book.cpp)
target_link_libraries(test_book PRIVATE order_book)
add_test(NAME order_book_tests COMMAND test_book)

add_executable(test_fix test/test_fix.cpp)
target_link_libraries(test_fix PRIVATE fix_gateway)
add_test(NAME fix_tests COMMAND test_fix)

# M4: backtester + strategy framework. Pulled into its own lib so bench + test
# + main driver all share the same object files.
add_library(backtest
  src/backtest/portfolio.cpp
  src/backtest/strategy.cpp
  src/backtest/replay.cpp
  strategies/mm.cpp
  strategies/ma_cross.cpp
)
target_include_directories(backtest PUBLIC src . strategies)
target_link_libraries(backtest PUBLIC order_book)

add_executable(gen_ticks bench/gen_ticks.cpp)
target_link_libraries(gen_ticks PRIVATE backtest)

add_executable(backtest_main bench/backtest_main.cpp)
target_link_libraries(backtest_main PRIVATE backtest)

add_executable(test_backtest test/test_backtest.cpp)
target_link_libraries(test_backtest PRIVATE backtest)
add_test(NAME backtest_tests COMMAND test_backtest)

# M6: PTP client + DPDK scaffold + FPGA stub + kernel-driver trait.
option(HFT_DPDK "Enable librte_eal-backed DPDK driver (Linux only)" OFF)
option(HFT_FPGA "Enable sample FPGA driver (requires /dev node)"    OFF)
option(HFT_PTP  "Enable Linux PHC /dev/ptp0 PTP backend"            OFF)

add_library(m6_runtime
  src/core/software_driver.cpp
  src/dpdk/dpdk_driver.cpp
  src/fpga/fpga_driver.cpp
  src/ptp/ptp_client.cpp
)
target_include_directories(m6_runtime PUBLIC src)
target_link_libraries(m6_runtime PUBLIC order_book fix_gateway uring_gateway Threads::Threads)

if(HFT_DPDK AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(m6_runtime PUBLIC HFT_DPDK)
  target_link_libraries(m6_runtime PUBLIC rte_eal rte_ethdev rte_mbuf rte_mempool)
  message(STATUS "M6: DPDK driver ENABLED (librte_eal)")
else()
  message(STATUS "M6: DPDK driver disabled (stub in use)")
endif()

if(HFT_FPGA)
  target_compile_definitions(m6_runtime PUBLIC HFT_FPGA)
  message(STATUS "M6: FPGA driver ENABLED (mmio path)")
else()
  message(STATUS "M6: FPGA driver disabled (stub in use)")
endif()

if(HFT_PTP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(m6_runtime PUBLIC HFT_PTP)
  message(STATUS "M6: PTP Linux PHC backend ENABLED")
else()
  message(STATUS "M6: PTP using mock-oracle backend (Linux PHC disabled)")
endif()

add_executable(ptp_oracle src/ptp/ptp_oracle.cpp)

add_executable(test_m6 test/test_m6.cpp)
target_link_libraries(test_m6 PRIVATE m6_runtime)
add_test(NAME m6_tests COMMAND test_m6)
