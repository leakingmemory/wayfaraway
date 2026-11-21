cmake_minimum_required(VERSION 3.28)

project(wayfaraway
    VERSION 0.1.0
    DESCRIPTION "Headless wlroots compositor served over RDP"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

pkg_check_modules(WLROOTS   REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND   REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKB       REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(PANGO     REQUIRED IMPORTED_TARGET pangocairo cairo)
pkg_check_modules(FREERDP   REQUIRED IMPORTED_TARGET freerdp3 freerdp-server3 winpr3)
pkg_check_modules(PAM       REQUIRED IMPORTED_TARGET pam)

option(WAYFARAWAY_AUDIO "Bridge RDP audio and microphone through PipeWire" ON)
if(WAYFARAWAY_AUDIO)
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    if(NOT PIPEWIRE_FOUND)
        message(WARNING "libpipewire-0.3 not found; building without audio")
    endif()
endif()

add_executable(wayfaraway
    src/main.cpp
    src/config.cpp
    src/server.cpp
    src/input.cpp
    src/view.cpp
    src/lock_screen.cpp
    src/authenticator.cpp
    src/clipboard.cpp
    src/rdp_audio.cpp
    src/rdp_display.cpp
    src/rdp_microphone.cpp
    src/rdp_server.cpp
    src/tls_certificate.cpp
)

if(PIPEWIRE_FOUND)
    target_sources(wayfaraway PRIVATE src/pipewire.cpp src/audio_sink.cpp src/audio_source.cpp)
    target_compile_definitions(wayfaraway PRIVATE WAYFARAWAY_HAVE_PIPEWIRE)
    target_link_libraries(wayfaraway PRIVATE PkgConfig::PIPEWIRE)
endif()

target_compile_definitions(wayfaraway PRIVATE
    WLR_USE_UNSTABLE
    WAYFARAWAY_VERSION="${PROJECT_VERSION}"
    WAYFARAWAY_DEFAULT_PAM_SERVICE="wayfaraway"
)

target_compile_options(wayfaraway PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    # wlroots/FreeRDP headers are C; silence noise they generate under C++.
    -Wno-missing-field-initializers
    -Wno-c99-extensions
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(wayfaraway PRIVATE -Wno-pedantic)
endif()

target_link_libraries(wayfaraway PRIVATE
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND
    PkgConfig::XKB
    PkgConfig::PIXMAN
    PkgConfig::PANGO
    PkgConfig::FREERDP
    PkgConfig::PAM
    OpenSSL::Crypto
)

install(TARGETS wayfaraway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/wayfaraway.pam
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/pam.d
    RENAME wayfaraway)
install(FILES data/wayfaraway.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
