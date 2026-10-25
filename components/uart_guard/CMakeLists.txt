idf_component_register(SRCS "uart_guard.c" "guard_selftest.c" INCLUDE_DIRS "." REQUIRES mavlink)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
