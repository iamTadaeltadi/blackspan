cmake_minimum_required(VERSION 3.16)
project(blackspan CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

option(BLACKSPAN_SANITIZE "Build with AddressSanitizer/UBSan" OFF)
if(BLACKSPAN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB_RECURSE BLACKSPAN_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(blackspan_core STATIC ${BLACKSPAN_SOURCES})
target_include_directories(blackspan_core
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(blackspan tools/blackspan_cli.cpp)
target_link_libraries(blackspan PRIVATE blackspan_core)

enable_testing()
file(GLOB BLACKSPAN_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(test_src ${BLACKSPAN_TESTS})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE blackspan_core)
  # The corpus test reads the checked-in example recordings from the source
  # tree, so it needs their location at compile time.
  target_compile_definitions(
    ${test_name} PRIVATE
    BLACKSPAN_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
