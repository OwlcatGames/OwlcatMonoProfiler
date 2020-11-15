cmake_minimum_required(VERSION 3.14)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_network CXX )

# ---------------- ASIO ----------------

include(FetchContent)

FetchContent_Declare(asio-cmake
  GIT_REPOSITORY https://github.com/kingsamchen/asio-cmake.git
  GIT_TAG        origin/master
)

# Specify asio version
set(ASIO_CMAKE_ASIO_TAG asio-1-18-0)
FetchContent_MakeAvailable(asio-cmake)

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )
set( INCLUDES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/include )

set( ALL_SOURCES         
    ${INCLUDES_ROOT}/network.h
    ${SOURCES_ROOT}/network.cpp
)

# ---------------- Targets ----------------

add_library( owlcat_mono_profiler_network STATIC ${ALL_SOURCES} )
set_property( TARGET owlcat_mono_profiler_network PROPERTY CXX_STANDARD 17 )

target_include_directories( owlcat_mono_profiler_network PRIVATE ${SOURCES_ROOT})
target_include_directories( owlcat_mono_profiler_network PUBLIC ${INCLUDES_ROOT})

target_link_libraries( owlcat_mono_profiler_network PRIVATE asio )