cmake_minimum_required(VERSION 3.9)

include_directories(${ZIncludes})

add_executable(throughput throughput.c)
target_link_libraries(throughput zhe)

install(TARGETS throughput DESTINATION bin)
