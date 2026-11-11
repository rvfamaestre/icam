add_executable(icam-vcamctl main.cpp)
target_link_libraries(icam-vcamctl PRIVATE icam_common mfplat mf mfuuid mfsensorgroup ole32)
