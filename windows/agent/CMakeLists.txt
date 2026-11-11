add_executable(icam-agent WIN32 main.cpp engine.cpp tray.cpp)
target_link_libraries(icam-agent PRIVATE icam_media shell32 user32 comctl32)
