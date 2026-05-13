# Mempool Tests

## Phase C.2: Covenant Mempool Policy Tests

### Running the Tests

To build and run the covenant mempool policy tests:

```bash
# Build the test
cd tests/mempool
g++ -std=c++17 -I../../include -I../../src \
    test_mempool_covenant_policy.cpp \
    -o test_mempool_covenant_policy \
    -L../../build/lib \
    -ldinero_consensus -ldinero_wallet

# Run the test
./test_mempool_covenant_policy
```

### Test Coverage

**test_mempool_covenant_policy.cpp**:
1. ✅ Covenant detection heuristic
2. ✅ DoS protection - too many covenant inputs
3. ✅ Covenant ancestor safety - missing parent
4. ✅ Covenant ancestor safety - confirmed parent allowed
5. ✅ Standard transactions unaffected by covenant policy
6. ✅ Mixed covenant and standard inputs counted correctly
7. ✅ Mempool entry metadata storage
8. ✅ Config defaults verification

### Test Philosophy

These are **POLICY tests**, not consensus tests:
- Mempool policy mirrors consensus rules but doesn't duplicate validation
- Covenant detection uses byte-pattern heuristic (not full script execution)
- Consensus validation happens in script interpreter (single source of truth)
- Tests verify policy enforcement, not covenant correctness

### Integration with Build System

To add to CMakeLists.txt (optional - standalone tests work):

```cmake
# Phase C.2: Covenant Mempool Policy Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_covenant_policy.cpp)
  add_executable(test_covenant_policy
    tests/mempool/test_mempool_covenant_policy.cpp
  )
  target_include_directories(test_covenant_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(test_covenant_policy PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_mempool
  )
  add_test(NAME CovenantMempoolPolicy COMMAND test_covenant_policy)
endif()
```

### Next Steps

Phase C.3 will add:
- Valid covenant transaction construction
- Full end-to-end covenant tests (wallet → mempool → consensus)
- Integration tests with real blockchain
