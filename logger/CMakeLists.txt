cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_logger CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )
set( INCLUDES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/include )

set( ALL_SOURCES         
    ${INCLUDES_ROOT}/logger.h
    ${SOURCES_ROOT}/logger.cpp
    ${SOURCES_ROOT}/sink_file.cpp
)

# ---------------- Targets ----------------
add_library( owlcat_mono_profiler_logger STATIC ${ALL_SOURCES} )
set_property( TARGET owlcat_mono_profiler_logger PROPERTY CXX_STANDARD 17 )

target_include_directories( owlcat_mono_profiler_logger PUBLIC ${INCLUDES_ROOT} )
