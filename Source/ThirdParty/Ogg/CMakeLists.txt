set (TARGET_NAME Ogg)

set(SOURCE_FILES src/bitwise.c src/framing.c)

# Setup target
list (APPEND INCLUDE_DIRS include)
setup_library ()

install_header_files (DIRECTORY include/ DESTINATION ${DEST_INCLUDE_DIR}/ThirdParty/Ogg FILES_MATCHING PATTERN *.h BUILD_TREE_ONLY)
