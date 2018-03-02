cmake_minimum_required(VERSION 3.9)
project(zhe C)

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

include_directories(
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/example/platform"
        "${PROJECT_SOURCE_DIR}/example/configs/p2p"
)

file(GLOB Headers "src/*.h" "example/platform/*.h" "example/configs/p2p/*.h")
file(GLOB Sources "src/*.c" "example/platform/*.c")

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

install (TARGETS zhe DESTINATION ${PROJECT_SOURCE_DIR}/lib)
install (TARGETS throughput DESTINATION ${PROJECT_SOURCE_DIR}/bin)
install (TARGETS roundtrip DESTINATION ${PROJECT_SOURCE_DIR}/bin)
install (TARGETS spub DESTINATION ${PROJECT_SOURCE_DIR}/bin)
install (TARGETS ssub DESTINATION ${PROJECT_SOURCE_DIR}/bin)
install (TARGETS psrid DESTINATION ${PROJECT_SOURCE_DIR}/bin)
install (TARGETS dzhe DESTINATION lib)
install(FILES ${Headers} DESTINATION include/zhe)
