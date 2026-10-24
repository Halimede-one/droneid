idf_component_register(SRCS "fc_link.c" INCLUDE_DIRS "." REQUIRES rid mavlink)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
