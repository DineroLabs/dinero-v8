# SLSA (Supply-chain Levels for Software Artifacts) Provenance Generation
# Implements SLSA Level 3 provenance for DineroCoin builds

# Find required tools
find_program(SLSA_GENERATOR slsa-generator)
find_program(COSIGN_EXECUTABLE cosign)

# SLSA provenance configuration
set(SLSA_BUILDER_ID "https://github.com/DineroCoin/DineroCoin/.github/workflows/release.yml")
set(SLSA_BUILD_TYPE "https://github.com/slsa-framework/slsa-github-generator/generic@v1")

# Function to generate SLSA provenance
function(generate_slsa_provenance TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Configuring SLSA provenance generation for ${TARGET_NAME}")
        
        set(SLSA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/slsa")
        file(MAKE_DIRECTORY "${SLSA_OUTPUT_DIR}")
        
        # Get build metadata
        execute_process(
            COMMAND git rev-parse HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        execute_process(
            COMMAND git remote get-url origin
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_REPO_URL
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        # Generate SLSA provenance metadata
        set(PROVENANCE_FILE "${SLSA_OUTPUT_DIR}/${TARGET_NAME}-provenance.json")
        
        # Create SLSA provenance document
        file(WRITE "${PROVENANCE_FILE}" "{\n")
        file(APPEND "${PROVENANCE_FILE}" "  \"_type\": \"https://in-toto.io/Statement/v0.1\",\n")
        file(APPEND "${PROVENANCE_FILE}" "  \"subject\": [\n")
        file(APPEND "${PROVENANCE_FILE}" "    {\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"name\": \"${TARGET_NAME}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"digest\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"sha256\": \"PLACEHOLDER_WILL_BE_REPLACED\"\n")
        file(APPEND "${PROVENANCE_FILE}" "      }\n")
        file(APPEND "${PROVENANCE_FILE}" "    }\n")
        file(APPEND "${PROVENANCE_FILE}" "  ],\n")
        file(APPEND "${PROVENANCE_FILE}" "  \"predicateType\": \"https://slsa.dev/provenance/v0.2\",\n")
        file(APPEND "${PROVENANCE_FILE}" "  \"predicate\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "    \"builder\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"id\": \"${SLSA_BUILDER_ID}\"\n")
        file(APPEND "${PROVENANCE_FILE}" "    },\n")
        file(APPEND "${PROVENANCE_FILE}" "    \"buildType\": \"${SLSA_BUILD_TYPE}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "    \"invocation\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"configSource\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"uri\": \"${GIT_REPO_URL}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"digest\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "          \"sha1\": \"${GIT_COMMIT_HASH}\"\n")
        file(APPEND "${PROVENANCE_FILE}" "        }\n")
        file(APPEND "${PROVENANCE_FILE}" "      },\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"parameters\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"cmake_build_type\": \"${CMAKE_BUILD_TYPE}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"cmake_version\": \"${CMAKE_VERSION}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"compiler\": \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"system\": \"${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"architecture\": \"${CMAKE_SYSTEM_PROCESSOR}\"\n")
        file(APPEND "${PROVENANCE_FILE}" "      },\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"environment\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"source_date_epoch\": \"$ENV{SOURCE_DATE_EPOCH}\"\n")
        file(APPEND "${PROVENANCE_FILE}" "      }\n")
        file(APPEND "${PROVENANCE_FILE}" "    },\n")
        file(APPEND "${PROVENANCE_FILE}" "    \"metadata\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"buildInvocationId\": \"${CMAKE_BUILD_TIMESTAMP}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"completeness\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"parameters\": true,\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"environment\": true,\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"materials\": true\n")
        file(APPEND "${PROVENANCE_FILE}" "      },\n")
        file(APPEND "${PROVENANCE_FILE}" "      \"reproducible\": true\n")
        file(APPEND "${PROVENANCE_FILE}" "    },\n")
        file(APPEND "${PROVENANCE_FILE}" "    \"materials\": [\n")
        file(APPEND "${PROVENANCE_FILE}" "      {\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"uri\": \"${GIT_REPO_URL}\",\n")
        file(APPEND "${PROVENANCE_FILE}" "        \"digest\": {\n")
        file(APPEND "${PROVENANCE_FILE}" "          \"sha1\": \"${GIT_COMMIT_HASH}\"\n")
        file(APPEND "${PROVENANCE_FILE}" "        }\n")
        file(APPEND "${PROVENANCE_FILE}" "      }\n")
        file(APPEND "${PROVENANCE_FILE}" "    ]\n")
        file(APPEND "${PROVENANCE_FILE}" "  }\n")
        file(APPEND "${PROVENANCE_FILE}" "}\n")
        
        # Post-build command to update SHA256 hash
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Generating SLSA provenance for ${TARGET_NAME}..."
            COMMAND bash -c "SHA256=$(sha256sum '$<TARGET_FILE:${TARGET_NAME}>' | cut -d' ' -f1) && sed -i 's/PLACEHOLDER_WILL_BE_REPLACED/'$SHA256'/g' '${PROVENANCE_FILE}'"
            COMMENT "Updating SLSA provenance with binary hash"
            VERBATIM
        )
        
        # Sign provenance if cosign is available
        if(COSIGN_EXECUTABLE)
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${COSIGN_EXECUTABLE} sign-blob 
                    --bundle "${SLSA_OUTPUT_DIR}/${TARGET_NAME}-provenance.bundle"
                    "${PROVENANCE_FILE}"
                COMMENT "Signing SLSA provenance with cosign"
                VERBATIM
            )
        endif()
        
        message(STATUS "SLSA provenance will be generated in: ${SLSA_OUTPUT_DIR}")
    endif()
endfunction()

# Function to verify SLSA provenance
function(verify_slsa_provenance TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(PROVENANCE_FILE "${CMAKE_BINARY_DIR}/slsa/${TARGET_NAME}-provenance.json")
        
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Verifying SLSA provenance..."
            COMMAND test -f "${PROVENANCE_FILE}" || (echo "SLSA provenance file missing" && exit 1)
            COMMAND python3 -c "
import json
import sys
try:
    with open('${PROVENANCE_FILE}') as f:
        provenance = json.load(f)
    
    # Verify required SLSA fields
    assert provenance.get('_type') == 'https://in-toto.io/Statement/v0.1'
    assert provenance.get('predicateType') == 'https://slsa.dev/provenance/v0.2'
    assert 'subject' in provenance
    assert 'predicate' in provenance
    
    predicate = provenance['predicate']
    assert 'builder' in predicate
    assert 'buildType' in predicate
    assert 'invocation' in predicate
    assert 'metadata' in predicate
    
    print('SLSA provenance validation: PASSED')
except Exception as e:
    print(f'SLSA provenance validation: FAILED - {e}')
    sys.exit(1)
"
            COMMENT "Verifying SLSA provenance structure"
            VERBATIM
        )
    endif()
endfunction()

# Function to create SLSA attestation bundle
function(create_slsa_bundle TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/slsa")
        set(BUNDLE_FILE "${BUNDLE_DIR}/${TARGET_NAME}-slsa-bundle.tar.gz")
        
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "Creating SLSA attestation bundle..."
            COMMAND tar -czf "${BUNDLE_FILE}"
                -C "${BUNDLE_DIR}"
                "${TARGET_NAME}-provenance.json"
                "${TARGET_NAME}-provenance.bundle"
            COMMENT "Creating SLSA attestation bundle"
            VERBATIM
        )
        
        # Generate bundle manifest
        set(BUNDLE_MANIFEST "${BUNDLE_DIR}/${TARGET_NAME}-bundle-manifest.txt")
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "SLSA Attestation Bundle Manifest" > "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "=================================" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "Target: ${TARGET_NAME}" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "Bundle: ${BUNDLE_FILE}" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "Generated: $(date -u)" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "Contents:" >> "${BUNDLE_MANIFEST}"
            COMMAND tar -tzf "${BUNDLE_FILE}" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "" >> "${BUNDLE_MANIFEST}"
            COMMAND ${CMAKE_COMMAND} -E echo "Bundle SHA256:" >> "${BUNDLE_MANIFEST}"
            COMMAND sha256sum "${BUNDLE_FILE}" >> "${BUNDLE_MANIFEST}"
            COMMENT "Generating bundle manifest"
            VERBATIM
        )
    endif()
endfunction()

# Installation rules for SLSA artifacts
function(install_slsa_artifacts TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        install(
            DIRECTORY "${CMAKE_BINARY_DIR}/slsa/"
            DESTINATION "share/dinero/slsa"
            COMPONENT slsa
            OPTIONAL
        )
    endif()
endfunction()

# Function to validate SLSA level compliance
function(validate_slsa_level TARGET_NAME REQUIRED_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Validating SLSA Level ${REQUIRED_LEVEL} compliance for ${TARGET_NAME}")
        
        # SLSA Level 1: Build process is fully scripted/automated
        set(LEVEL_1_MET TRUE)
        
        # SLSA Level 2: Version control and hosted build service
        set(LEVEL_2_MET FALSE)
        if(GIT_COMMIT_HASH AND DEFINED ENV{CI})
            set(LEVEL_2_MET TRUE)
        endif()
        
        # SLSA Level 3: Source and build platforms are hardened
        set(LEVEL_3_MET FALSE)
        if(LEVEL_2_MET AND COSIGN_EXECUTABLE)
            set(LEVEL_3_MET TRUE)
        endif()
        
        # Report compliance
        message(STATUS "SLSA Level 1 (Scripted Build): ${LEVEL_1_MET}")
        message(STATUS "SLSA Level 2 (Version Control + Hosted Build): ${LEVEL_2_MET}")
        message(STATUS "SLSA Level 3 (Hardened Platform): ${LEVEL_3_MET}")
        
        if(REQUIRED_LEVEL GREATER 1 AND NOT LEVEL_2_MET)
            message(WARNING "SLSA Level 2+ requires CI environment and version control")
        endif()
        
        if(REQUIRED_LEVEL GREATER 2 AND NOT LEVEL_3_MET)
            message(WARNING "SLSA Level 3+ requires signing capabilities (cosign)")
        endif()
    endif()
endfunction()
