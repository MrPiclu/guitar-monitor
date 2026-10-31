cmake_minimum_required(VERSION 3.22.1)
project("guitarmonitor")

find_package(oboe REQUIRED CONFIG)

add_library(guitarmonitor SHARED engine.cpp)
target_link_libraries(guitarmonitor oboe::oboe log android)