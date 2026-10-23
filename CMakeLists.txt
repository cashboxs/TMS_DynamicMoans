cmake_minimum_required(VERSION 3.21)
project(TMS_DynamicMoans VERSION 1.0.1 LANGUAGES CXX)
find_package(CommonLibSSE CONFIG REQUIRED)
set(SHARED "${CMAKE_CURRENT_SOURCE_DIR}/SharedMoan")
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    src/plugin.cpp src/Logger.cpp src/RuntimePaths.cpp src/core/RuntimeHelpers.cpp
    src/MoanWavDuration.cpp src/MoanMenu.cpp
    "${SHARED}/NativeMoanReplace.cpp"
    "${SHARED}/OStimMoanHook.cpp"
    "${SHARED}/OStimMoanAutoLocator.cpp"
    "${SHARED}/PPAEnhancement.cpp"
    "${SHARED}/MoanHookOwner.cpp"
    "${SHARED}/NativeMoanReplace.h"
    "${SHARED}/OStimMoanHook.h"
    "${SHARED}/OStimMoanAutoLocator.h"
    "${SHARED}/MoanHookOwner.h"
)

target_link_libraries(${PROJECT_NAME} PRIVATE version)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE TMS_MOAN_STANDALONE=1)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SHARED}")
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(DLL_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}/SKSE/Plugins")
elseif(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(DLL_FOLDER "$ENV{SKYRIM_FOLDER}/Data/SKSE/Plugins")
endif()
if(DEFINED DLL_FOLDER)
    add_custom_command(TARGET "${PROJECT_NAME}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM)
endif()
