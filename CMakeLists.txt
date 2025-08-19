cmake_minimum_required(VERSION 3.16)
project(bitpuppy C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBYAML REQUIRED yaml-0.1)
pkg_check_modules(JSONC REQUIRED json-c)

# Source files
add_executable(bitpup src/main.c)

# Include directories
target_include_directories(bitpup PRIVATE
    ${LIBYAML_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(bitpup PRIVATE
    ${LIBYAML_LIBRARIES}
    ${JSONC_LIBRARIES}
)
