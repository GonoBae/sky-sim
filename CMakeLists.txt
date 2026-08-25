cmake_minimum_required(VERSION 3.16)

project(cloud_sim_server VERSION 0.3.0 LANGUAGES CXX)

add_executable(cloud_sim_server
    src/main.cpp
)

target_compile_features(cloud_sim_server PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(cloud_sim_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(cloud_sim_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(cloud_sim_server PRIVATE ws2_32)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cloud_sim_self_test COMMAND cloud_sim_server --self-test)

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME cloud_protocol_python_test
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_receive_probe.py
        )
        add_test(
            NAME cloud_interactor_python_test
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_send_interactor.py
        )
        add_test(
            NAME cloud_protocol_loopback_test
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_protocol_loopback.py
                    --server $<TARGET_FILE:cloud_sim_server>
        )
        add_test(
            NAME cloud_control_loopback_test
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/test_control_loopback.py
                    --server $<TARGET_FILE:cloud_sim_server>
        )
        set_tests_properties(
            cloud_protocol_loopback_test
            cloud_control_loopback_test
            PROPERTIES TIMEOUT 12
        )
    endif()
endif()
