add_executable(icam-vcamtest main.cpp)
target_link_libraries(icam-vcamtest PRIVATE icam_common mfplat mf mfreadwrite mfuuid ole32)
