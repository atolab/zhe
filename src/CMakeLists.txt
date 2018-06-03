cmake_minimum_required(VERSION 3.9)
project(zhe C)

include_directories(${ZIncludes})
add_library(zhe ${ZSources} ${ZPlatform})
add_library(dzhe SHARED ${ZSources} ${ZPlatform})

install(TARGETS zhe DESTINATION lib)
install(TARGETS dzhe DESTINATION lib)

file(GLOB Headers "${PROJECT_SOURCE_DIR}/src/*.h" "example/platform/*.h" "example/configs/${ZHE_CONFIG}/*.h")
install(FILES ${Headers} DESTINATION include/zhe)
