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
pkg_check_modules(RSVG      REQUIRED IMPORTED_TARGET librsvg-2.0)
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
    src/retro_theme.cpp
    src/desktop_entry.cpp
    src/icon_loader.cpp
    src/desktop_icons.cpp
    src/program_manager.cpp
    src/task_switcher.cpp
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
    PkgConfig::RSVG
    PkgConfig::FREERDP
    PkgConfig::PAM
    OpenSSL::Crypto
)

# PAM only reads /etc/pam.d, whatever the install prefix, so this is an
# absolute path rather than one under CMAKE_INSTALL_SYSCONFDIR (which would
# give /usr/etc or /usr/local/etc). DESTDIR is still honoured for packaging.
set(WAYFARAWAY_PAM_DIR "/etc/pam.d" CACHE PATH "Directory to install the PAM service file into")
# The system stack the service file includes for auth and account.
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(WAYFARAWAY_PAM_INCLUDE_DEFAULT "system")
else()
    set(WAYFARAWAY_PAM_INCLUDE_DEFAULT "system-local-login")
endif()
set(WAYFARAWAY_PAM_INCLUDE "${WAYFARAWAY_PAM_INCLUDE_DEFAULT}"
    CACHE STRING "PAM service that wayfaraway's auth and account stacks include")
configure_file(data/wayfaraway.pam.in ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.pam @ONLY)

# A systemd user unit, so the desktop can run as a per-user service:
#   systemctl --user enable --now wayfaraway
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(WAYFARAWAY_SYSTEMD_DEFAULT ON)
else()
    set(WAYFARAWAY_SYSTEMD_DEFAULT OFF)
endif()
option(WAYFARAWAY_SYSTEMD "Install a systemd user service" ${WAYFARAWAY_SYSTEMD_DEFAULT})

if(WAYFARAWAY_SYSTEMD)
    # Ask systemd where user units go, relative to our prefix, so a
    # /usr/local install lands in /usr/local/lib/systemd/user.
    set(systemd_user_unit_dir "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
    pkg_check_modules(SYSTEMD QUIET systemd)
    if(SYSTEMD_FOUND)
        pkg_get_variable(systemd_pc_user_unit_dir systemd systemduserunitdir
            DEFINE_VARIABLES prefix=${CMAKE_INSTALL_PREFIX})
        if(systemd_pc_user_unit_dir)
            set(systemd_user_unit_dir "${systemd_pc_user_unit_dir}")
        endif()
    endif()
    set(WAYFARAWAY_SYSTEMD_USER_UNIT_DIR "${systemd_user_unit_dir}"
        CACHE PATH "Directory to install the systemd user unit into")

    configure_file(data/wayfaraway.service.in ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.service @ONLY)
endif()

# An rc.d script, so the desktop can run as a service on FreeBSD:
#   sysrc wayfaraway_enable=YES wayfaraway_user=alice
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(WAYFARAWAY_RC_DEFAULT ON)
else()
    set(WAYFARAWAY_RC_DEFAULT OFF)
endif()
option(WAYFARAWAY_RC "Install a FreeBSD rc.d script" ${WAYFARAWAY_RC_DEFAULT})

if(WAYFARAWAY_RC)
    # Local services live in ${PREFIX}/etc/rc.d, so /usr/local/etc/rc.d.
    set(WAYFARAWAY_RC_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/rc.d"
        CACHE PATH "Directory to install the rc.d script into")
    configure_file(data/wayfaraway.rc.in ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.rc @ONLY)
endif()

# An OpenRC init script, for Gentoo and other OpenRC systems:
#   ln -s wayfaraway /etc/init.d/wayfaraway.alice
#   rc-update add wayfaraway.alice default
set(WAYFARAWAY_OPENRC_DEFAULT OFF)
foreach(dir /sbin /usr/sbin /usr/bin)
    if(EXISTS ${dir}/openrc-run)
        set(WAYFARAWAY_OPENRC_DEFAULT ON)
    endif()
endforeach()
option(WAYFARAWAY_OPENRC "Install an OpenRC init script" ${WAYFARAWAY_OPENRC_DEFAULT})

if(WAYFARAWAY_OPENRC)
    # Like PAM, OpenRC only looks in /etc, whatever the install prefix.
    set(WAYFARAWAY_OPENRC_INITD_DIR "/etc/init.d" CACHE PATH "Directory to install the OpenRC init script into")
    set(WAYFARAWAY_OPENRC_CONFD_DIR "/etc/conf.d" CACHE PATH "Directory to install the OpenRC service configuration into")
    configure_file(data/wayfaraway.openrc.in ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.openrc @ONLY)
endif()

# A runit service, for Void Linux:
#   ln -s /etc/sv/wayfaraway /var/service/
set(WAYFARAWAY_RUNIT_DEFAULT OFF)
foreach(dir /sbin /usr/sbin /usr/bin)
    if(EXISTS ${dir}/runsvdir AND IS_DIRECTORY /etc/sv)
        set(WAYFARAWAY_RUNIT_DEFAULT ON)
    endif()
endforeach()
option(WAYFARAWAY_RUNIT "Install a runit service" ${WAYFARAWAY_RUNIT_DEFAULT})

if(WAYFARAWAY_RUNIT)
    set(WAYFARAWAY_RUNIT_DIR "/etc/sv" CACHE PATH "Directory to install the runit service directory into")
    configure_file(data/runit/run.in ${CMAKE_CURRENT_BINARY_DIR}/runit/run @ONLY)
endif()

# Installs a configuration file only if there is none yet: once installed, it
# is the admin's.
function(wayfaraway_install_config source destination name)
    install(CODE "
        if(NOT EXISTS \"\$ENV{DESTDIR}${destination}/${name}\")
            file(INSTALL \"${source}\" DESTINATION \"${destination}\" RENAME \"${name}\")
        endif()
    ")
endfunction()

install(TARGETS wayfaraway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.pam
    DESTINATION ${WAYFARAWAY_PAM_DIR}
    RENAME wayfaraway)
if(WAYFARAWAY_SYSTEMD)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.service
        DESTINATION ${WAYFARAWAY_SYSTEMD_USER_UNIT_DIR})
endif()
if(WAYFARAWAY_RC)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.rc
        DESTINATION ${WAYFARAWAY_RC_DIR}
        RENAME wayfaraway)
endif()
if(WAYFARAWAY_OPENRC)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/wayfaraway.openrc
        DESTINATION ${WAYFARAWAY_OPENRC_INITD_DIR}
        RENAME wayfaraway)
    wayfaraway_install_config(${CMAKE_CURRENT_SOURCE_DIR}/data/wayfaraway.confd
        ${WAYFARAWAY_OPENRC_CONFD_DIR} wayfaraway)
endif()
if(WAYFARAWAY_RUNIT)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/runit/run
        DESTINATION ${WAYFARAWAY_RUNIT_DIR}/wayfaraway)
    install(PROGRAMS data/runit/log/run
        DESTINATION ${WAYFARAWAY_RUNIT_DIR}/wayfaraway/log)
    wayfaraway_install_config(${CMAKE_CURRENT_SOURCE_DIR}/data/runit/conf
        ${WAYFARAWAY_RUNIT_DIR}/wayfaraway conf)
endif()
