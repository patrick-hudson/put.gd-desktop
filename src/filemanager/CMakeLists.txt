# "Upload with put.gd" menu plugins for Nautilus, Caja and Peony (#1105). The
# file manager loads them into its own process, so none links the file
# manager's library, Qt or putgd_build_options (its sanitizer flags would need
# ASan there): they compile with the pkg-config cflags only, every symbol
# resolves from the file manager, and the packages depend on no file manager.
# PUTGD_<NAME>_PLUGIN is AUTO (build if pkg-config finds the headers), ON
# (required, as package builds set it) or OFF. Each putgd-<fm>-check loads its
# plugin the way the file manager does; tools/releases/audit_file_manager_plugins.py
# runs them against the packaged plugins.
find_package(PkgConfig REQUIRED)
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

macro(putgd_fm_find name)
  set(putgd_fm_${name} OFF)
  if(PUTGD_${name}_PLUGIN STREQUAL "AUTO")
    pkg_check_modules(PUTGD_FM_${name} QUIET IMPORTED_TARGET ${ARGN})
    set(putgd_fm_${name} ${PUTGD_FM_${name}_FOUND})
  elseif(PUTGD_${name}_PLUGIN)
    pkg_check_modules(PUTGD_FM_${name} REQUIRED IMPORTED_TARGET ${ARGN})
    set(putgd_fm_${name} ON)
  endif()
endmacro()

# A directory a .pc variable names (plus a suffix), relative to the install
# prefix when it lies under that .pc's prefix, so dev installs stay in their prefix.
function(putgd_fm_install_dir target module variable)
  pkg_get_variable(dir ${module} ${variable})
  pkg_get_variable(prefix ${module} prefix)
  if(NOT dir)
    message(FATAL_ERROR "${module}.pc has no ${variable} for the ${target} install directory")
  endif()
  string(FIND "${dir}/" "${prefix}/" under)
  if(prefix AND under EQUAL 0)
    file(RELATIVE_PATH dir "${prefix}" "${dir}")
  endif()
  # packaging/linux installs each plugin here.
  set_target_properties(${target} PROPERTIES PUTGD_INSTALL_DIR "${dir}${ARGN}")
endfunction()

# Nautilus (extension API 4) and Caja (extensions-2.0) share menu-provider.c.
foreach(fm IN ITEMS NAUTILUS:libnautilus-extension-4 CAJA:libcaja-extension)
  string(REPLACE ":" ";" fm "${fm}")
  list(GET fm 0 name)
  list(GET fm 1 module)
  putgd_fm_find(${name} ${module})
  if(NOT putgd_fm_${name})
    continue()
  endif()
  string(TOLOWER "putgd-${name}" target)
  add_library(${target} MODULE menu-provider.c)
  target_compile_definitions(${target} PRIVATE PUTGD_FM_${name})
  target_include_directories(${target} SYSTEM PRIVATE ${PUTGD_FM_${name}_INCLUDE_DIRS})
  target_compile_options(${target} PRIVATE ${PUTGD_FM_${name}_CFLAGS_OTHER} -Wall -Wextra)
  set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden)
  putgd_fm_install_dir(${target} ${module} extensiondir)
  add_executable(${target}-check module-check.c)
  target_compile_definitions(${target}-check PRIVATE PUTGD_FM_${name})
  target_compile_options(${target}-check PRIVATE -Wall -Wextra)
  target_link_libraries(${target}-check PRIVATE PkgConfig::PUTGD_FM_${name} ${CMAKE_DL_LIBS})
endforeach()

# Peony: Qt 6 for Peony 4.20, Qt 5 for 4.0, whichever Qt its libpeony-dev uses.
putgd_fm_find(PEONY peony)
if(putgd_fm_PEONY)
  execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" --print-requires peony OUTPUT_VARIABLE peony_requires)
  if(peony_requires MATCHES "Qt([56])Widgets")
    set(peony_qt ${CMAKE_MATCH_1})
  else()
    message(FATAL_ERROR "peony.pc requires neither Qt5Widgets nor Qt6Widgets")
  endif()
  if(peony_qt EQUAL 5)
    find_package(Qt5Core CONFIG REQUIRED)
  endif()
  pkg_check_modules(PUTGD_FM_PEONY_QT REQUIRED IMPORTED_TARGET Qt${peony_qt}Widgets)
  # moc must match the headers, so a Qt 6 build needs the distro Qt Peony uses (not, say, an ASan Qt).
  if(peony_qt EQUAL 6 AND NOT Qt6Core_VERSION VERSION_EQUAL PUTGD_FM_PEONY_QT_VERSION)
    if(PUTGD_PEONY_PLUGIN STREQUAL "AUTO")
      message(STATUS "Peony plugin skipped: Qt ${Qt6Core_VERSION} is not Peony's Qt ${PUTGD_FM_PEONY_QT_VERSION}")
      set(putgd_fm_PEONY OFF)
    else()
      message(FATAL_ERROR "The Peony plugin needs Peony's Qt ${PUTGD_FM_PEONY_QT_VERSION}, not Qt ${Qt6Core_VERSION}")
    endif()
  endif()
