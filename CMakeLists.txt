cmake_minimum_required(VERSION 3.16.0)

function(apply_submodule_patch SUBMODULE_PATH PATCH_PATH)
    get_filename_component(SUBMODULE_NAME "${SUBMODULE_PATH}" NAME)
    get_filename_component(PATCH_NAME "${PATCH_PATH}" NAME)

    if(EXISTS "${PATCH_PATH}" AND EXISTS "${SUBMODULE_PATH}/CMakeLists.txt")
        # Check if already applied using git apply --check --reverse
        execute_process(
            COMMAND git apply --check --reverse "${PATCH_PATH}"
            WORKING_DIRECTORY "${SUBMODULE_PATH}"
            RESULT_VARIABLE IS_APPLIED
            OUTPUT_QUIET ERROR_QUIET
        )
        if(NOT IS_APPLIED EQUAL 0)
            message(NOTICE ">>> [Submodule Patcher] Applying ${PATCH_NAME} to ${SUBMODULE_NAME}...")
            execute_process(
                COMMAND git apply --whitespace=nowarn "${PATCH_PATH}"
                WORKING_DIRECTORY "${SUBMODULE_PATH}"
                RESULT_VARIABLE APPLY_RES
                ERROR_VARIABLE APPLY_ERR
            )
            if(APPLY_RES EQUAL 0)
                message(NOTICE ">>> [Submodule Patcher] Successfully applied ${PATCH_NAME} to ${SUBMODULE_NAME}")
            else()
                message(FATAL_ERROR ">>> [Submodule Patcher] Failed to apply ${PATCH_PATH}: ${APPLY_ERR}")
            endif()
        else()
            message(NOTICE ">>> [Submodule Patcher] ${PATCH_NAME} already applied to ${SUBMODULE_NAME}, skipping.")
        endif()
    else()
        message(FATAL_ERROR ">>> [Submodule Patcher] Patch \"${PATCH_PATH}\" or submodule \"${SUBMODULE_PATH}\" not found!")
    endif()
endfunction()

# Apply esp-nimble-cpp whitelist patch
apply_submodule_patch(
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-nimble-cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/esp-nimble-whitelist.patch"
)

list(APPEND EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3xy-bridge)

