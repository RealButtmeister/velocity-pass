cmake_minimum_required(VERSION 3.25)
project(VelocityPassPair VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Velocity Pass currently supports Windows x64 only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VST3_SDK_ROOT "" CACHE PATH "Path to a recursive Steinberg VST3 SDK checkout")
if(NOT EXISTS "${VST3_SDK_ROOT}/public.sdk/source/vst/vstaudioeffect.h")
    message(FATAL_ERROR "Set VST3_SDK_ROOT to the Steinberg VST3 SDK with its submodules initialized.")
endif()

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
add_subdirectory("${VST3_SDK_ROOT}" sdk)

if(MINGW)
    # This unused SDK utility requires MSVC-specific support. Neither plugin
    # uses the SDK data-exchange helpers; preserve the original MinGW build.
    get_target_property(sdk_sources sdk SOURCES)
    list(FILTER sdk_sources EXCLUDE REGEX "/utility/dataexchange\\.cpp$")
    set_property(TARGET sdk PROPERTY SOURCES "${sdk_sources}")
    if(TARGET validator)
        target_link_options(validator PRIVATE -municode -static)
    endif()
endif()

# SDK plug-in helpers use this variable in the caller's directory scope.
set(public_sdk_SOURCE_DIR "${VST3_SDK_ROOT}/public.sdk")

function(add_velocity_plugin target source)
    smtg_add_vst3plugin(${target} ${source})
    target_include_directories(${target} PRIVATE "${VST3_SDK_ROOT}")
    target_link_libraries(${target} PRIVATE sdk user32 gdi32)
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
        set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/${target}.vst3/Contents/x86_64-win")
    endif()
endfunction()

add_velocity_plugin(VelocityPass VelocityPass/Source/VelocityPass.cpp)
add_velocity_plugin(VelocityPassInverse VelocityPassInverse/Source/VelocityPassInverse.cpp)

include(CTest)
if(BUILD_TESTING)
    function(add_velocity_tests target source)
        add_executable(${target} ${source}
            "${VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp")
        target_include_directories(${target} PRIVATE "${VST3_SDK_ROOT}")
        target_link_libraries(${target} PRIVATE sdk sdk_hosting user32)
        if(MINGW)
            target_link_options(${target} PRIVATE -static)
        endif()
    endfunction()

    add_velocity_tests(velocity_pass_tests VelocityPass/Tests/PluginTests.cpp)
    add_velocity_tests(velocity_pass_inverse_tests VelocityPassInverse/Tests/PluginTests.cpp)
    add_dependencies(velocity_pass_tests VelocityPass)
    add_dependencies(velocity_pass_inverse_tests VelocityPassInverse VelocityPass)
    add_test(NAME velocity_pass_binary_tests
        COMMAND velocity_pass_tests "$<TARGET_FILE:VelocityPass>")
    add_test(NAME velocity_pass_inverse_binary_tests
        COMMAND velocity_pass_inverse_tests "$<TARGET_FILE:VelocityPassInverse>" "$<TARGET_FILE:VelocityPass>")
endif()
