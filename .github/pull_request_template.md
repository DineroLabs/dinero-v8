## PR Checklist

- [ ] Builds on Linux, macOS, Windows with `-DDIN_WITH_ROCKSDB=OFF`
- [ ] No Qt types in `src/core/`, `src/daemon/`, or `include/dinero/core/**`
- [ ] New files added explicitly to CMake (no globs)
- [ ] Placeholders use `DIN_TODO("…")` or are properly `#if-guarded`
- [ ] Headers include what they use (`<string>`, `<algorithm>`, etc.)
- [ ] Feature flags respected (`DIN_WITH_ROCKSDB`, `DIN_BUILD_GUI`, `DIN_ENABLE_P2P`)
- [ ] JSON via `djson::value` (no raw mix of JsonCpp and nlohmann in the same TU)
- [ ] Platform abstraction used instead of direct OS calls
- [ ] Core code is STL-only (no Qt dependencies)

## Changes

<!-- Describe what this PR changes -->

## Testing

<!-- How was this tested? -->

## Platform Testing

- [ ] Linux build tested
- [ ] macOS build tested  
- [ ] Windows build tested
