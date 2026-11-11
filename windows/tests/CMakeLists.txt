add_executable(vcam_security_tests
  vcam_security_tests.cpp
  ../vcam/source.cpp
  ../vcam/stream.cpp
  ../vcam/frames.cpp)
target_include_directories(vcam_security_tests PRIVATE ../vcam)
target_link_libraries(vcam_security_tests PRIVATE icam_common mfplat mfuuid mf runtimeobject propsys strmiids)
add_test(NAME vcam_security_tests COMMAND vcam_security_tests)
set_tests_properties(vcam_security_tests PROPERTIES RESOURCE_LOCK icam_local_shm)
set_tests_properties(vcam_security_tests PROPERTIES ENVIRONMENT "ICAM_SHM_NAMESPACE=Local")
