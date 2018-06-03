cmake_minimum_required(VERSION 3.9)

include_directories("." "${PROJECT_SOURCE_DIR}/src")
add_executable(zbotmon zbotmon.c ${ZSources})
