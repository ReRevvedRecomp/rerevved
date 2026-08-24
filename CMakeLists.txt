cmake_minimum_required(VERSION 3.25)
project(rerevved VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.9.0 REQUIRED CONFIG)

# Consumed by the codegen-emitted dll_targets.cmake to colocate guest DLLs.
set(REXGLUE_HOST_TARGET rerevved)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    include(generated/default/sources.cmake)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake")
    include(generated/default/dll_targets.cmake)
endif()

add_custom_target(rerevved_codegen
    COMMAND $<TARGET_FILE:rex::rexglue> codegen ${CMAKE_CURRENT_SOURCE_DIR}/rerevved_manifest.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating recompiled code for rerevved"
    VERBATIM
)

set(REREVVED_SOURCES
    src/app.cpp
    src/compat_hooks.cpp
    src/game_content.cpp
    src/game_state.cpp
    src/game_state.h
    src/imports.cpp
    src/main.cpp
    src/presence.cpp
    src/presence.h
    src/presence_model.cpp
    src/presence_model.h
)

if(WIN32)
    set(REREVVED_RC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/rerevved.ico")
    configure_file(res/rerevved.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc" @ONLY)
    list(APPEND REREVVED_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/rerevved.rc")
endif()

if(WIN32)
    add_executable(rerevved WIN32 ${REREVVED_SOURCES})
else()
    add_executable(rerevved ${REREVVED_SOURCES})
endif()

target_sources(rerevved PRIVATE ${GENERATED_SOURCES})
target_include_directories(rerevved PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
)
target_compile_definitions(rerevved PRIVATE
    REREVVED_GAMEPLAY_API_EXPORTS
    REREVVED_VERSION="${PROJECT_VERSION}"
)
if(UNIX)
    target_link_options(rerevved PRIVATE -Wl,--export-dynamic)
endif()
target_link_libraries(rerevved PRIVATE rex::runtime)
rexglue_configure_target(rerevved GPU_PLUGINS xenos)

include(CTest)
if(BUILD_TESTING)
    add_executable(presence_model_tests
        src/presence_model.cpp
        tests/presence_model_test.cpp
    )
    target_include_directories(presence_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/api
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME presence_model COMMAND presence_model_tests)
endif()
