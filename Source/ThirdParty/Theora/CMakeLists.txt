set (TARGET_NAME Theora)

set(SOURCE_FILES
	lib/analyze.c
    lib/apiwrapper.c
    lib/bitpack.c
    lib/cpu.c
    lib/decapiwrapper.c
    lib/decinfo.c
    lib/decode.c
    lib/dequant.c
    lib/encapiwrapper.c
    lib/encfrag.c
    lib/encinfo.c
    lib/encode.c
    lib/encoder_disabled.c
    lib/enquant.c
    lib/fdct.c
    lib/fragment.c
    lib/huffdec.c
    lib/huffenc.c
    lib/idct.c
    lib/info.c
    lib/internal.c
    lib/mathops.c
    lib/mcenc.c
    lib/quant.c
    lib/rate.c
    lib/state.c
    lib/tokenize.c
)

# Setup target
list (APPEND INCLUDE_DIRS include ../Ogg/include)
setup_library ()

install_header_files (DIRECTORY include/ DESTINATION ${DEST_INCLUDE_DIR}/ThirdParty/Theora FILES_MATCHING PATTERN *.h BUILD_TREE_ONLY)

