add_library(icam_media STATIC media.cpp)
target_include_directories(icam_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(icam_media PUBLIC icam_common mfplat mfuuid mf d3d11 strmiids wmcodecdspuuid)
