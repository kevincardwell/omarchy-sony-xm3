cmake_minimum_required(VERSION 3.20)

# Configurable on its own, so `setup --uninstall` can build just this helper.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sony-xm3-deploy LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# sony-xm3-deploy: the no-follow file placement helper setup runs. It is built
# alongside the daemon but never installed.
add_executable(sony-xm3-deploy
    deploy.cpp
)

target_compile_features(sony-xm3-deploy PRIVATE cxx_std_20)

if(BUILD_TESTING)
    add_test(NAME test_deploy
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_deploy.sh $<TARGET_FILE:sony-xm3-deploy>
    )
endif()
