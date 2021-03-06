INCLUDE_DIRECTORIES(AFTER ${Memcached_SOURCE_DIR}/auditd/src)
ADD_EXECUTABLE(memcached_auditfile_test auditfile_test.cc
               ${Memcached_SOURCE_DIR}/auditd/src/auditconfig.h
               ${Memcached_SOURCE_DIR}/auditd/src/auditconfig.cc
               ${Memcached_SOURCE_DIR}/auditd/src/auditfile.h
               ${Memcached_SOURCE_DIR}/auditd/src/auditfile.cc
               ${Memcached_SOURCE_DIR}/include/memcached/isotime.h)
SET_TARGET_PROPERTIES(memcached_auditfile_test
                      PROPERTIES COMPILE_FLAGS -DUNITTEST_AUDITFILE=1)
TARGET_LINK_LIBRARIES(memcached_auditfile_test mcd_time dirutils cJSON JSON_checker platform gtest gtest_main)
ADD_TEST(NAME memcached-auditfile-test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND memcached_auditfile_test)

ADD_EXECUTABLE(memcached_auditconfig_test auditconfig_test.cc
               ${Memcached_SOURCE_DIR}/auditd/src/auditconfig.h
               ${Memcached_SOURCE_DIR}/auditd/src/auditconfig.cc)
TARGET_LINK_LIBRARIES(memcached_auditconfig_test dirutils cJSON platform gtest gtest_main)
ADD_TEST(NAME memcached-auditconfig-test
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND memcached_auditconfig_test)

ADD_EXECUTABLE(memcached_auditd_tests testauditd.cc)
TARGET_LINK_LIBRARIES(memcached_auditd_tests auditd mcd_util mcd_time cJSON dirutils)
ADD_DEPENDENCIES(memcached_auditd_tests generate_audit_descriptors)
ADD_TEST(NAME memcached-auditd-test
         COMMAND memcached_auditd_tests ${Memcached_BINARY_DIR}/auditd)
