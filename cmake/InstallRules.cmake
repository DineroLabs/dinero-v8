# Install and package-gate rules for the root build.
#
# Included from the repository root after daemon, CLI, tools, and optional
# lightningd target wiring has run.

if(TARGET lightningd)
  install(TARGETS lightningd DESTINATION bin)
endif()

# Phase E.3 (Dinero Core 1.0): under DINERO_PACKAGED_BUILD, the .deb
# ships only the daemon and the CLI per spec section 1.1 row 1. Miner and
# bench tooling continues to build (unit tests + dev workflows still
# need them) but is excluded from `cmake --install` so dpkg doesn't
# package them. They get split into a separate dinero-miner package
# in a later phase.
#
# Manual / dev / fleet builds (DINERO_PACKAGED_BUILD=OFF) keep the
# pre-Phase-E install set to avoid breaking workflows that rely on
# /usr/local/bin/dinero-stratum-worker etc.
if(DINERO_PACKAGED_BUILD)
  install(TARGETS dinerod dinero-cli DESTINATION bin)

  # DT_RUNPATH discipline (spec section 1.4, "DT_RUNPATH not DT_RPATH").
  # INSTALL_RPATH applies only to the installed binary; the build-tree
  # binary keeps its CMake-managed RPATH so dev runs against
  # build/_deps/... still resolve. --enable-new-dtags emits RUNPATH
  # instead of legacy RPATH so an operator debugging a bundled .so can
  # override via LD_LIBRARY_PATH without rebuilding dinerod.
  set_target_properties(dinerod dinero-cli PROPERTIES
    INSTALL_RPATH "/usr/lib/dinero"
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE)
  target_link_options(dinerod PRIVATE
    "$<$<PLATFORM_ID:Linux>:LINKER:--enable-new-dtags>")
  target_link_options(dinero-cli PRIVATE
    "$<$<PLATFORM_ID:Linux>:LINKER:--enable-new-dtags>")
else()
  install(TARGETS dinerod dinero-cli ${DINERO_TOOL_TARGETS} DESTINATION bin)
endif()

# Phase C (Dinero Core 1.0) - install the documented examples alongside
# the binaries so packagers and manual-mode operators have a starting
# point for dinero.conf and the systemd unit.
#
# Both files land under ${CMAKE_INSTALL_PREFIX}/share/doc/dinero/. The
# `.example` suffix on the systemd unit is intentional: cmake --install
# does NOT activate the unit automatically (per spec section 1.2). Phase E's
# .deb packaging will copy the example without the suffix to
# /lib/systemd/system/dinero.service for packaged-service mode.
install(FILES
  ${CMAKE_SOURCE_DIR}/share/dinero.conf.example
  DESTINATION share/doc/dinero
)
install(FILES
  ${CMAKE_SOURCE_DIR}/share/systemd/dinero.service.example
  DESTINATION share/doc/dinero
)

# Phase D.1 (Dinero Core 1.0) - journald retention drop-in example.
# Renamed at install time so its purpose is unambiguous when sitting
# alongside the regular dinero.conf.example. Phase E's .deb postinst
# will copy it without the suffix to /etc/systemd/journald.conf.d/dinero.conf.
install(FILES
  ${CMAKE_SOURCE_DIR}/share/systemd/journald.conf.d/dinero.conf.example
  DESTINATION share/doc/dinero
  RENAME journald-dinero.conf.example
)

# Phase D.2 (Dinero Core 1.0) - wallet/shielded backup script. Lives
# at ${CMAKE_INSTALL_PREFIX}/bin/dinero-backup so operators can invoke
# it with the same PATH that holds dinerod / dinero-cli. Mode 0755
# (executable) - the script itself enforces output mode 0600 on the
# archives it produces.
install(PROGRAMS
  ${CMAKE_SOURCE_DIR}/share/scripts/dinero-backup
  DESTINATION bin
)

# Phase D.3 (Dinero Core 1.0) - pre-upgrade rollback capture script.
# Operator-invoked (NOT wired to systemd; capture failure must not
# block dinerod restart per spec section 1.1 row 9). Captures the running
# dinerod binary to <datadir>/binaries/dinerod.live-pre-<commit>-<ts>
# so the operator has a guaranteed-good rollback target before installing
# a new version.
install(PROGRAMS
  ${CMAKE_SOURCE_DIR}/share/scripts/dinero-prepare-upgrade
  DESTINATION bin
)

# Phase E.3 (Dinero Core 1.0) - release-time gate for the .deb.
# Verifies dinerod / dinero-cli have RUNPATH (not RPATH), only system
# libs in NEEDED resolve to allowed paths, and Depends: contains no
# forbidden vendored-leak entries. Run by CI on every tag against the
# staging dir before signing; failure fails the release. Not installed
# into the .deb (it's a CI/build tool, not an operator tool).
if(DINERO_PACKAGED_BUILD)
  add_test(NAME PackageGate
    COMMAND ${CMAKE_SOURCE_DIR}/share/scripts/dinero-deb-verify
            --static --staging ${CMAKE_BINARY_DIR}/debian/dinero-core
  )
  set_tests_properties(PackageGate PROPERTIES
    LABELS "package-gate"
  )
  # Live-mode counterpart `dinero-deb-verify --installed` is not a ctest:
  # it needs `dpkg -i` + `systemctl enable --now` on a clean Linux box.
  # That runs in a separate CI job; both gates must pass before E.3/E.6
  # is complete.
  message(STATUS "Phase E.3 PackageGate ctest registered (label package-gate, --static mode)")
endif()
