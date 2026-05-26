cmake_minimum_required(VERSION 3.16)
project(lensx C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED libelf)
pkg_check_modules(ZLIB REQUIRED zlib)
add_subdirectory(libbpf)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

add_executable(lensx src/lensx.cpp src/config.cpp src/bpf_gen.cpp src/matcher.cpp)

target_include_directories(lensx PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libbpf/src
    ${LIBELF_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIR}
)

target_link_libraries(lensx PRIVATE
    bpf
    ${LIBELF_LIBRARIES}
    ${ZLIB_LIBRARIES}
    yaml-cpp
)

target_compile_options(lensx PRIVATE ${LIBELF_CFLAGS} ${ZLIB_CFLAGS})
