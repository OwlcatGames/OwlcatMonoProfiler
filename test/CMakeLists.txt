cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_test CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )

if ("${MONO_DLL_PATH}" STREQUAL "")
	message(FATAL_ERROR "MONO_DLL_PATH not set. Test won't be run")
endif()

if (NOT EXISTS "${MONO_DLL_PATH}")
	message(FATAL_ERROR "MONO_DLL_PATH does not points to an existing file")
endif()

file(TO_CMAKE_PATH "${MONO_DLL_PATH}" MONO_DLL_PATH)

configure_file( ${SOURCES_ROOT}/mono_profiler_test.h.cmake ${PROJECT_BINARY_DIR}/mono_profiler_test.h )

set( ALL_SOURCES         
    ${SOURCES_ROOT}/mono_profiler_test.cpp
	${PROJECT_BINARY_DIR}/mono_profiler_test.h
)

# ---------------- Targets ----------------

if (WIN32)
    set_source_files_properties( copy_dll PROPERTIES SYMBOLIC TRUE)
    set_source_files_properties( copy_pdb PROPERTIES SYMBOLIC TRUE)

    add_custom_command(
        OUTPUT copy_dll
        DEPENDS mono_profiler_mono
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:mono_profiler_mono>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_test>/$<TARGET_FILE_NAME:mono_profiler_mono>
    )

    add_custom_command(
        OUTPUT copy_pdb
        DEPENDS mono_profiler_mono
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_PDB_FILE:mono_profiler_mono>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_test>/$<TARGET_PDB_FILE_NAME:mono_profiler_mono>
    )

    add_executable( owlcat_mono_profiler_test ${ALL_SOURCES} copy_dll copy_pdb )
else()
    add_executable( owlcat_mono_profiler_test ${ALL_SOURCES} )
endif()

set_property( TARGET owlcat_mono_profiler_test PROPERTY CXX_STANDARD 17 )
target_link_libraries( owlcat_mono_profiler_test PRIVATE mono_profiler_mono owlcat_mono_profiler_client )
target_include_directories( owlcat_mono_profiler_test PRIVATE ${PROJECT_BINARY_DIR} )
