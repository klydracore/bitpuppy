cmake_minimum_required(VERSION 3.16)
project(bitpuppy C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBYAML REQUIRED yaml-0.1)
pkg_check_modules(JANSSON REQUIRED jansson)

add_executable(bitpup src/main.c)

target_include_directories(bitpup PRIVATE
    ${LIBYAML_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIRS}
)

target_link_directories(bitpup PRIVATE
    ${LIBYAML_LIBRARY_DIRS}
    ${JANSSON_LIBRARY_DIRS}
)

target_link_libraries(bitpup PRIVATE
    ${LIBYAML_LIBRARIES}
    ${JANSSON_LIBRARIES}
)
