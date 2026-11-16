cmake_minimum_required(VERSION 3.20)
project(Apex VERSION 0.5.0 LANGUAGES CXX)

option(APEX_ENABLE_TESTS "Build GoogleTest tests" ON)
option(APEX_ENABLE_BENCHMARKS "Build the Release throughput/latency harness" OFF)
option(APEX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(APEX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(APEX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(APEX_ENABLE_ASAN AND APEX_ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
endif()

add_library(apex_options INTERFACE)
target_compile_features(apex_options INTERFACE cxx_std_20)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(apex_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  foreach(sanitizer IN ITEMS ASAN UBSAN TSAN)
    if(APEX_ENABLE_${sanitizer})
      if(sanitizer STREQUAL "ASAN")
        set(sanitizer_name address)
      elseif(sanitizer STREQUAL "UBSAN")
        set(sanitizer_name undefined)
      else()
        set(sanitizer_name thread)
      endif()
      target_compile_options(apex_options INTERFACE
        -fsanitize=${sanitizer_name} -fno-omit-frame-pointer)
      target_link_options(apex_options INTERFACE -fsanitize=${sanitizer_name})
    endif()
  endforeach()
elseif(APEX_ENABLE_ASAN OR APEX_ENABLE_UBSAN OR APEX_ENABLE_TSAN)
  message(FATAL_ERROR "Sanitizer configuration currently supports GCC and Clang")
endif()

add_library(apex_core src/core/version.cpp src/book/order_book.cpp
  src/matching/matching_engine.cpp src/risk/risk_manager.cpp
  src/journal/codec.cpp src/journal/journal.cpp src/replay/replay.cpp
  src/simulation/generator.cpp src/simulation/workload_io.cpp src/simulation/runner.cpp)
target_include_directories(apex_core PUBLIC include)
target_link_libraries(apex_core PUBLIC apex_options)

find_package(Threads REQUIRED)
add_library(apex_network src/network/protocol.cpp src/network/client.cpp src/network/gateway.cpp)
target_link_libraries(apex_network PUBLIC apex_core Threads::Threads)
add_executable(apex_gateway src/app/gateway_main.cpp)
target_link_libraries(apex_gateway PRIVATE apex_network)
add_executable(apex_client src/app/client_main.cpp)
target_link_libraries(apex_client PRIVATE apex_network)

add_executable(apex src/app/main.cpp src/app/demo.cpp src/app/journal_demo.cpp src/app/workload_cli.cpp)
target_link_libraries(apex PRIVATE apex_core)

if(APEX_ENABLE_TESTS OR APEX_ENABLE_BENCHMARKS)
  include(benchmarks/targets.cmake)
endif()

if(APEX_ENABLE_TESTS)
  include(CTest)
  enable_testing()
  find_package(GTest 1.17 CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    # Pinned release; downloaded only if no suitable installation is available.
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.17.0
      GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(apex_tests tests/unit/smoke_test.cpp tests/unit/domain_test.cpp
    tests/unit/order_book_test.cpp tests/unit/matching_engine_test.cpp
    tests/unit/risk_test.cpp tests/integration/journal_test.cpp tests/unit/workload_test.cpp
    tests/unit/benchmark_test.cpp tests/unit/optimization_test.cpp tests/unit/protocol_test.cpp
    tests/integration/network_test.cpp tests/unit/bounded_queue_test.cpp tests/unit/differential_test.cpp)
  target_link_libraries(apex_tests PRIVATE apex_metrics apex_network GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(apex_tests NO_PRETTY_VALUES)
  add_test(NAME Apex.CliSmoke COMMAND apex)
  set_tests_properties(Apex.CliSmoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Apex 0.5.0 - deterministic exchange simulator")
  add_test(NAME Apex.BookDemo COMMAND apex --book-demo)
  set_tests_properties(Apex.BookDemo PROPERTIES
    PASS_REGULAR_EXPRESSION "Cancelled order 1; order 2 remains at the bid.")
  add_test(NAME Apex.TradingDemo COMMAND apex --trading-demo)
  set_tests_properties(Apex.TradingDemo PROPERTIES
    PASS_REGULAR_EXPRESSION "Trading demo complete; book invariants hold.")
  add_test(NAME Apex.RiskDemo COMMAND apex --risk-demo)
  set_tests_properties(Apex.RiskDemo PROPERTIES
    PASS_REGULAR_EXPRESSION "Risk demo complete; rejection checks and book invariants hold.")
  add_test(NAME Apex.JournalDemo COMMAND apex --journal-demo)
  set_tests_properties(Apex.JournalDemo PROPERTIES
    PASS_REGULAR_EXPRESSION "Replay verified twice; events and final book match.")
  add_test(NAME Apex.WorkloadDemo COMMAND apex --workload-demo)
  set_tests_properties(Apex.WorkloadDemo PROPERTIES
    PASS_REGULAR_EXPRESSION "Workload verified: same seed, saved inputs, events and final books match.")
  add_test(NAME Apex.SavedWorkload COMMAND apex --run-workload
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/workloads/large-partial.workload")
  set_tests_properties(Apex.SavedWorkload PROPERTIES PASS_REGULAR_EXPRESSION "trades=16 executed_quantity=16")
  add_test(NAME Apex.PreoptimizationJournal COMMAND apex --replay
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/preoptimization-balanced.apex")
  set_tests_properties(Apex.PreoptimizationJournal PROPERTIES PASS_REGULAR_EXPRESSION "commands=100 events=272")
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME Apex.PortfolioEvidence COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_portfolio_evidence.py")
    set_tests_properties(Apex.PortfolioEvidence PROPERTIES
      PASS_REGULAR_EXPRESSION "Portfolio evidence verified: 75 recorded bundles")
    add_test(NAME Apex.NetworkDemo COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/network_demo.py" --build-dir "$<TARGET_FILE_DIR:apex_gateway>")
    set_tests_properties(Apex.NetworkDemo PROPERTIES TIMEOUT 30
      PASS_REGULAR_EXPRESSION "Network demo verified: 7 commands, 3 trades, continuous feed and journal replay.")
  endif()
endif()
