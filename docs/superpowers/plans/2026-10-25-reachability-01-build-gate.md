# Reachability Plan 1 — Build-Integrity Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every shipped `dinerod` (macOS arm64 and Intel, Windows desktop and server,
Linux server `.deb`/tarball, Linux desktop, and the Docker image built from the Linux
tarball) is proven, from the finished binary, to have UPnP, NAT-PMP, QUIC and the QUIC
TLS bridge compiled in. A release lane that would ship without them fails.

**Architecture:** There are two gates.
1. **Configure time.** A new CMake option, `DINERO_REQUIRE_REACHABILITY_STACK`, turns
   today's silent "not found" STATUS messages into a FATAL_ERROR.
2. **After the build.** A Python checker launches the built daemon in regtest, reads
   `getnetworkinfo` and fails if any of the four features is absent.

Linux and Windows lanes start building miniupnpc/libnatpmp statically from `depends/`,
as macOS already does, so the Linux dynamic-dependency allow-list stays unchanged.

**Tech Stack:** CMake 3.x, GitHub Actions (bash and PowerShell), Python 3 (stdlib only),
the `depends/` ExternalProject recipes, the vendored ngtcp2 1.22.1 and OpenSSL 3.5.7.

**Spec:** `docs/superpowers/specs/2026-10-25-zero-touch-p2p-reachability-design.md`,
section A ("Build-integrity gate") and acceptance test 7.

**This is plan 1 of 7 for the spec.** The later plans are:
- 2a: native UDP/QUIC peer transport
- 2b: dial-back verification, IPv6 listening and NAT-type measurement
- 2c: crawler, community seeds and the health page
- 3: community relays with caps and identity binding
- 4: relay-introduced hole punching
- 5: peer quality, upgrade prompts, PCP and UPnP IPv6 pinholes
- 6: combined-candidate re-qualification and rollout

Plan 1 lands first because every later acceptance test needs real builds that carry
the stack.

## Global Constraints

- Linux release dynamic-dependency allow-list stays **exactly**
  `ld-linux-x86-64.so.2 libc.so.6 libgcc_s.so.1 libm.so.6 libstdc++.so.6 libudev.so.1`
  (`.github/workflows/linux-release.yml`, "Assert dynamic dependency contract").
- OpenSSL pin stays 3.5.7 in every lane (`scripts/ci/check_openssl_version.sh` / `.ps1`).
- New CMake option name: `DINERO_REQUIRE_REACHABILITY_STACK`, default `OFF`. Release
  lanes set it `ON`.
- The four feature names, used verbatim in CMake messages, checker output and tests, in
  this order: `upnp`, `natpmp`, `quic`, `quic_crypto`.
- RPC fields checked (from `src/rpc/methods_network_context.cpp`):
  `port_mapping.upnp_compiled`, `port_mapping.natpmp_compiled`,
  `quic_transport.ngtcp2_available`, `quic_transport.crypto_available`. A missing field
  counts as **missing**, never as present.
- No consensus-code changes in this plan. Only build files, CI workflows, packaging
  scripts and the new checker are touched.
- Owner verification rules:
  - A passing test counts only after you've confirmed it ran (grep the log) and that
    it fails without the change (neuter it).
  - No `assert()`-only gates.
  - `touch` and rebuild before trusting a rerun.
