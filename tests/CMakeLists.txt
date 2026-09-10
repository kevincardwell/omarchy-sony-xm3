cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Stress, boundary and concurrency harnesses.
#
# These are separate from daemon/tests/test_protocol.cpp: that one is the fast
# correctness suite wired into CTest, while these are longer-running adversarial
# harnesses. They are still registered with CTest so `ctest` runs everything.
# ---------------------------------------------------------------------------

if(NOT BUILD_TESTING)
    return()
endif()

set(STRESS_TARGETS
    stress_test_protocol
    stress_test_boundary_transport
    stress_state_engine
    stress_multi_process
)

foreach(target IN LISTS STRESS_TARGETS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp")
        add_executable(${target} ${target}.cpp)
        target_link_libraries(${target} PRIVATE sony_xm3_protocol_lib)
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/daemon/src
        )
        add_test(NAME ${target} COMMAND ${target})
    endif()
endforeach()

# Multi-process and lifecycle harnesses fork and wait on real sockets, so give
# them more headroom than CTest's default.
foreach(target stress_multi_process stress_state_engine)
    if(TARGET ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 300)
    endif()
endforeach()
