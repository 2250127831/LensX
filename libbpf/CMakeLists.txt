set(LIBBPF_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(BPF_A ${CMAKE_CURRENT_BINARY_DIR}/obj/libbpf.a)

add_custom_command(
    OUTPUT ${BPF_A}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${LIBBPF_DIR}/src
            BUILD_STATIC_ONLY=1
            OBJDIR=${CMAKE_CURRENT_BINARY_DIR}/obj
    COMMENT "Building libbpf 1.0 via Makefile"
)

add_custom_target(bpf_static DEPENDS ${BPF_A})
add_library(bpf STATIC IMPORTED GLOBAL)
set_target_properties(bpf PROPERTIES IMPORTED_LOCATION ${BPF_A})
add_dependencies(bpf bpf_static)
target_include_directories(bpf INTERFACE
    ${LIBBPF_DIR}/src
    ${LIBBPF_DIR}/include
    ${LIBBPF_DIR}/include/uapi)