- Commits as Claude:
  `git -c user.name=Claude -c user.email=team@dinerolabs.org commit ...` with trailer
  `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- Work on branch `feat/reachability-build-gate` off `origin/dinero-main`, in its own
  worktree. Never touch `~/src/dinero-v8`, which holds the owner's uncommitted work.

## Review Focus

1. **An old binary without the RPC fields** (e.g. v8.1.12 lacks nothing, but a future
   refactor could rename a field). The checker must report the feature MISSING and exit
   1, never 0. Pinned in Task 1, `test_absent_fields_count_as_missing`.
2. **A daemon that never answers RPC** (crash on start, port clash, slow runner). The
   checker must exit 2 with the daemon's last log lines and must not hang past
   `--timeout`. Pinned in Task 1, `test_daemon_that_never_answers_exits_2`.
3. **A daemon left running after the check** on a CI runner (a later step's port or
   datadir clash). The checker must kill the daemon on every exit path. Pinned in
   Task 1, `test_daemon_is_stopped_after_success`.
4. **Host libraries picked up instead of `depends/`** on a developer-like runner, which
   would leak `libminiupnpc.so`/`libnatpmp.so` into NEEDED. The allow-list step must
   still pass on the new Linux build. Pinned in Task 3, Step 4 (objdump against the
   exact allow-list).
5. **MSVC CRT mismatch between `depends/` (static `/MT` by default in
   `depends/toolchains/windows-msvc-x64.cmake`) and the main Windows build.** This
   shows up as LNK2038 or a crash. Pinned in Task 4, Step 1 (match the runtime) and
   Step 4 (the lane builds and the checker passes on `dinerod.exe`).

---

### Task 1: Binary-level reachability checker

**Files:**
- Create: `scripts/ci/check_reachability_stack.py`
- Create: `scripts/ci/test_check_reachability_stack.py`
- Modify: `.github/workflows/tests.yml` (add a run step next to the existing
  `python3 scripts/ci/test_check_mandatory_tests_execute.py` at ~line 140)

**Interfaces:**
- Produces:
  - `FEATURES: tuple[str, ...] = ("upnp", "natpmp", "quic", "quic_crypto")`
  - `evaluate(netinfo: dict) -> list[str]` returns the missing features, in `FEATURES`
    order.
  - CLI 1: `check_reachability_stack.py --dinerod PATH --cli PATH [--timeout SECONDS=90]`
  - CLI 2: `check_reachability_stack.py --from-json FILE`
  - Exit codes: 0 = all present, 1 = something missing, 2 = daemon or RPC failure.
  - Output: one line per feature, `upnp: compiled` or `upnp: MISSING`. On exit 1, a
    final line `reachability stack incomplete: missing <comma-separated list>`.

  Later tasks call CLI 1 from CI.

- [ ] **Step 1: Write the failing tests** in `scripts/ci/test_check_reachability_stack.py`.

  Follow the style of `scripts/ci/test_check_mandatory_tests_execute.py`: the header
  docstring, loading the module via `importlib`, a plain `unittest` main.

  ```python
  FULL = {"port_mapping": {"upnp_compiled": True, "natpmp_compiled": True},
          "quic_transport": {"ngtcp2_available": True, "crypto_available": True}}

  def test_all_present(self):
      self.assertEqual(gate.evaluate(FULL), [])

  def test_each_feature_reported_by_name(self):
      for path, name in [(("port_mapping", "upnp_compiled"), "upnp"),
                         (("port_mapping", "natpmp_compiled"), "natpmp"),
                         (("quic_transport", "ngtcp2_available"), "quic"),
                         (("quic_transport", "crypto_available"), "quic_crypto")]:
          info = copy.deepcopy(FULL); info[path[0]][path[1]] = False
          self.assertEqual(gate.evaluate(info), [name])

  def test_absent_fields_count_as_missing(self):
      self.assertEqual(gate.evaluate({}), ["upnp", "natpmp", "quic", "quic_crypto"])
      self.assertEqual(gate.evaluate({"port_mapping": FULL["port_mapping"]}),
                       ["quic", "quic_crypto"])

  def test_from_json_exit_codes(self):
      # write FULL -> exit 0; write {} -> exit 1, and stdout ends with
      # "reachability stack incomplete: missing upnp, natpmp, quic, quic_crypto"

  def test_daemon_that_never_answers_exits_2(self):
      # fake dinerod = python script that exits 1 immediately;
      # fake cli = script that always exits 1.
      # Run with --timeout 5: exit code 2, wall time < 15 s

  def test_daemon_is_stopped_after_success(self):
      # fake dinerod = script writing its pid to <tmp>/pid then sleeping 600 s;
      # fake cli = script printing json.dumps(FULL).
      # Exit 0, and os.kill(pid, 0) then raises ProcessLookupError (daemon stopped)
  ```

  Generate the fake executables as `#!/usr/bin/env python3` files with `chmod 0o755`
  in a `tempfile.TemporaryDirectory()`.

