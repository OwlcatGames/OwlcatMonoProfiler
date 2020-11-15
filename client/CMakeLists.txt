cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_client CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )
set( INCLUDES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/include )

set( ALL_SOURCES         
    ${INCLUDES_ROOT}/mono_profiler_client.h
    ${SOURCES_ROOT}/mono_profiler_client.cpp
    ${SOURCES_ROOT}/db_queries.h
    ${SOURCES_ROOT}/db_queries.cpp
    ${SOURCES_ROOT}/db_migrations.h
    ${SOURCES_ROOT}/db_migrations.cpp
)

# ---------------- Targets ----------------

add_library( owlcat_mono_profiler_client STATIC ${ALL_SOURCES} )
set_property( TARGET owlcat_mono_profiler_client PROPERTY CXX_STANDARD 17 )

target_include_directories( owlcat_mono_profiler_client PRIVATE ${SOURCES_ROOT} ${SQLITE_PATH} )
target_include_directories( owlcat_mono_profiler_client PUBLIC ${INCLUDES_ROOT} )

target_link_libraries( owlcat_mono_profiler_client PUBLIC owlcat_mono_profiler_network persistent_storage )

if(WIN32)
    #target_link_libraries( owlcat_mono_profiler_client PRIVATE detours )
    add_dependencies( owlcat_mono_profiler_client detours )
    target_include_directories( owlcat_mono_profiler_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/include )
    target_link_libraries( owlcat_mono_profiler_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/lib.X64/detours.lib )
    target_compile_definitions( owlcat_mono_profiler_client PRIVATE -D _AMD64_ )
endif()
