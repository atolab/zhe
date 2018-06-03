cmake_minimum_required(VERSION 3.9)

# because I haven't cleaned up all of the UDP/TCP configuration issues, roundtrip can currently only
# be built with platform-udp
if(NOT TCP)
  include_directories(${ZIncludes})
  add_executable(roundtrip roundtrip.c)
  target_link_libraries(roundtrip zhe)
  install(TARGETS roundtrip DESTINATION bin)
endif()
