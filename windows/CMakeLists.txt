cmake_minimum_required(VERSION 3.20)
project(WinDrop LANGUAGES CXX RC)

# One version for the whole project: the VERSION file at the repo root (also read by Gradle).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" WINDROP_VERSION LIMIT_COUNT 1)
string(REPLACE "." ";" _v "${WINDROP_VERSION}")
list(GET _v 0 WINDROP_V1)
list(GET _v 1 WINDROP_V2)
list(GET _v 2 WINDROP_V3)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

set(CMAKE_CXX_STANDARD 20)  # C++/WinRT
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(WinDrop WIN32
  src/main.cpp
  src/common.cpp
  src/Settings.cpp
  src/NetworkEngine.cpp
  src/TopOverlayWindow.cpp
  src/DropTarget.cpp
  src/WifiDirectHost.cpp
  src/Crypto.cpp
  src/Updater.cpp
  src/Diagnostics.cpp
  src/Radios.cpp
  res/app.rc
)

target_compile_definitions(WinDrop PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
  WINDROP_VERSION="${WINDROP_VERSION}" WINDROP_V1=${WINDROP_V1} WINDROP_V2=${WINDROP_V2} WINDROP_V3=${WINDROP_V3})
target_link_libraries(WinDrop PRIVATE ws2_32 ole32 oleaut32 shell32 dwmapi gdiplus iphlpapi shcore uuid user32 gdi32 advapi32 windowsapp bcrypt crypt32 winhttp)

if(MINGW)
  target_link_options(WinDrop PRIVATE -municode -static)
  target_compile_options(WinDrop PRIVATE -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
elseif(MSVC)
  target_compile_options(WinDrop PRIVATE /utf-8 /W3 /EHsc /permissive- /bigobj)
  target_link_options(WinDrop PRIVATE /MANIFEST:NO)
  set_property(TARGET WinDrop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
