cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( capture_check CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )

set( ALL_SOURCES
    ${SOURCES_ROOT}/capture_check.cpp
)

# ---------------- Targets ----------------

add_executable( capture_check ${ALL_SOURCES} )
set_property( TARGET capture_check PROPERTY CXX_STANDARD 17 )

target_link_libraries( capture_check PRIVATE owlcat_mono_profiler_client )
# The tool reads the event log directly, which is a client implementation detail
target_include_directories( capture_check PRIVATE ${CMAKE_SOURCE_DIR}/client/src )