endif()
if(putgd_fm_PEONY)
  # Explicit moc of the distro Qt (no AUTOMOC: the target doesn't link Qt).
  list(TRANSFORM PUTGD_FM_PEONY_INCLUDE_DIRS PREPEND "-I" OUTPUT_VARIABLE peony_moc_includes)
  add_custom_command(OUTPUT peony-menu.moc
    COMMAND Qt${peony_qt}::moc ${peony_moc_includes} -o "${CMAKE_CURRENT_BINARY_DIR}/peony-menu.moc" "${CMAKE_CURRENT_SOURCE_DIR}/peony-menu.cpp"
    DEPENDS peony-menu.cpp putgd-peony.json Qt${peony_qt}::moc VERBATIM)
  add_library(putgd-peony MODULE peony-menu.cpp peony-menu.moc)
  target_include_directories(putgd-peony PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(putgd-peony SYSTEM PRIVATE ${PUTGD_FM_PEONY_INCLUDE_DIRS})
  target_compile_definitions(putgd-peony PRIVATE QT_NO_VERSION_TAGGING $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG>)
  target_compile_options(putgd-peony PRIVATE ${PUTGD_FM_PEONY_CFLAGS_OTHER} -Wall -Wextra)
  putgd_fm_install_dir(putgd-peony Qt${peony_qt}Core libdir /peony-extensions)
  add_executable(putgd-peony-check peony-check.cpp)
  target_include_directories(putgd-peony-check SYSTEM PRIVATE ${PUTGD_FM_PEONY_INCLUDEDIR})
  target_link_libraries(putgd-peony-check PRIVATE PkgConfig::PUTGD_FM_PEONY_QT)
  # Qt 5 is built with -reduce-relocations, which needs -fPIC, not the executable's -fPIE.
  target_compile_options(putgd-peony-check PRIVATE -fPIC)
  # Qt 5.15 headers predate C++20.
  set_target_properties(putgd-peony putgd-peony-check PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
endif()

# KIO on KF6 (#2136): "Upload encrypted with put.gd" as a KFileItemAction plugin,
# which hides while the resident's marker exists; Upload with put.gd stays in
# the KIO service menu, installed then without its encrypted action
# (packaging/linux). Its name, icon and Exec are read from that service menu
# here, so the two never disagree. Unlike the other plugins it links KF6 KIO and
# the distro Qt, as any Dolphin plugin does: Plasma's desktop loads KIO through
# a QML plugin with local symbols, where an unlinked plugin couldn't resolve it.
# The packages still don't depend on KDE: RPM excludes the KF6 sonames, and the
# DEB's dpkg-shlibdeps reads kio-shlibs.local, which names them with no dependency.
set(putgd_fm_KIO OFF)
if(PUTGD_KIO_PLUGIN)
  if(PUTGD_KIO_PLUGIN STREQUAL "AUTO")
    find_package(KF6KIO CONFIG QUIET)
  else()
    find_package(KF6KIO CONFIG REQUIRED)
  endif()
  if(KF6KIO_FOUND)
    # moc and the headers must be the Qt Dolphin runs on, as for Peony.
    pkg_check_modules(PUTGD_FM_KIO_QT QUIET Qt6Core)
    if(NOT PUTGD_FM_KIO_QT_FOUND OR NOT Qt6Core_VERSION VERSION_EQUAL PUTGD_FM_KIO_QT_VERSION)
      if(PUTGD_KIO_PLUGIN STREQUAL "AUTO")
        message(STATUS "KIO plugin skipped: Qt ${Qt6Core_VERSION} is not the distro's Qt ${PUTGD_FM_KIO_QT_VERSION}")
      else()
        message(FATAL_ERROR "The KIO plugin needs the distro's Qt ${PUTGD_FM_KIO_QT_VERSION}, not Qt ${Qt6Core_VERSION}")
      endif()
    else()
      set(putgd_fm_KIO ON)
    endif()
  endif()
endif()
if(putgd_fm_KIO)
  set(kio_menu_source "${PROJECT_SOURCE_DIR}/packaging/linux/filemanager/kio-servicemenu.desktop")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kio_menu_source}")
  file(READ "${kio_menu_source}" kio_menu)
  string(REGEX MATCH "\n\\[Desktop Action putgdUploadEncrypted\\]\n[^[]*" kio_encrypted "${kio_menu}")
  if(NOT kio_encrypted)
    message(FATAL_ERROR "kio-servicemenu.desktop has no [Desktop Action putgdUploadEncrypted]")
  endif()
  set(kio_entry_header "// Generated from packaging/linux/filemanager/kio-servicemenu.desktop (#2136).\n#pragma once\n")
  foreach(key Name Icon Exec)
    string(REGEX MATCH "\n${key}=([^\n]*)" kio_line "${kio_encrypted}")
    set(kio_value "${CMAKE_MATCH_1}")
    if(kio_value STREQUAL "" OR kio_value MATCHES "[\"\\\\]")
      message(FATAL_ERROR "kio-servicemenu.desktop putgdUploadEncrypted ${key} is missing or needs escaping")
    endif()
    string(TOUPPER "${key}" kio_key)
    string(APPEND kio_entry_header "#define PUTGD_KIO_ENCRYPTED_${kio_key} \"${kio_value}\"\n")
  endforeach()
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/kio-entry.h" CONTENT "${kio_entry_header}" @ONLY)
  # The service menu this build installs: Upload with put.gd only.
  string(REPLACE "${kio_encrypted}" "" kio_plain "${kio_menu}")
  string(REPLACE "Actions=putgdUpload;putgdUploadEncrypted;" "Actions=putgdUpload;" kio_plain "${kio_plain}")
  if(kio_plain MATCHES "putgdUploadEncrypted")
    message(FATAL_ERROR "kio-servicemenu.desktop names putgdUploadEncrypted outside its Actions line and group")
  endif()
  string(REPLACE "[Desktop Entry]" "# Here the putgd KFileItemAction plugin offers Upload encrypted with put.gd (#2136).\n[Desktop Entry]" kio_plain "${kio_plain}")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/kio-servicemenu-plain.desktop" CONTENT "${kio_plain}" @ONLY)

  add_library(putgd-kio MODULE kio-plugin.cpp)
  target_link_libraries(putgd-kio PRIVATE KF6::KIOWidgets KF6::KIOGui KF6::KIOCore KF6::Service KF6::CoreAddons Qt6::Widgets)
  target_include_directories(putgd-kio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(putgd-kio PRIVATE $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG>)
  target_compile_options(putgd-kio PRIVATE -Wall -Wextra)
  # Only the libraries it uses: each NEEDED KF6 library is one dpkg-shlibdeps must not turn into a dependency.
  target_link_options(putgd-kio PRIVATE "LINKER:--as-needed")
  set_target_properties(putgd-kio PROPERTIES AUTOMOC ON CXX_VISIBILITY_PRESET hidden)
  set_property(SOURCE kio-plugin.cpp APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/putgd-kio.json")
  # The KF6 libraries it NEEDs, for the DEB's dpkg-shlibdeps (packaging/linux).
  add_custom_command(TARGET putgd-kio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DPLUGIN=$<TARGET_FILE:putgd-kio> -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/kio-shlibs.local
      -P "${CMAKE_CURRENT_SOURCE_DIR}/kio-weak-deps.cmake" VERBATIM)
  # KIO names the plugin by its file: kf6/kfileitemaction/putgd.so, plugin ID putgd.
  set_target_properties(putgd-kio PROPERTIES PREFIX "" OUTPUT_NAME putgd)
  if(NOT QT6_INSTALL_PREFIX OR NOT QT6_INSTALL_PLUGINS)
    message(FATAL_ERROR "Qt6 doesn't name its plugin directory (QT6_INSTALL_PREFIX, QT6_INSTALL_PLUGINS)")
  endif()
  set(kio_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/kf6/kfileitemaction")
  string(FIND "${kio_dir}/" "${CMAKE_INSTALL_PREFIX}/" kio_under)
  if(kio_under EQUAL 0)
    file(RELATIVE_PATH kio_dir "${CMAKE_INSTALL_PREFIX}" "${kio_dir}")
  endif()
  set_target_properties(putgd-kio PROPERTIES PUTGD_INSTALL_DIR "${kio_dir}"
    PUTGD_SERVICE_MENU "${CMAKE_CURRENT_BINARY_DIR}/kio-servicemenu-plain.desktop"
    PUTGD_WEAK_DEPENDENCIES "${CMAKE_CURRENT_BINARY_DIR}/kio-shlibs.local")
  add_executable(putgd-kio-check kio-check.cpp)
  target_include_directories(putgd-kio-check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(putgd-kio-check PRIVATE KF6::KIOWidgets Qt6::Widgets)
endif()
