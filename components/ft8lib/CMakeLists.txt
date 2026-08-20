idf_component_register(
    SRCS
        "common/monitor.c"
        "fft/kiss_fft.c"
        "fft/kiss_fftr.c"
    "ft8/constants.c"
    "ft8/crc.c"
    "ft8/decode.c"
    "ft8/encode.c"
    "ft8/ldpc.c"
        "ft8/message.c"
        "ft8/text.c"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=unused-const-variable)
