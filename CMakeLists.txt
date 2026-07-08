cmake_minimum_required(VERSION 3.13)

# Define project name, version, and language
project(RDP-KeepAlive LANGUAGES C VERSION 1.0.0)

# ============================================================
# FreeRDP dependency configuration
# ============================================================

set(WITH_CLIENT ON CACHE BOOL "Enable FreeRDP client support")
set(WITH_CLIENT_SDL OFF CACHE BOOL "Disable SDL client")
set(WITH_CLIENT_SDL2 OFF CACHE BOOL "Disable SDL2 client")
set(WITH_CLIENT_SDL3 OFF CACHE BOOL "Disable SDL3 client")
set(WITH_CLIENT_WINDOWS OFF CACHE BOOL "Disable native Windows client")
set(WITH_SERVER OFF CACHE BOOL "Disable server build")
set(WITH_PROXY OFF CACHE BOOL "Disable proxy build")
set(WITH_SHADOW OFF CACHE BOOL "Disable shadow build")
set(WITH_PLATFORM_SERVER OFF CACHE BOOL "Disable platform server")
set(WITH_FFMPEG OFF CACHE BOOL "Disable FFmpeg")
set(WITH_SWSCALE OFF CACHE BOOL "Disable SWScale")
set(WITH_CAIRO OFF CACHE BOOL "Disable Cairo")
set(WITH_CHANNEL_SSHAGENT OFF CACHE BOOL "Disable SSH Agent channel (needs sys/socket.h)")
set(WITH_CHANNEL_URBDRC OFF CACHE BOOL "Disable USB redirection (needs libusb)")
set(WITH_VERBOSE_WINPR_ASSERT OFF CACHE BOOL "Disable verbose WinPR assertions")
set(WITH_SAMPLE OFF CACHE BOOL "Disable sample client")
set(WITH_ZLIB OFF CACHE BOOL "Disable ZLIB")
set(WITH_SMARTCARD_EMULATE OFF CACHE BOOL "Disable smartcard emulation (needs ZLIB)")
set(CHANNEL_URBDRC OFF CACHE BOOL "Disable USB redirection (needs libusb)")

# FreeRDP compilation standard - use C11 for MSVC compatibility
set(CMAKE_C_STANDARD 11 CACHE STRING "C standard (C11 for MSVC compatibility)")
set(CMAKE_C_STANDARD_REQUIRED ON CACHE BOOL "Require C standard")

# ============================================================
# Build FreeRDP as a dependency
# ============================================================
add_subdirectory(FreeRDP)

# ============================================================
# RDP Keep-Alive tool
# ============================================================

add_executable(rdp-keepalive
    src/rdp_keepalive.c
    src/rdp_keepalive.h
)

target_link_libraries(rdp-keepalive PRIVATE
    freerdp-client
    freerdp
    winpr
)

# Include paths for FreeRDP headers
target_include_directories(rdp-keepalive PRIVATE
    ${CMAKE_BINARY_DIR}/FreeRDP/include      # Generated freerdp/config.h, settings_keys.h
    ${CMAKE_BINARY_DIR}/FreeRDP/winpr/include # Generated winpr/config.h
    ${CMAKE_SOURCE_DIR}/FreeRDP/include
    ${CMAKE_SOURCE_DIR}/FreeRDP/winpr/include
    ${CMAKE_SOURCE_DIR}/src
)

# Windows-specific: add version resource and manifest
if(WIN32 AND MSVC)
    set_target_properties(rdp-keepalive PROPERTIES
        OUTPUT_NAME rdp-keepalive
        WIN32_EXECUTABLE FALSE           # Console application
    )
endif()

# Copy FreeRDP DLLs to output directory (Windows)
if(WIN32)
    add_custom_command(TARGET rdp-keepalive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:winpr>"
            "$<TARGET_FILE_DIR:rdp-keepalive>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:freerdp>"
            "$<TARGET_FILE_DIR:rdp-keepalive>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:freerdp-client>"
            "$<TARGET_FILE_DIR:rdp-keepalive>"
        COMMENT "Copying FreeRDP DLLs to output directory"
    )
endif()

# Install rule
install(TARGETS rdp-keepalive RUNTIME DESTINATION bin)
