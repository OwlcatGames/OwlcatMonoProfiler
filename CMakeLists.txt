cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler CXX )
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(WIN32)
    add_definitions( -D _CRT_SECURE_NO_WARNINGS -D NOMINMAX -D WIN32_LEAN_AND_MEAN )
else()
    set( CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ---------------- Variables ----------------


set(MONO_HEADERS "${CMAKE_SOURCE_DIR}/mono_headers" CACHE STRING "Path to Mono installation directory, or a portion of Mono headers")
set(MONO_DLL_PATH "" CACHE STRING "Path to mono-2.0-bdwgc.dll for test")
#set(UNITY_PLUGIN_API_HEADERS "" CACHE STRING "Path to Unity Plugin API headers")

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

set(COMMON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/common")
include_directories(${COMMON_PATH})

# ---------------- Sub-projects ----------------

# Include Detours library as MSVC project on Windows
if (WIN32)
    include_external_msproject( detours ${CMAKE_SOURCE_DIR}/external/detours/vc/Detours.vcxproj )
    set_target_properties(detours PROPERTIES
         MAP_IMPORTED_CONFIG_DEBUG DebugMDd
         MAP_IMPORTED_CONFIG_MINSIZEREL ReleaseMD
         MAP_IMPORTED_CONFIG_RELWITHDEBINFO ReleaseMD
    )
endif()

#add_subdirectory( external/robin-map )
add_subdirectory( network )
add_subdirectory( logger )
add_subdirectory( persistent_storage )
add_subdirectory( server )
add_subdirectory( client )
add_subdirectory( ui )
add_subdirectory( test )

# ---------------- Packaging ----------------

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "Owlcat Mono Profiler")
SET(CPACK_PACKAGE_NAME "Owlcat Mono Profiler")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Owlcat Mono Profiler")
SET(CPACK_PACKAGE_VENDOR "Owlcat Games")
SET(CPACK_PACKAGE_VERSION_MAJOR "0")
SET(CPACK_PACKAGE_VERSION_MINOR "6")
SET(CPACK_PACKAGE_VERSION_PATCH "0")
SET(CPACK_PACKAGE_INSTALL_DIRECTORY "Owlcat Mono Profiler")
SET(CPACK_PACKAGE_EXECUTABLES "owlcat_mono_profiler_ui" "Owlcat Mono Profiler")
SET(CPACK_PACKAGE_FILE_NAME "OGT_Memory_${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}_Win64")
SET( CPACK_PACKAGE_CONTACT "Max Savenkov (savenkov@owlcatgames.com)" )
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 0 )
include(InstallRequiredSystemLibraries)

install(TARGETS owlcat_mono_profiler_ui RUNTIME DESTINATION "Owlcat Mono Profiler" )
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/iconengines" DESTINATION "Owlcat Mono Profiler")
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/imageformats" DESTINATION "Owlcat Mono Profiler")
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/platforms" DESTINATION "Owlcat Mono Profiler")
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/styles" DESTINATION "Owlcat Mono Profiler")
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/translations" DESTINATION "Owlcat Mono Profiler")
install(DIRECTORY "$<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/" DESTINATION "Owlcat Mono Profiler" FILES_MATCHING PATTERN "*.dll")

set(CPACK_GENERATOR "ZIP")

include(CPack)