- [ ] **Step 2: Run the tests and see them fail**

  Run: `python3 scripts/ci/test_check_reachability_stack.py`
  Expected: an error loading `check_reachability_stack.py` (file does not exist).

- [ ] **Step 3: Implement `scripts/ci/check_reachability_stack.py`**

  Standard library only.
  - Launch: `[dinerod, "--regtest", f"--datadir={tmp}", f"--port={p2p}",
    f"--rpcport={rpc}", "--dnsseed=0", "--fixedseeds=0", "--listen=0"]`. Allocate both
    ports by binding a socket to port 0 and closing it.
  - Poll `[cli, "-regtest", f"-datadir={tmp}", f"-rpcport={rpc}", "getnetworkinfo"]`
    once a second until it returns JSON or the timeout passes.
  - Always terminate the daemon in a `finally`: `terminate()`, wait up to 30 s, then
    `kill()`.
  - On exit 2, print the last 40 lines of the daemon's captured stdout and stderr.
  - It must run on Windows: no `os.setsid`, and executable paths are passed in by the
    caller.

- [ ] **Step 4: Run the tests and see them pass**

  Run: `python3 scripts/ci/test_check_reachability_stack.py`
  Expected: `OK`, with all 6 tests run (confirm the count in the output).

- [ ] **Step 5: Check against real binaries on the owner's Mac.** This is the neuter
  check, using a known-good and a known-bad binary.

  ```bash
  R=/Applications/dinero-qt-8.1.12-backup.app/Contents/Resources
  python3 scripts/ci/check_reachability_stack.py --dinerod $R/dinerod --cli $R/dinero-cli
  # expect exit 0, four "compiled" lines

  R=/Applications/dinero-qt.app/Contents/Resources
  python3 scripts/ci/check_reachability_stack.py --dinerod $R/dinerod --cli $R/dinero-cli
  # expect exit 1, "missing upnp, natpmp, quic, quic_crypto" (the dev build)
  ```

  Record both outputs in the task report.

- [ ] **Step 6: Wire the self-test into `tests.yml`**

  Add a step named `Reachability checker self-test` running
  `python3 scripts/ci/test_check_reachability_stack.py`, next to the existing
  self-test step.

- [ ] **Step 7: Commit**

  ```bash
  git add scripts/ci/check_reachability_stack.py scripts/ci/test_check_reachability_stack.py .github/workflows/tests.yml
  git -c user.name=Claude -c user.email=team@dinerolabs.org commit -m "ci: binary-level reachability stack checker (upnp/natpmp/quic/quic_crypto)"
  ```

---

### Task 2: Fail-closed CMake gate

**Files:**
- Create: `cmake/RequireReachabilityStack.cmake`
- Create: `tests/cmake/require_reachability_stack_driver.cmake`
- Create: `scripts/ci/test_require_reachability_stack.py`
- Modify: `cmake/BuildOptions.cmake` (add the option after the existing QUIC options at
  ~line 19-20)
- Modify: `cmake/PortMapping.cmake` (record what was found)
- Modify: `CMakeLists.txt` (include and call it right after
  `include(cmake/VendorNgtcp2.cmake)` at ~line 258)
- Modify: `.github/workflows/tests.yml` (run the new self-test)

**Interfaces:**
- Consumes:
  - `DINERO_ENABLE_PORTMAPPING` and `DINERO_ENABLE_QUIC` (existing options)
  - `DINERO_NGTCP2_CRYPTO_BACKEND` (existing, set in `cmake/VendorNgtcp2.cmake`; values
    `none` / `ossl` / `quictls`). It is undefined when QUIC is off, because that file
    `return()`s early; treat undefined as `none`.
- Produces:
  - `option(DINERO_REQUIRE_REACHABILITY_STACK "Fail configure unless UPnP, NAT-PMP, QUIC and the QUIC TLS bridge are all compiled in (release lanes)" OFF)`
  - Normal variables `DINERO_PORTMAP_HAVE_MINIUPNPC` and `DINERO_PORTMAP_HAVE_NATPMP`
    (`ON`/`OFF`), set by `PortMapping.cmake`.
  - `function(dinero_require_reachability_stack)`. When the option is ON and anything
    is missing:
    `message(FATAL_ERROR "Release build requires the reachability stack; missing: <list>")`.
    When the option is OFF and anything is missing:
    `message(WARNING "Reachability stack incomplete (dev build): missing <list> — this binary cannot open router ports and/or carry encrypted relay traffic")`.
  - `<list>` uses the four Global Constraint names, comma-space separated, in order.

