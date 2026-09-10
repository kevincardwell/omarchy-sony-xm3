cmake_minimum_required(VERSION 3.20)

# Package Resolution via PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Target 1: sony_xm3_protocol_lib (Static Protocol, State & Bluetooth Transport Library)
# ---------------------------------------------------------------------------
set(PROTOCOL_LIB_SOURCES
    src/MDRProtocolV1.cpp
    src/BluetoothManager.cpp
    src/StateEngine.cpp
    src/IpcServer.cpp
)

add_library(sony_xm3_protocol_lib STATIC
    ${PROTOCOL_LIB_SOURCES}
)

target_include_directories(sony_xm3_protocol_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sony_xm3_protocol_lib PUBLIC
    PkgConfig::BLUEZ
    PkgConfig::DBUS
    Threads::Threads
)

target_compile_features(sony_xm3_protocol_lib PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Target 2: test_protocol (Protocol, Framing, StateEngine & IpcServer Tests)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.cpp")
        add_executable(test_protocol
            tests/test_protocol.cpp
        )

        target_link_libraries(test_protocol PRIVATE
            sony_xm3_protocol_lib
        )

        target_include_directories(test_protocol PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        add_test(NAME ProtocolTest COMMAND test_protocol)
    endif()
endif()

# ---------------------------------------------------------------------------
# Target 3: sony-xm3-daemon (Headless Daemon Executable)
# ---------------------------------------------------------------------------
add_executable(sony-xm3-daemon
    src/main.cpp
)

target_link_libraries(sony-xm3-daemon PRIVATE
    sony_xm3_protocol_lib
)

target_include_directories(sony-xm3-daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

install(TARGETS sony-xm3-daemon RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Target 4: systemd User Service Unit
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sony-xm3.service")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/sony-xm3.service"
        DESTINATION "lib/systemd/user"
    )
endif()
