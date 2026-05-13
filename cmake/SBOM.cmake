# Software Bill of Materials (SBOM) Generation for DineroCoin
# Generates SPDX-compliant SBOM for supply chain security

# Find required tools
find_program(SYFT_EXECUTABLE syft)
find_program(CYCLONEDX_EXECUTABLE cyclonedx-bom)

# Function to generate SBOM
function(generate_sbom TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Configuring SBOM generation for ${TARGET_NAME}")
        
        set(SBOM_OUTPUT_DIR "${CMAKE_BINARY_DIR}/sbom")
        file(MAKE_DIRECTORY "${SBOM_OUTPUT_DIR}")
        
        # Generate SPDX SBOM
        if(SYFT_EXECUTABLE)
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${SYFT_EXECUTABLE} packages 
                    "$<TARGET_FILE:${TARGET_NAME}>" 
                    -o spdx-json="${SBOM_OUTPUT_DIR}/${TARGET_NAME}-sbom.spdx.json"
                COMMENT "Generating SPDX SBOM for ${TARGET_NAME}"
                VERBATIM
            )
            
            # Also generate human-readable format
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${SYFT_EXECUTABLE} packages 
                    "$<TARGET_FILE:${TARGET_NAME}>" 
                    -o table="${SBOM_OUTPUT_DIR}/${TARGET_NAME}-sbom.txt"
                COMMENT "Generating text SBOM for ${TARGET_NAME}"
                VERBATIM
            )
        else()
            message(WARNING "syft not found - SBOM generation disabled")
        endif()
        
        # Generate CycloneDX SBOM if available
        if(CYCLONEDX_EXECUTABLE)
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CYCLONEDX_EXECUTABLE} 
                    --input-file "$<TARGET_FILE:${TARGET_NAME}>"
                    --output-file "${SBOM_OUTPUT_DIR}/${TARGET_NAME}-cyclonedx.json"
                    --output-format json
                COMMENT "Generating CycloneDX SBOM for ${TARGET_NAME}"
                VERBATIM
            )
        endif()
        
        # Create manifest with build metadata
        set(MANIFEST_FILE "${SBOM_OUTPUT_DIR}/${TARGET_NAME}-manifest.json")
        
        # Get git information
        execute_process(
            COMMAND git rev-parse HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        execute_process(
            COMMAND git describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        # Generate build manifest
        file(WRITE "${MANIFEST_FILE}" "{\n")
        file(APPEND "${MANIFEST_FILE}" "  \"name\": \"${TARGET_NAME}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"version\": \"${PROJECT_VERSION}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"build_timestamp\": \"$ENV{SOURCE_DATE_EPOCH}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"git_commit\": \"${GIT_COMMIT_HASH}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"git_describe\": \"${GIT_DESCRIBE}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"cmake_version\": \"${CMAKE_VERSION}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"compiler\": \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"build_type\": \"${CMAKE_BUILD_TYPE}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"system\": \"${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}\",\n")
        file(APPEND "${MANIFEST_FILE}" "  \"architecture\": \"${CMAKE_SYSTEM_PROCESSOR}\"\n")
        file(APPEND "${MANIFEST_FILE}" "}\n")
        
        message(STATUS "SBOM will be generated in: ${SBOM_OUTPUT_DIR}")
    endif()
endfunction()

# Function to verify SBOM integrity
function(verify_sbom TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND SYFT_EXECUTABLE)
        set(SBOM_FILE "${CMAKE_BINARY_DIR}/sbom/${TARGET_NAME}-sbom.spdx.json")
        
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Verifying SBOM integrity..."
            COMMAND test -f "${SBOM_FILE}" || (echo "SBOM file missing" && exit 1)
            COMMAND python3 -c "import json; json.load(open('${SBOM_FILE}'))" || (echo "Invalid SBOM JSON" && exit 1)
            COMMENT "Verifying SBOM for ${TARGET_NAME}"
            VERBATIM
        )
    endif()
endfunction()

# Installation rules for SBOM files
function(install_sbom TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        install(
            DIRECTORY "${CMAKE_BINARY_DIR}/sbom/"
            DESTINATION "share/dinero/sbom"
            COMPONENT sbom
            OPTIONAL
        )
    endif()
endfunction()