- [ ] **Step 1: Write the failing tests** in
  `scripts/ci/test_require_reachability_stack.py`.

  Each case runs
  `cmake -D<VAR>=<VALUE>... -P tests/cmake/require_reachability_stack_driver.cmake`.
  The driver only does `include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/RequireReachabilityStack.cmake)`
  and `dinero_require_reachability_stack()`.

  ```python
  ALL = dict(DINERO_ENABLE_PORTMAPPING="ON", DINERO_PORTMAP_HAVE_MINIUPNPC="ON",
             DINERO_PORTMAP_HAVE_NATPMP="ON", DINERO_ENABLE_QUIC="ON",
             DINERO_NGTCP2_CRYPTO_BACKEND="ossl")

  def test_required_and_complete_passes(self):            # REQUIRE=ON + ALL -> rc 0
  def test_required_reports_each_missing_by_name(self):
      # NATPMP OFF -> rc != 0, stderr has "missing: natpmp"
      # BACKEND none -> "missing: quic_crypto"
      # QUIC OFF with BACKEND unset -> "missing: quic, quic_crypto"
  def test_required_all_missing_lists_in_order(self):
      # REQUIRE=ON only -> "missing: upnp, natpmp, quic, quic_crypto"
  def test_not_required_only_warns(self):
      # REQUIRE=OFF only -> rc 0, stderr contains "Reachability stack incomplete (dev build)"
  ```

