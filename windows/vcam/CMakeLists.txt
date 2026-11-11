add_library(icam-vcam SHARED
  dllmain.cpp
  source.cpp
  stream.cpp
  frames.cpp
  icam-vcam.def)
target_link_libraries(icam-vcam PRIVATE icam_common mfplat mfuuid mf runtimeobject propsys strmiids)
