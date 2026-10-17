# core: engine-independent contract (header-only), unit-tested offline
add_library(lie_core INTERFACE)
target_include_directories(lie_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lie_core INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

if(BUILD_TESTING)
    add_executable(lie_tests tests/test_core.cpp)
    target_compile_options(lie_tests PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(lie_tests PRIVATE LIE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(lie_tests PRIVATE lie_core)
    set_target_properties(lie_tests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_test(NAME lie_tests COMMAND lie_tests)
endif()

if(LIE_CORE_ONLY)
    return()
endif()

# rt: Windows / UE4SS runtime shared by both mods
add_library(lie_rt STATIC
    rt/shm.cpp
    rt/hook.cpp
    rt/binary.cpp
    rt/ue.cpp
    rt/input.cpp
    rt/d3d.cpp
)
target_include_directories(lie_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lie_rt PRIVATE /utf-8)
target_link_libraries(lie_rt PUBLIC lie_core UE4SS bcrypt d3d11 dxgi)
set_target_properties(lie_rt PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

function(lie_add_mod target dir)
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
    if(NOT sources)
        return()
    endif()
    add_library(${target} SHARED ${sources})
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_libraries(${target} PRIVATE lie_rt)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "main"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods/${target}/dlls"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods/${target}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods/${target}/dlls"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endfunction()

lie_add_mod(LiliesGuest guest)
lie_add_mod(MagnoliaHost host)
if(TARGET MagnoliaHost)
    target_link_libraries(MagnoliaHost PRIVATE d3dcompiler user32)
endif()
