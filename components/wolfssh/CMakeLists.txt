set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/wolfssh")
idf_component_register(SRCS "${ROOT}/src/ssh.c" "${ROOT}/src/internal.c"
    "${ROOT}/src/io.c" "${ROOT}/src/log.c" "${ROOT}/src/port.c"
    "${ROOT}/src/keygen.c" "${ROOT}/src/ossh.c"
    INCLUDE_DIRS "${ROOT}" REQUIRES wolfcrypt lwip)
target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
