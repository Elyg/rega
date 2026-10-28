cmake_minimum_required(VERSION 3.25)

# Version comes only from git tags (vX.Y.Z); "dev" when untagged.
execute_process(COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _GIT_TAG OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REGEX REPLACE "^v" "" _APP_VERSION "${_GIT_TAG}")
if(DEFINED REGA_VERSION)
    set(_APP_VERSION "${REGA_VERSION}") # nix builds have no .git
elseif(NOT _APP_VERSION)
    set(_APP_VERSION "dev")
endif()
set(PROJECT_VERSION_STRING "${_APP_VERSION}")

project(rega LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++20, not gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REGA_BUILD_TESTS "Build unit tests" ON)

# Dependencies come from conan (dev) or nixpkgs (nix build); target names differ slightly.
find_package(ftxui REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenEXR REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libdeflate REQUIRED)
find_package(TIFF REQUIRED)
find_package(CLI11 REQUIRED)
find_package(OpenColorIO REQUIRED)
find_package(Threads REQUIRED)
if(TARGET ftxui::ftxui)
    set(REGA_FTXUI ftxui::ftxui)
else()
    set(REGA_FTXUI ftxui::component ftxui::dom ftxui::screen)
endif()
# conan: libdeflate::libdeflate; libdeflate's own config: _static / _shared.
if(TARGET libdeflate::libdeflate)
    set(REGA_LIBDEFLATE libdeflate::libdeflate)
elseif(TARGET libdeflate::libdeflate_static)
    set(REGA_LIBDEFLATE libdeflate::libdeflate_static)
else()
    set(REGA_LIBDEFLATE libdeflate::libdeflate_shared)
endif()
find_package(stb QUIET)
if(NOT TARGET stb::stb)
    find_path(STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb REQUIRED)
    add_library(stb::stb INTERFACE IMPORTED)
    target_include_directories(stb::stb INTERFACE ${STB_INCLUDE_DIR})
endif()

configure_file(cmake/BuildInfo.h.in ${CMAKE_BINARY_DIR}/generated/BuildInfo.h @ONLY)

# Sources listed by hand (no globbing): adding a file means adding it here.
add_library(rega_core STATIC
    src/app/Annotations.cpp
    src/app/AnnotationsPane.cpp
    src/app/App.cpp
    src/app/AppContext.cpp
    src/app/ColourPane.cpp
    src/app/Browser.cpp
    src/app/FileWatcher.cpp
    src/app/FilesPane.cpp
    src/app/HelpKeys.cpp
    src/app/InspectorPane.cpp
    src/app/LayersPane.cpp
    src/app/LineEditor.cpp
    src/app/MetaPane.cpp
    src/app/Playbar.cpp
    src/app/Player.cpp
    src/app/Sheet.cpp
    src/app/Tour.cpp
    src/app/Tutorial.cpp
    src/app/Viewer.cpp
    src/app/ViewerState.cpp
    src/image/Colour.cpp
    src/image/ExrLoader.cpp
    src/image/ImageBuffer.cpp
    src/image/ImageService.cpp
    src/image/Loader.cpp
    src/image/Overlay.cpp
    src/image/Render.cpp
    src/image/Sequence.cpp
    src/image/StbLoader.cpp
    src/image/TiffLoader.cpp
    src/term/BitmapSlot.cpp
    src/term/Caps.cpp
    src/term/Doctor.cpp
    src/term/ImageView.cpp
    src/term/Kitty.cpp
    src/term/LinkMeter.cpp
    src/term/Output.cpp
    src/util/Clipboard.cpp
    src/util/Fuzzy.cpp
    src/util/Log.cpp
    src/util/Parallel.cpp
    src/util/Ui.cpp)
target_include_directories(rega_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(rega_core PUBLIC
    ${REGA_FTXUI} spdlog::spdlog OpenEXR::OpenEXR OpenColorIO::OpenColorIO
    stb::stb TIFF::TIFF ZLIB::ZLIB
    ${REGA_LIBDEFLATE} Threads::Threads)

add_executable(rega src/main.cpp)
target_link_libraries(rega PRIVATE rega_core CLI11::CLI11)

# Only add the bucket render demo tool if the source file exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/bucket-render.cpp")
    add_executable(rega_buckets EXCLUDE_FROM_ALL tools/bucket-render.cpp)
    target_link_libraries(rega_buckets PRIVATE OpenEXR::OpenEXR)
endif()


if(REGA_BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_executable(rega_tests
        tests/test_annotations.cpp
        tests/test_browser.cpp
        tests/test_clipboard.cpp
        tests/test_colour.cpp
        tests/test_file_watcher.cpp
        tests/test_help_keys.cpp
        tests/test_image.cpp
        tests/test_line_editor.cpp
        tests/test_link_meter.cpp
        tests/test_log.cpp
        tests/test_panes.cpp
        tests/test_playbar.cpp
        tests/test_player.cpp
        tests/test_sequence_kitty.cpp
        tests/test_sheet.cpp
        tests/test_tour.cpp
        tests/test_transfer.cpp
        tests/test_tutorial.cpp
        tests/test_util_ui.cpp
        tests/test_viewer.cpp)
    target_link_libraries(rega_tests PRIVATE rega_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(rega_tests)
endif()

install(TARGETS rega RUNTIME DESTINATION bin)
