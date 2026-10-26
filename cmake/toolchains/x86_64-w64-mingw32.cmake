# Cross toolchain for the native Windows build (src/windows) from a Linux host.
# Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix mingw-w64-tools
# Fedora:        dnf install mingw64-gcc-c++ mingw64-winpthreads-static
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_putgd_triplet x86_64-w64-mingw32)
foreach(_tool C_COMPILER:gcc CXX_COMPILER:g++ RC_COMPILER:windres)
  string(REPLACE ":" ";" _pair "${_tool}")
  list(GET _pair 0 _var)
  list(GET _pair 1 _name)
  # Debian names the POSIX-thread flavour *-posix; plain names exist on Fedora.
  find_program(_putgd_${_var} NAMES ${_putgd_triplet}-${_name}-posix ${_putgd_triplet}-${_name})
  if(NOT _putgd_${_var})
    message(FATAL_ERROR "${_putgd_triplet}-${_name} not found; install the mingw-w64 toolchain")
  endif()
  set(CMAKE_${_var} "${_putgd_${_var}}")
endforeach()
set(CMAKE_FIND_ROOT_PATH /usr/${_putgd_triplet} /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
