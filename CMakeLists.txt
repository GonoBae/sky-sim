cmake_minimum_required(VERSION 3.16)

project(cloud_sim_server VERSION 0.1.0 LANGUAGES CXX)

add_executable(cloud_sim_server
    src/main.cpp
)

target_compile_features(cloud_sim_server PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(cloud_sim_server PRIVATE /W4 /permissive-)
    target_link_libraries(cloud_sim_server PRIVATE ws2_32)
else()
    target_compile_options(cloud_sim_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cloud_sim_self_test COMMAND cloud_sim_server --self-test)
endif()
