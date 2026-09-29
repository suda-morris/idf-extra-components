idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_screenshot.c"
                            "src/esp_lcd_host_sdl_backend.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd)

set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/SDL")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR
        "esp_lcd_host builds for the ESP-IDF linux (POSIX) target, which runs on "
        "Linux and macOS. This machine reports the host system "
        "'${CMAKE_SYSTEM_NAME}', which the target does not support.")
endif()

message(STATUS "esp_lcd_host: building SDL for the ${CMAKE_SYSTEM_NAME} host")

# Only build a static SDL3, and none of the SDL samples, tests or install
# rules. Plain variables suffice: the subdirectory inherits the variables of
# this directory, and SDL reads its options with option() and
# cmake_dependent_option(), which honor an existing normal variable (CMP0077,
# new since the cmake_minimum_required of SDL).
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_DISABLE_INSTALL ON)

# Subsystems the panel does not use.
set(SDL_AUDIO OFF)
set(SDL_CAMERA OFF)
set(SDL_JOYSTICK OFF)
set(SDL_HAPTIC OFF)
set(SDL_HIDAPI OFF)
set(SDL_POWER OFF)
set(SDL_SENSOR OFF)
set(SDL_DIALOG OFF)
set(SDL_TRAY OFF)
set(SDL_GPU OFF)

# Optional third party libraries SDL would otherwise pick up
set(SDL_SYSTEM_ICONV ON)
set(SDL_LIBICONV OFF)
set(SDL_FRIBIDI OFF)
set(SDL_LIBTHAI OFF)
set(SDL_DBUS OFF)
set(SDL_IBUS OFF)
set(SDL_LIBURING OFF)
set(SDL_LIBUDEV OFF)

# Video drivers that are tied to a board or to a GPU: off everywhere
set(SDL_RPI OFF)
set(SDL_ROCKCHIP OFF)
set(SDL_VIVANTE OFF)

# Render drivers the panel never selects: it renders through the SDL software
# renderer and presents with SDL_RenderPresent().
set(SDL_OPENGL OFF)
set(SDL_VULKAN OFF)
set(SDL_RENDER_GPU OFF)
set(SDL_RENDER_VULKAN OFF)

# Do not record dlopen dependencies in .note.dlopen sections: SDL is built into
# an ESP-IDF application, which does not read those sections.
set(SDL_DLOPEN_NOTES OFF)

# The console build is allowed on Linux so that a machine without the desktop packages can still build
set(SDL_UNIX_CONSOLE_BUILD ON)

# SDL and the ESP-IDF build system both vendor the same include-guarded
# GetGitRevisionDescription module. The build system includes its copy first,
# so the guard variable is already set here and the include of the SDL copy
# would return early: the git_describe() that stays active is then the one of
# the build system, which takes a second argument that the SDL project does not
# pass, and the SDL configuration fails with "Function invoked with incorrect
# arguments". Clear the guard so the SDL project registers its own copy. The
# function definitions of a subdirectory do not leak back into the build
# system, so this only affects the SDL project itself.
unset(__get_git_revision_description)

# idf_component_register() seeds the directory scope with the include
# directories, the compile options and the definitions of the build system (the
# linux build compiles everything with -Werror and a fixed C standard, which
# the SDL sources do not survive), plus the common link libraries: an exported
# target may not link targets outside the export set. Snapshot all of that onto
# the component target and reset the directory scope, so the SDL subproject
# configures with the flags of its own project, exactly like a standalone build
# of SDL, and the component keeps compiling the way the build system configured
# it.
set(_lcd_host_dir_props INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS
                        C_COMPILE_OPTIONS CXX_COMPILE_OPTIONS ASM_COMPILE_OPTIONS)
foreach(_lcd_host_prop IN LISTS _lcd_host_dir_props)
    get_property(_lcd_host_val DIRECTORY PROPERTY ${_lcd_host_prop})
    if(_lcd_host_val)
        if(_lcd_host_prop STREQUAL "INCLUDE_DIRECTORIES")
            target_include_directories(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        elseif(_lcd_host_prop STREQUAL "COMPILE_DEFINITIONS")
            # The definitions are stored without their -D prefix and the
            # target_compile_definitions command adds it back.
            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        else()
            target_compile_options(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        endif()
    endif()
    set_property(DIRECTORY PROPERTY ${_lcd_host_prop} "")
endforeach()
unset(_lcd_host_dir_props)
unset(_lcd_host_val)
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

add_subdirectory("${SDL_DIR}" "SDL")

# The component has to supply a few definitions SDL wants from the parts of
# itself it does not build (see src/sdl_port_stubs.c). They are added to the
# SDL library, because the stub includes the private SDL_internal.h header,
# which is only on the include path of SDL3::SDL3.
target_sources(SDL3-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/sdl_port_stubs.c")

target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
