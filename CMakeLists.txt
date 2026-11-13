# Polarity installers.
#   Windows (MSVC): polarity-installer.exe        scripts\build-windows.ps1
#   Linux (Alpine/musl, static): polarity-installer + installer_tests
#                                                 scripts\build-linux.ps1
cmake_minimum_required(VERSION 3.20)
project(PolarityInstaller C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  # The static CRT: the exe runs without the Visual C++ runtime installed.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

  add_library(shared STATIC src/core/update_info.cpp src/core/json.cpp)
  target_include_directories(shared PUBLIC src)
  target_link_libraries(shared PUBLIC bcrypt)

  add_executable(polarity_installer WIN32 src/installer/installer_win.cpp src/installer/installer.rc)
  set_target_properties(polarity_installer PROPERTIES OUTPUT_NAME polarity-installer)
  target_link_libraries(polarity_installer PRIVATE shared winhttp comctl32 shell32 ole32)
else()
  # One static binary for any x86_64 Linux: musl (its static getaddrinfo
  # works on every distro; glibc's does not) and mbedTLS for HTTPS and the
  # signature check.
  include(FetchContent)
  # 3.6 LTS: the release tarball (it carries the generated sources a git
  # checkout lacks); the hash is the one mbedTLS publishes with it.
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(mbedtls)

  add_library(shared STATIC src/core/update_info.cpp src/core/json.cpp src/installer/linux_steam.cpp)
  target_include_directories(shared PUBLIC src)
  target_link_libraries(shared PUBLIC mbedtls mbedx509 mbedcrypto)

  add_executable(polarity-installer src/installer/installer_linux.cpp)
  target_link_libraries(polarity-installer PRIVATE shared)
  target_link_options(polarity-installer PRIVATE -static -s)

  add_executable(installer_tests tests/linux_tests.cpp)
  target_link_libraries(installer_tests PRIVATE shared)
  target_link_options(installer_tests PRIVATE -static)
endif()
