cmake_minimum_required(VERSION 3.0.0)
project(fat16 C)
set(CMAKE_C_STANDARD 11)

add_executable(fat16 main.c file_reader.c)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
