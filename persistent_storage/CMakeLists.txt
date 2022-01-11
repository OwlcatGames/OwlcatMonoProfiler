cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

# ---------------- Paths ----------------

set( ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. )

# ---------------- Main project properties ----------------

project(persistent_storage CXX C)

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )
set( INCLUDES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/include )

set( ALL_SOURCES 
    ${SOURCES_ROOT}/sqlite3.c
    ${SOURCES_ROOT}/sqlite3.h
    ${SOURCES_ROOT}/persistent_storage.cpp
    ${INCLUDES_ROOT}/persistent_storage.h
)

# ---------------- Targets ----------------

add_library( persistent_storage ${ALL_SOURCES} )

set_property( TARGET persistent_storage PROPERTY CXX_STANDARD 17 )

# ---------------- Includes ----------------

target_include_directories( persistent_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/ )
target_include_directories( persistent_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ )

target_link_libraries( persistent_storage PUBLIC -ldl )
