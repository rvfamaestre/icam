cmake_minimum_required(VERSION 3.25)
project(icam VERSION 0.2.1 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The PC side of iCam targets Windows 11 only (the iOS app is built with xtool, see ios/).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the camera DLL runs inside the Frame Server service; don't depend on a redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zi /wd4100 /wd4324 /sdl /guard:cf)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
                        ICAM_VERSION="${PROJECT_VERSION}")
add_link_options(/DEBUG /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf /CETCOMPAT)

enable_testing()

add_library(icam_common STATIC
  common/src/protocol.cpp
  common/src/plist.cpp
  common/src/net.cpp
  common/src/usbmux.cpp
  common/src/shm.cpp
  common/src/image.cpp
  common/src/log.cpp)
target_include_directories(icam_common PUBLIC common/include)
target_link_libraries(icam_common PUBLIC ws2_32 advapi32 gdi32 shell32 ole32 cfgmgr32)

add_executable(common_tests common/tests/common_tests.cpp)
target_link_libraries(common_tests PRIVATE icam_common)
target_compile_definitions(common_tests PRIVATE ICAM_VECTORS="${CMAKE_SOURCE_DIR}/docs/protocol-vectors-v1.txt")
add_test(NAME common_tests COMMAND common_tests)
set_tests_properties(common_tests PROPERTIES RESOURCE_LOCK icam_local_shm)

add_subdirectory(windows)
