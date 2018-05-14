cmake_minimum_required(VERSION 3.9)
project(zhe C)

#set(TCP ON)
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

include_directories(
  "${PROJECT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/example/platform"
  "${PROJECT_SOURCE_DIR}/example/configs/${ZHE_CONFIG}")

file(GLOB Headers "src/*.h" "example/platform/*.h" "example/configs/${ZHE_CONFIG}/*.h")

if(TCP)

  add_definitions(-DTCP)
  file(GLOB Sources "src/*.c" "example/platform/zhe-*.c" "example/platform/platform-tcp.c")

  add_library(zhe ${Sources})
  
  add_executable(throughput example/throughput/throughput.c)
  target_link_libraries(throughput zhe)

  add_executable(spub example/simple/pub.c)
  target_link_libraries(spub zhe)

  add_executable(ssub example/simple/sub.c)
  target_link_libraries(ssub zhe)

else()

  file(GLOB Sources "src/*.c" "example/platform/zhe-*.c" "example/platform/platform-udp.c")

  add_library(zhe ${Sources})
  add_library(dzhe SHARED ${Sources})

  add_executable(throughput example/throughput/throughput.c)
  target_link_libraries(throughput zhe)

  add_executable(roundtrip example/roundtrip/roundtrip.c)
  target_link_libraries(roundtrip zhe)

  add_executable(spub example/simple/pub.c)
  target_link_libraries(spub zhe)

  add_executable(ssub example/simple/sub.c)
  target_link_libraries(ssub zhe)

  add_executable(psrid test/psrid.c)
  target_link_libraries(psrid zhe)

  install(TARGETS zhe DESTINATION ${PROJECT_SOURCE_DIR}/lib)
  install(TARGETS throughput DESTINATION ${PROJECT_SOURCE_DIR}/bin)
  install(TARGETS roundtrip DESTINATION ${PROJECT_SOURCE_DIR}/bin)
  install(TARGETS spub DESTINATION ${PROJECT_SOURCE_DIR}/bin)
  install(TARGETS ssub DESTINATION ${PROJECT_SOURCE_DIR}/bin)
  install(TARGETS psrid DESTINATION ${PROJECT_SOURCE_DIR}/bin)
  install(TARGETS dzhe DESTINATION lib)
  install(FILES ${Headers} DESTINATION include/zhe)

endif()
