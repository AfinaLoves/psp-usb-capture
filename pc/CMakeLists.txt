cmake_minimum_required(VERSION 3.21)

# Use vcpkg (manifest mode, see vcpkg.json) unless a toolchain is given explicitly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

project(pspcap VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# vcpkg's libusb ships no CMake config, only headers + import library.
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0 REQUIRED)

# stb is header-only.
find_path(STB_INCLUDE_DIR stb_image_write.h REQUIRED)

# Warnings and Windows header hygiene shared by every target here.
function(pspcap_target_defaults target)
    target_include_directories(${target} PRIVATE ../common src)
    if(WIN32)
        # libusb.h pulls in windows.h; keep its min/max macros away from std::min.
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    endif()
endfunction()

add_executable(pspcap
    src/main.cpp
    src/settings.cpp
    src/settings_menu.cpp
    src/audio_out.cpp
    src/commands.cpp
    src/frame_receiver.cpp
    src/image_io.cpp
    src/pixel_format.cpp
    src/renderer.cpp
    src/usb_link.cpp
)
pspcap_target_defaults(pspcap)
target_compile_definitions(pspcap PRIVATE
    PSPCAP_VERSION="${PROJECT_VERSION}"
    SDL_MAIN_HANDLED
)
target_include_directories(pspcap PRIVATE ${LIBUSB_INCLUDE_DIR} ${STB_INCLUDE_DIR})
target_link_libraries(pspcap PRIVATE SDL2::SDL2 OpenGL::GL glad::glad imgui::imgui ${LIBUSB_LIBRARY})

# ---- tests (ctest --test-dir pc/build -C Release) ----
enable_testing()

add_executable(pixel_format_test
    tests/pixel_format_test.cpp
    src/pixel_format.cpp
)
pspcap_target_defaults(pixel_format_test)
add_test(NAME pixel_format COMMAND pixel_format_test)
