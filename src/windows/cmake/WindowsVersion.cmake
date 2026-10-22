# Numeric Windows versions for a calendar version YY.MDD.N (docs/versioning.md, #396).
#
# VERSIONINFO FILEVERSION/PRODUCTVERSION: four 16-bit fields YY,MDD,N/10000,N%10000.
# Read as a tuple they order like parseReleaseVersion (date, then count); 26.1022.942
# is 26,1022,0,942 and 28.315.65536 is 28,315,6,5536.
#
# MSI ProductVersion: major.minor.build, at most 255.255.65535, and Windows Installer
# ignores a fourth field. So the year and month share the major field and the count
# keeps 19 bits:
#   major = (YY - 26) * 12 + month - 1   (0 is January 2026, 255 is April 2047)
#   minor = day * 8 + N / 65536
#   build = N % 65536
# This orders like parseReleaseVersion for N below 524,288 (about 14 years at 100
# commits a day); a larger N or a later date fails the configuration instead of
# producing an MSI that sorts below its predecessor. A nightly, a development build
# and the stable release of one YY.MDD.N map to the same MSI version.
function(putgd_windows_version version prefix)
  if(NOT version MATCHES "^([2-9][0-9])\\.([1-9][0-9][0-9][0-9]?)\\.([1-9][0-9]*)(-.*)?$")
    message(FATAL_ERROR "Not a calendar version: ${version}")
  endif()
  set(_year "${CMAKE_MATCH_1}")
  set(_date "${CMAKE_MATCH_2}")
  set(_count "${CMAKE_MATCH_3}")
  math(EXPR _month "${_date} / 100")
  math(EXPR _day "${_date} % 100")
  if(_year LESS 26 OR _month LESS 1 OR _month GREATER 12 OR _day LESS 1 OR _day GREATER 31)
    message(FATAL_ERROR "Not a calendar date: ${version}")
  endif()
  string(LENGTH "${_count}" _digits)
  if(_digits GREATER 7)
    message(FATAL_ERROR "${version} is past the MSI version mapping (see src/windows/cmake/WindowsVersion.cmake)")
  endif()
  math(EXPR _high "${_count} / 10000")
  math(EXPR _low "${_count} % 10000")
  set(${prefix}_FILE_VERSION "${_year},${_date},${_high},${_low}" PARENT_SCOPE)
  set(${prefix}_FILE_VERSION_TEXT "${_year}.${_date}.${_high}.${_low}" PARENT_SCOPE)
  math(EXPR _major "(${_year} - 26) * 12 + ${_month} - 1")
  if(_major GREATER 255 OR _count GREATER_EQUAL 524288)
    message(FATAL_ERROR "${version} is past the MSI version mapping (see src/windows/cmake/WindowsVersion.cmake)")
  endif()
  math(EXPR _minor "${_day} * 8 + ${_count} / 65536")
  math(EXPR _build "${_count} % 65536")
  set(${prefix}_MSI_VERSION "${_major}.${_minor}.${_build}" PARENT_SCOPE)
endfunction()
