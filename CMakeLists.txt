cmake_minimum_required(VERSION 3.22)
project(LilyInEM CXX C)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LilyInEM builds with MSVC x64 only (use build.bat)")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# UE4SS is consumed read-only from an external checkout (pinned 32d8a381); its build output lands in our build dir.
set(LIE_UE4SS_DIR "$ENV{LIE_UE4SS_DIR}" CACHE PATH "RE-UE4SS source checkout (commit 32d8a381)")
option(LIE_CORE_ONLY "Build only the engine-independent core + tests" OFF)

set(LIE_FETCH_CACHE "$ENV{LIE_FETCH_CACHE}" CACHE PATH "Directory holding <name>-src FetchContent sources")
if(LIE_FETCH_CACHE)
    file(GLOB _lie_cached LIST_DIRECTORIES true "${LIE_FETCH_CACHE}/*-src")
    foreach(_dir ${_lie_cached})
        get_filename_component(_name "${_dir}" NAME)
        string(REGEX REPLACE "-src$" "" _name "${_name}")
        string(TOUPPER "${_name}" _name)
        set(FETCHCONTENT_SOURCE_DIR_${_name} "${_dir}")
    endforeach()
endif()

if(NOT LIE_CORE_ONLY)
    if(NOT EXISTS "${LIE_UE4SS_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "LIE_UE4SS_DIR does not point to a RE-UE4SS checkout: '${LIE_UE4SS_DIR}'")
    endif()
    add_subdirectory("${LIE_UE4SS_DIR}" "${CMAKE_BINARY_DIR}/ue4ss")
endif()

include(CTest)
add_subdirectory(native)
