cmake_minimum_required(VERSION 3.25)

project(peres LANGUAGES CXX RC)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

if(NOT MSVC)
    message(FATAL_ERROR "peres requires MSVC (Visual Studio 2026 toolchain)")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_compile_options(/utf-8 /W4 /Zc:__cplusplus /EHsc "$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")

# WTL 9.10 (shipped with the toolset) still relies on the pre-C++20
# unqualified lookup in dependent base classes.  GUI translation units are
# compiled in permissive mode; the PE/resource core stays fully conformant.
set(PERES_WTL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MainFrame.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PreviewWnd.cpp"
)
set_source_files_properties(${PERES_WTL_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/permissive;/Zc:twoPhase-"
)

# Locate the MSVC toolset root (when the shell was prepared by vcvars64.bat).
if(DEFINED ENV{VCToolsInstallDir})
    set(PERES_MSVC_ROOT "$ENV{VCToolsInstallDir}")
else()
    get_filename_component(_peres_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(PERES_MSVC_ROOT "${_peres_cl_dir}/../../.." ABSOLUTE)
endif()
file(TO_CMAKE_PATH "${PERES_MSVC_ROOT}" PERES_MSVC_ROOT)

set(WTL_INCLUDE_DIR "${PERES_MSVC_ROOT}/include/WTL/Include" CACHE PATH "WTL include directory")
if(NOT EXISTS "${WTL_INCLUDE_DIR}/atlapp.h")
    message(FATAL_ERROR "WTL not found at ${WTL_INCLUDE_DIR}. Pass -DWTL_INCLUDE_DIR=<path>.")
endif()

set(PERES_FMT_LIB "${PERES_MSVC_ROOT}/lib/x64/fmt.lib" CACHE FILEPATH "fmt static library")
if(NOT EXISTS "${PERES_FMT_LIB}")
    message(FATAL_ERROR "fmt.lib not found at ${PERES_FMT_LIB}. Pass -DPERES_FMT_LIB=<path>.")
endif()

set(PERES_RESOURCE_H "${CMAKE_CURRENT_SOURCE_DIR}/resource.h")
set(PERES_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
set(PERES_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")
file(TO_CMAKE_PATH "${PERES_RESOURCE_H}" PERES_RESOURCE_H)
file(TO_CMAKE_PATH "${PERES_APP_ICON}" PERES_APP_ICON)
file(TO_CMAKE_PATH "${PERES_APP_MANIFEST}" PERES_APP_MANIFEST)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/peres.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/peres.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/peres.rc"
    PROPERTIES OBJECT_DEPENDS "${PERES_APP_ICON};${PERES_APP_MANIFEST};${PERES_RESOURCE_H}")

add_library(peres_core STATIC
    src/PeImage.cpp
    src/ResourceSection.cpp
    src/ResourceModel.cpp
    src/ResourceId.cpp
    src/ResourceTypes.cpp
    src/TextUtil.cpp
    src/XmlFormat.cpp
    src/StringTableCodec.cpp
    src/MenuCodec.cpp
    src/DialogCodec.cpp
    src/VersionCodec.cpp
    src/IconCodec.cpp
    src/BitmapCodec.cpp
    src/ImageCodec.cpp
    src/Config.cpp
    src/ProcessRunner.cpp
    src/UpXTool.cpp
)
target_include_directories(peres_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_options(peres_core PRIVATE /permissive-)
target_link_libraries(peres_core PUBLIC
    "${PERES_FMT_LIB}"
    windowscodecs ole32 oleaut32 uuid
)

add_executable(peres WIN32
    src/main.cpp
    src/MainFrame.cpp
    src/PreviewWnd.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/peres.rc"
)

target_include_directories(peres PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WTL_INCLUDE_DIR}"
    "${PERES_MSVC_ROOT}/atlmfc/include"
)

target_link_libraries(peres PRIVATE
    peres_core
    "${PERES_FMT_LIB}"
    comctl32 comdlg32 shell32 shlwapi ole32 oleaut32 uuid uxtheme version
    gdi32 user32 advapi32 msimg32
)

target_link_options(peres PRIVATE /MANIFEST:NO)

add_custom_command(TARGET peres POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/peres.ini"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/peres.ini"
)

# ---- Tests ----------------------------------------------------------------
enable_testing()

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/sample_data.txt" PERES_SAMPLE_DATA)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/sample.png" PERES_SAMPLE_PNG)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/sample.jpg" PERES_SAMPLE_JPG)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tests/sample.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/sample.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/sample.rc"
    PROPERTIES OBJECT_DEPENDS "${PERES_APP_ICON};${PERES_SAMPLE_PNG};${PERES_SAMPLE_JPG};${PERES_SAMPLE_DATA}")

add_library(peres_test_dll SHARED
    tests/sample_dll.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/sample.rc"
)
set_target_properties(peres_test_dll PROPERTIES
    PREFIX ""
    OUTPUT_NAME "peres_sample"
)
target_link_options(peres_test_dll PRIVATE /MANIFEST:NO)

add_executable(peres_tests tests/test_main.cpp)
target_compile_options(peres_tests PRIVATE /permissive-)
target_link_libraries(peres_tests PRIVATE peres_core user32 version)
target_compile_definitions(peres_tests PRIVATE
    PERES_TEST_SAMPLE_DLL="$<TARGET_FILE:peres_test_dll>"
    PERES_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
    PERES_TEST_IMAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets"
    PERES_TEST_EXE="$<TARGET_FILE:peres>"
)
add_dependencies(peres_tests peres peres_test_dll)
add_test(NAME peres_core_tests COMMAND peres_tests)
