cmake_minimum_required(VERSION 3.10)

# ---------------- Main project properties ----------------

project( owlcat_mono_profiler_ui CXX )

# ---------------- Sources ----------------

set( SOURCES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src )

set( ALL_SOURCES         
    ${SOURCES_ROOT}/mono_profiler_ui.cpp
	${SOURCES_ROOT}/mono_profiler_ui.h
	${SOURCES_ROOT}/main_window.h
    ${SOURCES_ROOT}/main_window.cpp
	${SOURCES_ROOT}/connect_dialog.h
    ${SOURCES_ROOT}/connect_dialog.cpp
	${SOURCES_ROOT}/run_dialog.h
    ${SOURCES_ROOT}/run_dialog.cpp
	${SOURCES_ROOT}/mainwindow.ui
	${SOURCES_ROOT}/connectdialog.ui
    ${SOURCES_ROOT}/rundialog.ui
    ${SOURCES_ROOT}/band_picker.cpp
    ${SOURCES_ROOT}/band_picker.h
    ${SOURCES_ROOT}/byte_scale_draw.cpp
    ${SOURCES_ROOT}/byte_scale_draw.h
    ${SOURCES_ROOT}/graphs_data.cpp
    ${SOURCES_ROOT}/graphs_data.h
    ${SOURCES_ROOT}/live_objects_data.cpp
    ${SOURCES_ROOT}/live_objects_data.h
    ${SOURCES_ROOT}/search_results_data.cpp
    ${SOURCES_ROOT}/search_results_data.h
    ${SOURCES_ROOT}/object_references_model.cpp
    ${SOURCES_ROOT}/object_references_model.h
    ${SOURCES_ROOT}/common_ui.h
)

# ---------------- Qt & Qwt ----------------

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qwt REQUIRED)

# For Windows builds, we'd like to copy DLL and PDB for Qwt to output folder, or the application
# will not be able to run
if (WIN32)
    set_source_files_properties( copy_qwt_dll PROPERTIES SYMBOLIC TRUE)
    set_source_files_properties( copy_qwt_pdb PROPERTIES SYMBOLIC TRUE)

    add_custom_command(
        OUTPUT copy_qwt_dll
        COMMAND ${CMAKE_COMMAND} -E copy
            ${QWT_LIBRARY_DIR}/$<IF:$<CONFIG:Debug>,qwtd.dll,qwt.dll>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_ui>
    )    

    # There is no PDB for Qwt's Release build, but we can't easily prevent the command from running
    add_custom_command(
        OUTPUT copy_qwt_pdb
        COMMAND ${CMAKE_COMMAND} -E copy            
            ${QWT_LIBRARY_DIR}/$<IF:$<CONFIG:Debug>,qwtd.pdb,qwtd.pdb>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_ui>
    )

    # Detour need a server DLL to use, let's provide one in output folder
    add_custom_command(
        OUTPUT copy_server_dll
        DEPENDS mono_profiler_mono
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:mono_profiler_mono>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_ui>
    )

    # Detour need a server DLL to use, let's provide one in output folder
    add_custom_command(
        OUTPUT copy_server_dll_il2cpp
        DEPENDS mono_profiler_il2cpp
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:mono_profiler_il2cpp>
            $<TARGET_FILE_DIR:owlcat_mono_profiler_ui>
    )
endif()

# ---------------- Targets ----------------

if (WIN32)
	add_executable( owlcat_mono_profiler_ui WIN32 ${ALL_SOURCES} copy_qwt_dll copy_qwt_pdb copy_server_dll copy_server_dll_il2cpp )
else()
    add_executable( owlcat_mono_profiler_ui ${ALL_SOURCES} )
endif()

set_property( TARGET owlcat_mono_profiler_ui PROPERTY CXX_STANDARD 17 )

if (WIN32)
    target_link_libraries( owlcat_mono_profiler_ui PRIVATE owlcat_mono_profiler_client Qt5::Widgets debug Qwt::Qwtd optimized Qwt::Qwt)
else()
    # Linux don't have debug versions of libraries installed usually
    target_link_libraries( owlcat_mono_profiler_ui PRIVATE -ldl owlcat_mono_profiler_client Qt5::Widgets Qwt::Qwt)
endif()



# Copy Qt DLLs to ouput folder
if (WIN32)
    add_custom_command(
        TARGET owlcat_mono_profiler_ui
        POST_BUILD
        COMMAND ${Qt5_DIR}/../../../bin/windeployqt.exe $<TARGET_FILE_DIR:owlcat_mono_profiler_ui>/$<TARGET_FILE_NAME:owlcat_mono_profiler_ui> -opengl -angle --no-opengl-sw
    )
endif()
