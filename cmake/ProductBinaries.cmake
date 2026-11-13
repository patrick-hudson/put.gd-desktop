# The crash handler and debug splitting for every product binary.
# Each product binary gets the crash handler and, in Release builds, its debug
# information moved to <file>.debug with a .gnu_debuglink back to it. This runs
# at link time, before the install step hashes the S3 module or the AppImage
# inventories its files; tools/releases/debug_symbols.py collects the .debug
# files by GNU build ID for the release's debug asset.
foreach(product putgd putgd-ui putgd-capture putgd-region putgd-record putgd-ocr putgd-s3-worker putgd-b2-worker putgd-http-worker putgd-cli putgd_s3_module)
  if(NOT TARGET ${product})
    continue()
  endif()
  if(TARGET putgd_crash AND NOT product MATCHES "^(putgd-cli|putgd_s3_module)$")
    target_link_libraries(${product} PRIVATE putgd_crash)
  endif()
  if(putgd_split_debug)
    add_custom_command(TARGET ${product} POST_BUILD
      COMMAND "${PUTGD_OBJCOPY}" --only-keep-debug --compress-debug-sections=zlib "$<TARGET_FILE:${product}>" "$<TARGET_FILE:${product}>.debug"
      COMMAND "${PUTGD_OBJCOPY}" --strip-debug "--add-gnu-debuglink=$<TARGET_FILE:${product}>.debug" "$<TARGET_FILE:${product}>"
      VERBATIM)
  endif()
endforeach()
