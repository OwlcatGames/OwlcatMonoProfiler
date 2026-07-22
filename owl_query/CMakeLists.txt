cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owl_query CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )

set( ALL_SOURCES
    ${SOURCES_ROOT}/owl_query.cpp
)

# ---------------- Targets ----------------

add_executable( owl_query ${ALL_SOURCES} )
set_property( TARGET owl_query PROPERTY CXX_STANDARD 17 )

target_link_libraries( owl_query PRIVATE owlcat_mono_profiler_client )
