# System Qt stays dynamically linked and receives distro maintenance updates.
find_package(Qt6 6.4.2 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Qt6 6.4.2 REQUIRED COMPONENTS DBus)
endif()
find_package(Qt6Keychain 0.14.2 CONFIG REQUIRED)
# Provision separately using the SDK's own CMake project; never download at configure time.
find_package(AWSSDK 1.11.907 EXACT CONFIG REQUIRED COMPONENTS s3)
