# Block graph ownership inventory

This preparatory gate records two things before the block graph moves into a
chainstate owner:

- First-party CMake listings of `block_index.cpp` and `block_lifecycle.cpp`,
  including direct test compiles.
- Raw references to the graph globals in first-party `src/` and `include/`
  C++ files outside those two implementations.

It fails if either count grows beyond `graph_owner_baseline.json`. A reduction
requires updating the baseline in the same commit, so removed access cannot
silently return. Run it with:

```sh
python3 scripts/ci/chainstate-separation/graph_owner_gate.py
uv run --no-project --with pytest python -m pytest -q \
  scripts/ci/chainstate-separation/tests/test_graph_owner_gate.py
```

The current CMake inventory lists `block_index.cpp` in both `dinero_core` and
`dinero_consensus`, plus seven direct test compiles. It lists
`block_lifecycle.cpp` in `dinero_core` plus six direct test compiles. This is
the existing link contract, not approval to add another owner.

The reference scan is conservative. It includes declarations in headers and
the dormant `src/core/consensus/block_index.cpp` mirror; counts are source
tokens, not proven live call sites. Comments and string/character literals are
masked. CMake scanning walks the whole first-party tree while excluding vendor
and generated build directories. It recognizes literal source paths in
`CMakeLists.txt` files; it cannot resolve a source path assembled entirely
through CMake variables. Before graph ownership changes, verify the CMake
codemodel and archive symbols in both Orchard configurations and on Windows.

This checker is local until the graph-owner stage defines the final link
boundary and CI wiring. It does not alter consensus, graph state, or reorg
transitions. It belongs to the chainstate program, separate from the
consensus-globals purity stages.