- [ ] **Step 2: Run the tests and see them fail**

  Run: `python3 scripts/ci/test_require_reachability_stack.py`
  Expected: FAIL (the driver's include target does not exist).

- [ ] **Step 3: Implement it**
  - Add the option to `cmake/BuildOptions.cmake`.
  - In `cmake/PortMapping.cmake`, set `DINERO_PORTMAP_HAVE_MINIUPNPC`/`_NATPMP` to `OFF`
    at the top of the `if(DINERO_ENABLE_PORTMAPPING)` block. Set each to `ON` in both of
    its "found" branches (pkg-config and find_library).
  - Write `cmake/RequireReachabilityStack.cmake`.
  - In `CMakeLists.txt`, add the include and the function call after the
    `VendorNgtcp2.cmake` include.

- [ ] **Step 4: Run the tests and see them pass**

  Run: `python3 scripts/ci/test_require_reachability_stack.py`
  Expected: `OK`, with 4 tests run.

- [ ] **Step 5: Real configure check on the Dell.**

  Linux builder `tower@192.168.1.108`. Use a fresh clone or worktree of the branch,
  not the owner's trees, and run the builds with `nice -n 19` because the Dell's node
  is syncing.

  ```bash
  cmake -S . -B /tmp/gate-neg -G Ninja -DDINERO_REQUIRE_REACHABILITY_STACK=ON -DDINERO_ENABLE_PORTMAPPING=OFF -DDINERO_ENABLE_QUIC=ON -DDINERO_USE_VENDORED_DEPS=ON
  # expect: configure FAILS, "missing: upnp, natpmp"

  cmake -S depends -B depends/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=depends/toolchains/linux-x86_64.cmake && cmake --build depends/build
  export DINERO_DEPENDS_PREFIX="$(sed -n 's/^DINERO_DEPENDS_PREFIX=//p' depends/current-prefix.env)"
  cmake -S . -B /tmp/gate-pos -G Ninja -DDINERO_REQUIRE_REACHABILITY_STACK=ON -DDINERO_ENABLE_PORTMAPPING=ON -DDINERO_ENABLE_QUIC=ON -DDINERO_USE_VENDORED_DEPS=ON -DDINERO_DEPENDS_PREFIX="$DINERO_DEPENDS_PREFIX"
  # expect: configure succeeds; the log shows miniupnpc and libnatpmp found from the
  # depends prefix and "ngtcp2 QUIC crypto bridge" enabled
  ```

  The vendored OpenSSL 3.5.7 must be built first (`bash scripts/build-openssl-vendored.sh`).
  Without it the QUIC TLS bridge isn't detected. That is itself a correct gate failure
  to note in the report.

- [ ] **Step 6: Wire into `tests.yml` and commit**

  Add a step `Reachability CMake gate self-test` running
  `python3 scripts/ci/test_require_reachability_stack.py`.

  ```bash
  git add cmake/RequireReachabilityStack.cmake cmake/BuildOptions.cmake cmake/PortMapping.cmake CMakeLists.txt tests/cmake/require_reachability_stack_driver.cmake scripts/ci/test_require_reachability_stack.py .github/workflows/tests.yml
  git -c user.name=Claude -c user.email=team@dinerolabs.org commit -m "build: DINERO_REQUIRE_REACHABILITY_STACK fails configure when upnp/natpmp/quic/quic_crypto are missing"
  ```

---

### Task 3: Linux lanes ship the full stack (server tarball/.deb, Docker, Linux desktop)

**Files:**
- Modify: `.github/workflows/linux-release.yml`
  - new "Build depends" step after "Build vendored OpenSSL 3.5.7"
  - "Configure" at ~lines 120-137, including the comment at ~119-122 that explains why
    port mapping was OFF
  - new "Assert reachability stack" step after "Assert dynamic dependency contract"
- Modify: `.github/workflows/desktop-release.yml` (`linux-desktop` job)
  - new "Build depends" step after "Build vendored OpenSSL 3.5.7" (~line 205)
  - "Configure exact release" (~lines 208-224)
  - new gate step after "Build complete desktop stack" (~line 225)

**Interfaces:**
- Consumes: Task 1 CLI 1 and the Task 2 option.
- Produces: Linux artifacts that pass the checker. The Docker image
  (`Dockerfile` downloads `dinero-linux-x86_64-<ver>.tar.gz`) inherits the gate from
  this lane, so no Docker change is needed.

- [ ] **Step 1: Edit `linux-release.yml`**
  - "Build depends" step:
    ```bash
    cmake -S depends -B depends/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=depends/toolchains/linux-x86_64.cmake
    cmake --build depends/build -j"$(nproc)"
    sed -n 's/^DINERO_DEPENDS_PREFIX=/DINERO_DEPENDS_PREFIX=/p' depends/current-prefix.env >> "$GITHUB_ENV"
    ```
  - In Configure:
    - replace `-DDINERO_ENABLE_PORTMAPPING=OFF` with `-DDINERO_ENABLE_PORTMAPPING=ON`;
    - add `-DDINERO_ENABLE_QUIC=ON`, `-DDINERO_REQUIRE_REACHABILITY_STACK=ON` and
      `-DDINERO_DEPENDS_PREFIX="$DINERO_DEPENDS_PREFIX"`;
    - rewrite the comment: port mapping is now ON via static `depends/` libs, which add
      no NEEDED entries, and the allow-list below still enforces that.
  - "Assert reachability stack" step:
    `python3 scripts/ci/check_reachability_stack.py --dinerod build-release/dinerod --cli build-release/dinero-cli`
  - Add `depends/sources` to the existing "Cache vendored prerequisites" paths.

- [ ] **Step 2: Edit `desktop-release.yml` `linux-desktop`.** Make the same three
  changes:
  - the Build depends step;
  - in Configure, flip `-DDINERO_ENABLE_PORTMAPPING=OFF` at ~line 220 to ON, and add
    `-DDINERO_REQUIRE_REACHABILITY_STACK=ON` and the depends prefix (QUIC is already ON);
  - a gate step after "Build complete desktop stack" against that job's built
    `dinerod`/`dinero-cli`. Use the paths the "Verify Linux desktop artifacts" step
    already uses.

- [ ] **Step 3: Reproduce the release configuration on the Dell.** Use the
  `linux-release.yml` flag set exactly as edited, in `/tmp/linux-rel` under `nice -n 19`.

  ```bash
  cmake --build /tmp/linux-rel --target dinerod dinero-cli -j4
  python3 scripts/ci/check_reachability_stack.py --dinerod /tmp/linux-rel/dinerod --cli /tmp/linux-rel/dinero-cli
  # expect exit 0, four "compiled"
  ```

- [ ] **Step 4: Prove the allow-list still holds** (Review Focus 4).

  ```bash
  objdump -p /tmp/linux-rel/dinerod | awk '/NEEDED/{print $2}' | sort
  # expect EXACTLY: ld-linux-x86-64.so.2 libc.so.6 libgcc_s.so.1 libm.so.6 libstdc++.so.6 libudev.so.1
  ```

  Then run the workflow's allow-list loop verbatim against this binary and confirm it
  prints `dependency contract satisfied`.

- [ ] **Step 5: Neuter check.** Reconfigure the same tree with
  `-DDINERO_REQUIRE_REACHABILITY_STACK=OFF -DDINERO_ENABLE_QUIC=OFF`, rebuild `dinerod`,
  and run the checker. Expect exit 1 with `missing quic, quic_crypto`. Restore the flags.

- [ ] **Step 6: Commit**

  ```bash
  git add .github/workflows/linux-release.yml .github/workflows/desktop-release.yml
  git -c user.name=Claude -c user.email=team@dinerolabs.org commit -m "ci(linux): ship upnp/natpmp (static depends) and QUIC in server and desktop lanes, gated"
  ```

---

### Task 4: Windows lanes ship the full stack (desktop and server installer)

**Files:**
- Modify: `.github/workflows/windows-release.yml`
  - new "Build depends (MSVC)" step after "Setup MSVC" (~line 137) and before "Build
    complete Windows desktop stack" (~line 250)
  - the desktop configure at ~lines 253-264
  - new gate step after "Daemon startup, mining, and restart smoke" (~line 321)
- Modify: `packaging/windows/build-server-installer.ps1` (`$configureArgs`, ~lines
  285-302)

**Interfaces:**
- Consumes: Task 1 CLI 1 and the Task 2 option.
- Produces: `dinerod.exe` (desktop bundle and server installer) that passes the checker.

- [ ] **Step 1: Make the depends runtime match the main build** (Review Focus 5).
  - In the lane, run the desktop configure once and read `CMAKE_MSVC_RUNTIME_LIBRARY`
    from `build-msvc-native\CMakeCache.txt`. Also look for the `MSVC Runtime:` STATUS
    line printed by `cmake/Repro.cmake`.
  - Configure `depends/` with the same value. If the main build uses the CMake default,
    that's `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`, so pass `MultiThreadedDLL`.
  - Write the chosen value into the step as a literal, with a comment saying where it
    came from.

- [ ] **Step 2: Add the "Build depends (MSVC)" step** (PowerShell, inside the MSVC dev
  shell set up by "Setup MSVC").

  ```powershell
  cmake -S depends -B depends\build-msvc -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=depends\toolchains\windows-msvc-x64.cmake `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=<value from Step 1>
  cmake --build depends\build-msvc
  ```

  Export the prefix from `depends\current-prefix.env` to `$env:GITHUB_ENV` as
  `DINERO_DEPENDS_PREFIX`. Confirm it is `depends\windows-AMD64`, the path
  `build-server-installer.ps1` already probes. If the host name differs, use the env
  value in both places.

- [ ] **Step 3: Pass the prefix and the requirement**
  - Desktop configure: add ``-DDINERO_DEPENDS_PREFIX="$env:DINERO_DEPENDS_PREFIX" ` ``
    and ``-DDINERO_REQUIRE_REACHABILITY_STACK=ON ` ``.
  - `build-server-installer.ps1`: append the same two to `$configureArgs`. Keep the
    existing `-DCMAKE_PREFIX_PATH=$dependsPrefix` block; it's harmless.

- [ ] **Step 4: Add the gate step after the smoke test**

  ```powershell
  python scripts\ci\check_reachability_stack.py --dinerod build-msvc-server\Release\dinerod.exe --cli build-msvc-server\Release\dinero-cli.exe
  if ($LASTEXITCODE -ne 0) { throw 'server dinerod lacks the reachability stack' }
  python scripts\ci\check_reachability_stack.py --dinerod build-msvc-native\Release\dinerod.exe --cli build-msvc-native\Release\dinero-cli.exe
  if ($LASTEXITCODE -ne 0) { throw 'desktop dinerod lacks the reachability stack' }
  ```

  `dinero-cli.exe` in `build-msvc-native\Release` is an assumption. Use the paths the
  "Stage GUI installer" step actually copies.

- [ ] **Step 5: Commit**

  ```bash
  git add .github/workflows/windows-release.yml packaging/windows/build-server-installer.ps1
  git -c user.name=Claude -c user.email=team@dinerolabs.org commit -m "ci(windows): build depends with matching CRT, require and verify the reachability stack"
  ```

  This lane can only be verified on a Windows runner; Task 6 does that.

---

### Task 5: macOS lanes require and verify the stack

**Files:**
- Modify: `.github/workflows/macos-release.yml`
  - "CMake configure" (~lines 145-160)
  - new gate step after "Build daemon + CLI" (~line 164)
- Modify: `.github/workflows/desktop-release.yml` (`macos-intel-desktop`)
  - "Configure exact Intel release" (~lines 485-505)
  - new gate step after "Build complete Intel desktop stack" (~line 508)

**Interfaces:**
- Consumes: Task 1 CLI 1 and the Task 2 option.

- [ ] **Step 1: Add `-DDINERO_REQUIRE_REACHABILITY_STACK=ON`** to both configure
  commands. Both already pass PORTMAPPING and QUIC ON.

- [ ] **Step 2: Add the gate steps.** Run
  `python3 scripts/ci/check_reachability_stack.py --dinerod <built dinerod> --cli <built dinero-cli>`
  with each job's build paths (the ones its existing verify/smoke step uses). Keep
  `packaging/mac/assert-v8-release-lane.sh` unchanged; it stays as the build-file
  check, and this adds the binary check.

- [ ] **Step 3: Local sanity on the owner's Mac.** Run Task 1 Step 5 again; the v8.1.12
  release binary passes. A full macOS lane build is verified in Task 6.

- [ ] **Step 4: Commit**

  ```bash
  git add .github/workflows/macos-release.yml .github/workflows/desktop-release.yml
  git -c user.name=Claude -c user.email=team@dinerolabs.org commit -m "ci(macos): require and verify the reachability stack in release and Intel desktop lanes"
  ```

---

### Task 6: Prove every lane in CI, including a deliberately broken build

**Files:** none new. This task runs the workflows on the branch.

**Owner approval needed at execution time:** this pushes `feat/reachability-build-gate`
to GitHub and dispatches workflows. Ask before the first push.

- [ ] **Step 1: Push the branch** (after approval) and open a draft PR so `tests.yml`
  runs. Confirm in the run log that both self-test steps executed and reported `OK` with
  6 and 4 tests.

- [ ] **Step 2: Dispatch each release lane on the branch** (`workflow_dispatch`):
  `linux-release`, `desktop-release` (linux-desktop and macos-intel-desktop),
  `macos-release` and `windows-release`.
  - For each, grep the log for the gate step's four `compiled` lines and exit 0.
  - Record run URLs.
  - Any red that isn't the gate goes into the report; don't fix unrelated breakage in
    this plan.

- [ ] **Step 3: Negative run** (acceptance test 7)
  - Push a temporary commit flipping `linux-release.yml` back to
    `-DDINERO_ENABLE_PORTMAPPING=OFF` while keeping
    `-DDINERO_REQUIRE_REACHABILITY_STACK=ON`.
  - Dispatch `linux-release` and confirm it fails at Configure with
    `missing: upnp, natpmp`.
  - Then drop that commit (`git revert`, or reset the branch before review) and confirm
    the next dispatch is green.

- [ ] **Step 4: Write the evidence into the PR description**: the run URLs, the gate
  output per lane, and the negative run's failure line. Mark the PR ready for review.

---

## After this plan

Fleet nodes (SJ, NA, EU1) run hand-built binaries without QUIC, so the encrypted relay
data plane is down network-wide. Putting gated release artifacts on the fleet and
DineroTX belongs to Plan 3's canary (relays) and Plan 6's rollout, under the owner's
one-node-at-a-time deploy rules. It is not part of this plan.
