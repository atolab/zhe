cmake_minimum_required(VERSION 3.9)

set(Platform mindeps-platform.c)
include_directories("." "${PROJECT_SOURCE_DIR}/src")
add_executable(mindeps-pub mindeps-pub.c ${Platform} ${ZSources})
add_executable(mindeps-sub mindeps-sub.c ${Platform} ${ZSources})
