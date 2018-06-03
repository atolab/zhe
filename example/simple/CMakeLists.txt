cmake_minimum_required(VERSION 3.9)

include_directories(${ZIncludes})

add_executable(spub pub.c)
target_link_libraries(spub zhe)

add_executable(ssub sub.c)
target_link_libraries(ssub zhe)

install(TARGETS spub DESTINATION bin)
install(TARGETS ssub DESTINATION bin)
