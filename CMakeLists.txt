cmake_minimum_required(VERSION 3.20)
project(telemetry_orb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(TELEMETRY_BUILD_TESTS "Build the GoogleTest suite" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- omniORB
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(OMNIORB IMPORTED_TARGET omniORB4 omnithread4)
endif()
if(OMNIORB_FOUND)
  add_library(omniorb_deps INTERFACE)
  target_link_libraries(omniorb_deps INTERFACE PkgConfig::OMNIORB)
else()
  find_path(OMNIORB_INCLUDE_DIR omniORB4/CORBA.h REQUIRED)
  find_library(OMNIORB_LIB omniORB4 REQUIRED)
  find_library(OMNITHREAD_LIB omnithread REQUIRED)
  add_library(omniorb_deps INTERFACE)
  target_include_directories(omniorb_deps INTERFACE ${OMNIORB_INCLUDE_DIR})
  target_link_libraries(omniorb_deps INTERFACE ${OMNIORB_LIB} ${OMNITHREAD_LIB})
endif()
target_link_libraries(omniorb_deps INTERFACE Threads::Threads)

find_program(OMNIIDL omniidl REQUIRED)
# omniidl is a Python script with a compiled extension built for the
# distribution's default Python. If another python3 comes first on PATH the
# script fails to import it, so probe once and fall back to running the
# script with the interpreter the extension was built for.
set(OMNIIDL_CMD ${OMNIIDL})
execute_process(COMMAND ${OMNIIDL} -u RESULT_VARIABLE _omniidl_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _omniidl_rc EQUAL 0)
  file(GLOB _omniidl_ext /usr/lib/omniidl/_omniidl.cpython-*.so)
  foreach(_ext IN LISTS _omniidl_ext)
    string(REGEX MATCH "cpython-([0-9])([0-9]+)" _m "${_ext}")
    find_program(_omniidl_py NAMES python${CMAKE_MATCH_1}.${CMAKE_MATCH_2})
    if(_omniidl_py)
      set(OMNIIDL_CMD ${_omniidl_py} ${OMNIIDL})
      break()
    endif()
  endforeach()
  message(STATUS "omniidl: using ${OMNIIDL_CMD}")
endif()

# ------------------------------------------------------------- IDL stubs
set(IDL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/idl/Telemetry.idl)
set(IDL_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${IDL_OUT})
add_custom_command(
  OUTPUT ${IDL_OUT}/Telemetry.hh ${IDL_OUT}/TelemetrySK.cc
  COMMAND ${OMNIIDL_CMD} -bcxx -Wbh=.hh -Wbs=SK.cc -C ${IDL_OUT} ${IDL_FILE}
  DEPENDS ${IDL_FILE}
  COMMENT "omniidl: generating C++ stubs and skeletons from Telemetry.idl"
  VERBATIM)

add_library(telemetry_idl STATIC ${IDL_OUT}/TelemetrySK.cc)
target_include_directories(telemetry_idl PUBLIC ${IDL_OUT})
target_link_libraries(telemetry_idl PUBLIC omniorb_deps)
# Generated code is not ours to lint.
target_compile_options(telemetry_idl PRIVATE -w)

set(WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

# --------------------------------------------------- core (no ORB at all)
add_library(telemetry_core STATIC
  src/core/filter.cpp
  src/core/lease.cpp
  src/core/dispatcher.cpp
  src/core/vitals_sim.cpp
  src/core/bed_table.cpp)
target_include_directories(telemetry_core PUBLIC src)
target_link_libraries(telemetry_core PUBLIC Threads::Threads)
target_compile_options(telemetry_core PRIVATE ${WARNINGS})

# ------------------------------------------------------------ ORB layer
add_library(telemetry_orb STATIC
  src/orb/convert.cpp
  src/orb/corba_sink.cpp
  src/orb/orb_util.cpp
  src/hub/hub_server.cpp
  src/station/subscriber_impl.cpp)
target_link_libraries(telemetry_orb PUBLIC telemetry_core telemetry_idl)
target_compile_options(telemetry_orb PRIVATE ${WARNINGS})

# ---------------------------------------------------------- executables
add_executable(telemetry-hub src/hub/hub_main.cpp)
add_executable(telemetry-producer src/producer/producer_main.cpp)
add_executable(telemetry-station src/station/station_main.cpp)
foreach(exe telemetry-hub telemetry-producer telemetry-station)
  target_link_libraries(${exe} PRIVATE telemetry_orb)
  target_compile_options(${exe} PRIVATE ${WARNINGS})
endforeach()

# ---------------------------------------------------------------- tests
if(TELEMETRY_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(core_tests
    tests/bounded_queue_test.cpp
    tests/filter_test.cpp
    tests/lease_test.cpp
    tests/dispatcher_test.cpp
    tests/latency_stats_test.cpp
    tests/vitals_sim_test.cpp
    tests/bed_table_test.cpp)
  target_link_libraries(core_tests PRIVATE telemetry_core GTest::gtest_main)
  target_compile_options(core_tests PRIVATE ${WARNINGS})
  gtest_discover_tests(core_tests)

  add_executable(orb_tests tests/orb_integration_test.cpp tests/corba_sink_test.cpp)
  target_link_libraries(orb_tests PRIVATE telemetry_orb GTest::gtest)
  target_compile_options(orb_tests PRIVATE ${WARNINGS})
  # One process, one ORB: the test binary has its own main() that owns it.
  add_test(NAME orb_tests COMMAND orb_tests)
  set_tests_properties(orb_tests PROPERTIES TIMEOUT 120)

  add_test(NAME demo_script
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/demo.sh --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(demo_script PROPERTIES
    ENVIRONMENT "TELEMETRY_BIN=${CMAKE_CURRENT_BINARY_DIR};DEMO_SECONDS=3"
    TIMEOUT 60)
endif()
