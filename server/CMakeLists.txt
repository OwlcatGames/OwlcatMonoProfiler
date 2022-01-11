cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_server CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )
set( INCLUDES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/include )

set( ALL_SOURCES         
    ${SOURCES_ROOT}/mono_functions.h    
    ${SOURCES_ROOT}/mono_profiler.h
    ${SOURCES_ROOT}/mono_profiler.cpp
    ${INCLUDES_ROOT}/mono_profiler_server.h
    ${SOURCES_ROOT}/mono_profiler_server.cpp
    ${SOURCES_ROOT}/functor.h
    ${SOURCES_ROOT}/load_library.h
    ${SOURCES_ROOT}/load_library.cpp
    ${SOURCES_ROOT}/worker_thread.h
    ${SOURCES_ROOT}/worker_thread.cpp
)

if (WIN32)
    list( APPEND ALL_SOURCES ${INCLUDES_ROOT}/dll_exports.h ${SOURCES_ROOT}/dll_main.cpp )
endif()

set( ALL_SOURCES_MONO ${ALL_SOURCES} )
list( APPEND ALL_SOURCES_MONO ${SOURCES_ROOT}/mono_functions_mono.cpp )
set( ALL_SOURCES_IL2CPP ${ALL_SOURCES} )
list( APPEND ALL_SOURCES_IL2CPP ${SOURCES_ROOT}/mono_functions_il2cpp.cpp )

# ---------------- Targets ----------------

add_library( mono_profiler_mono SHARED ${ALL_SOURCES_MONO} )
set_property( TARGET mono_profiler_mono PROPERTY CXX_STANDARD 17 )

target_include_directories( mono_profiler_mono PUBLIC ${INCLUDES_ROOT} )
target_include_directories( mono_profiler_mono PRIVATE ${SOURCES_ROOT} ${MONO_HEADERS} )

target_link_libraries( mono_profiler_mono PUBLIC owlcat_mono_profiler_network owlcat_mono_profiler_logger )

target_compile_definitions( mono_profiler_mono PRIVATE -D MONO_PROFILER_UNSTABLE_GC_ROOTS -D OWLCAT_MONO )

if(WIN32)
    add_dependencies(mono_profiler_mono detours)
    target_include_directories( mono_profiler_mono PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/include )
    target_link_libraries( mono_profiler_mono PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/lib.X64/detours.lib )
    target_compile_definitions( mono_profiler_mono PRIVATE -D _AMD64_ )
else()
    target_link_libraries( mono_profiler_mono PRIVATE dl )
endif()



add_library( mono_profiler_il2cpp SHARED ${ALL_SOURCES_IL2CPP} )
set_property( TARGET mono_profiler_il2cpp PROPERTY CXX_STANDARD 17 )

target_include_directories( mono_profiler_il2cpp PUBLIC ${INCLUDES_ROOT} )
target_include_directories( mono_profiler_il2cpp PRIVATE ${SOURCES_ROOT} ${MONO_HEADERS} )

target_link_libraries( mono_profiler_il2cpp PUBLIC owlcat_mono_profiler_network owlcat_mono_profiler_logger )

target_compile_definitions( mono_profiler_il2cpp PRIVATE -D MONO_PROFILER_UNSTABLE_GC_ROOTS -D OWLCAT_IL2CPP )

if(WIN32)
    add_dependencies(mono_profiler_il2cpp detours)
    target_include_directories( mono_profiler_il2cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/include )
    target_link_libraries( mono_profiler_il2cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/detours/lib.X64/detours.lib )
    target_compile_definitions( mono_profiler_il2cpp PRIVATE -D _AMD64_ )
else()
    target_link_libraries( mono_profiler_il2cpp PRIVATE dl )
endif()
