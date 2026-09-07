idf_component_register(
    SRCS "psk_rtty.c" "psk_viterbi_p4.S"
    INCLUDE_DIRS "include" "../../main"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
