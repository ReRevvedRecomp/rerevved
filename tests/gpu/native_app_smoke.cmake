# The bounded lifecycle smoke and its CTest registrations. Included from
# tests/gpu/render_tests.cmake inside its BUILD_TESTING block.

if(WIN32 AND NOT REREVVED_TESTS_ONLY)
    # The bounded lifecycle smoke drives the real presentation session on a
    # hidden window; it does not start the title or its guest.
    add_executable(rerevved_native_app_smoke
        tests/gpu/native_app_smoke.cpp
        src/gpu/present/native_app_session.cpp
        src/gpu/present/native_immediate_drawer.cpp
        src/gpu/present/native_presentation.cpp
        src/gpu/present/plume_host.cpp
        tools/presentation_fixture.cpp
    )
    if(REREVVED_DEVELOPER)
        target_sources(rerevved_native_app_smoke PRIVATE src/gpu/present/native_cadence_trace.cpp)
    endif()
    target_include_directories(rerevved_native_app_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(rerevved_native_app_smoke PRIVATE
        rerevved_native_fixture_core
        rex::runtime
        d3d12 dxgi dxguid d3dcompiler user32 plume
    )
    add_dependencies(rerevved_native_app_smoke rerevved_present_shaders)
    rexglue_apply_target_settings(rerevved_native_app_smoke)
    add_custom_command(TARGET rerevved_native_app_smoke POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:rerevved_native_app_smoke>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:rerevved_native_app_smoke>;$<TARGET_FILE_DIR:rerevved_native_app_smoke>>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    add_test(NAME native_app_hidden
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/native_app_harness.py
            smoke $<TARGET_FILE:rerevved_native_app_smoke> ${CMAKE_CURRENT_BINARY_DIR}/native-app-harness)
    set_tests_properties(native_app_hidden PROPERTIES
        SKIP_RETURN_CODE 77 RESOURCE_LOCK native_gpu LABELS release-smoke)
    if(REREVVED_NATIVE_PRESENT_CATALOG)
        set_tests_properties(native_app_hidden PROPERTIES
            ENVIRONMENT "REREVVED_NATIVE_PRESENT_CATALOG=${REREVVED_NATIVE_PRESENT_CATALOG}")
    endif()
    # The developer executable keeps the full-app lifecycle trace check,
    # which also exercises the SDK settings overlay and detached input.
    if(REREVVED_DEVELOPER)
        add_test(NAME native_app_hidden_trace
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/native_app_harness.py
                app $<TARGET_FILE:rerevved> ${CMAKE_CURRENT_BINARY_DIR}/native-app-trace-harness)
        set_tests_properties(native_app_hidden_trace PROPERTIES
            SKIP_RETURN_CODE 77 RESOURCE_LOCK native_gpu)
        if(REREVVED_NATIVE_PRESENT_CATALOG)
            set_tests_properties(native_app_hidden_trace PROPERTIES
                ENVIRONMENT "REREVVED_NATIVE_PRESENT_CATALOG=${REREVVED_NATIVE_PRESENT_CATALOG}")
        endif()
    endif()
endif()
