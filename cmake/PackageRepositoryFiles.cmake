# The pkg.put.gd repository files of both channels for one package label (#293,
# #386). A tagged package installs its own channel's file as the live one and
# both under /usr/share/putgd/repo, so moving to the other channel is copying
# that file into place by hand. Both come from this one function, so a copy is
# byte for byte what the other channel's package installs and dpkg takes that
# package without a conffile prompt.
#
# Script mode, for tests:
#   cmake -DLABEL=debian13 -DREPOSITORY=https://pkg.put.gd -DREPO_DIR=packaging/linux/repo -DOUTPUT=out -P cmake/PackageRepositoryFiles.cmake
function(putgd_repository_files label repository repo_dir output_dir)
  set(PUTGD_PACKAGE_TARGET "${label}")
  set(PUTGD_REPO_URL "${repository}")
  foreach(PUTGD_REPO_CHANNEL nightly stable)
    if(label MATCHES "^(ubuntu24[.]04|ubuntu26[.]04|debian13)$")
      # deb822 multi-line Signed-By: the key travels inside the file, so a
      # removed (not purged) package leaves a source apt can still verify.
      file(READ "${repo_dir}/keys/putgd-${PUTGD_REPO_CHANNEL}.asc" key_block)
      string(STRIP "${key_block}" key_block)
      string(REPLACE "\n\n" "\n.\n" key_block "${key_block}")
      string(REPLACE "\n" "\n " key_block " ${key_block}")
      set(PUTGD_REPO_KEY_BLOCK "${key_block}")
      configure_file("${repo_dir}/putgd.sources.in" "${output_dir}/putgd-${PUTGD_REPO_CHANNEL}.sources" @ONLY)
    elseif(label STREQUAL "fedora44")
      configure_file("${repo_dir}/putgd.repo.in" "${output_dir}/putgd-${PUTGD_REPO_CHANNEL}.repo" @ONLY)
    else()
      message(FATAL_ERROR "No package repository for ${label}")
    endif()
  endforeach()
endfunction()
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  putgd_repository_files("${LABEL}" "${REPOSITORY}" "${REPO_DIR}" "${OUTPUT}")
endif()
