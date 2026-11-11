# Windows components. Each lives in its own folder; see docs/ARCHITECTURE.md.
foreach(dir media vcam agent tools/fakephone tools/vcamtest tools/vcamctl tools/muxprobe tests)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
