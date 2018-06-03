cmake_minimum_required(VERSION 3.9)
project(zhe C)

#cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DTCP=OFF -DCMAKE_INSTALL_PREFIX=.. ..
#cmake -GNinja -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl -DCMAKE_BUILD_TYPE=Debug -DTCP=ON -DSSL=ON -DZHE_CONFIG=client-nbiot ..

#set(TCP ON)
#set(SSL ON)
#set(ZHE_CONFIG client-nbiot)
if(NOT ZHE_CONFIG)
  set(ZHE_CONFIG p2p)
endif()

set(CMAKE_C_STANDARD 99)
include(CheckIPOSupported)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  check_ipo_supported(RESULT supported OUTPUT error)
  if(supported)
    #message(STATUS "IPO / LTO enabled")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
  
add_compile_options(
  $<$<C_COMPILER_ID:MSVC>:/W3>
  $<$<C_COMPILER_ID:Clang>:-Wall>
  $<$<C_COMPILER_ID:AppleClang>:-Wall>
  $<$<C_COMPILER_ID:GNU>:-Wall>)
add_compile_options(
  $<$<C_COMPILER_ID:Clang>:-pedantic>
  $<$<C_COMPILER_ID:AppleClang>:-pedantic>
  $<$<C_COMPILER_ID:GNU>:-pedantic>)

# Clang doesn't colour its output by default when invoked by Ninja
# Can't commit this as it would break when ninja itself is not sending its output to a TTY
if(${CMAKE_GENERATOR} STREQUAL "Ninja" AND (${CMAKE_C_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_C_COMPILER_ID} STREQUAL "AppleClang"))
  add_compile_options(-Xclang -fcolor-diagnostics)
endif()

set(ZIncludes
  "${PROJECT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/example/platform"
  "${PROJECT_SOURCE_DIR}/example/configs/${ZHE_CONFIG}")

file(GLOB ZSources "src/*.c")

if(TCP)
  add_definitions(-DTCP)
  file(GLOB ZPlatform "example/platform/zhe-*.c" "example/platform/platform-tcp.c")
  if(SSL)
    find_package(OpenSSL REQUIRED)
    add_definitions(-DUSE_SSL=1)
    include_directories(${OPENSSL_INCLUDE_DIR})
    link_libraries(${OPENSSL_LIBRARIES})
    message(STATUS "Using OpenSSL ${OPENSSL_VERSION} at ${OPENSSL_INCLUDE_DIR}")
  endif()
else()
  file(GLOB ZPlatform "example/platform/zhe-*.c" "example/platform/platform-udp.c")
endif()

add_subdirectory(src)
add_subdirectory(example)
