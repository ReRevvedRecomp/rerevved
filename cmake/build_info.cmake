function(rerevved_write_build_info out_header)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE git_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ls-files
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE git_tracked_files
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE ";" "\\;" git_tracked_files "${git_tracked_files}")
    string(REPLACE "\n" ";" git_tracked_files "${git_tracked_files}")

    foreach(dependency HEAD index)
        if(EXISTS "${git_dir}/${dependency}")
            set_property(DIRECTORY APPEND PROPERTY
                CMAKE_CONFIGURE_DEPENDS "${git_dir}/${dependency}")
        endif()
    endforeach()
    foreach(path IN LISTS git_tracked_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
    endforeach()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE REREVVED_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

    if(git_status)
        set(REREVVED_GIT_DIRTY dirty)
    else()
        set(REREVVED_GIT_DIRTY clean)
    endif()

    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/build_info.h.in"
        "${out_header}" @ONLY)
endfunction()
