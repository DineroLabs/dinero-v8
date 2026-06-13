# Manual test plan — Issue #295 (child lifecycle, fail-loud startup, DMG prompt)

Build under test: `dinero-qt` from branch `fix/qt-lifecycle-295`.
All steps assume macOS unless noted. Daemon RPC port = 20998.

Useful commands:

```sh
# Who holds the RPC port?
lsof -i :20998

# Any dinero children alive?
pgrep -fl 'dinerod|dinero-seeder|dinero-stratum|dinero-miner'
```

---

## A. Child lifecycle — children die with the GUI

### A1. Normal quit paths kill the GUI-spawned daemon

Repeat for each quit path: (a) menu File → Quit, (b) Cmd+Q, (c) close the
main window with the red traffic-light button.

1. Ensure no dinerod is running: `pgrep -fl dinerod` → empty.
2. Launch dinero-qt. Wait for status pill "Connected".
3. `pgrep -fl dinerod` → exactly one dinerod (spawned by the GUI, with
   `--embedded-parent-pid`).
4. Quit via the path under test.
5. Within ~10 s: `pgrep -fl dinerod` → **empty**. `lsof -i :20998` → **empty**.

Expected: daemon receives a graceful `stop` first (check tail of
`~/Library/Application Support/Dinero/debug.log` for a clean shutdown
sequence), escalating to SIGTERM/SIGKILL only if it hangs.

### A2. External daemon is NOT killed (connect-to-existing preserved)

1. Start a daemon manually:
   `./dinerod --datadir "$HOME/Library/Application Support/Dinero" --rpc --rpcport 20998 --listen --p2pport 20999`
2. Launch dinero-qt → it should connect to the existing daemon
   (no second dinerod in `pgrep -fl dinerod`).
3. Quit the GUI.
4. `pgrep -fl dinerod` → the manually started daemon is **still running**.
   (The GUI only owns daemons it spawned; this is the deliberate
   keep-running behavior for operator daemons.)
5. Cleanup: `dinero-cli stop`.

### A3. GUI crash still reaps the daemon (POSIX death-watch)

1. Launch dinero-qt, wait for "Connected", note the dinerod PID.
2. Hard-kill the GUI: `kill -9 $(pgrep -f dinero-qt | head -1)`.
3. Within a few seconds dinerod should exit on its own
   (`--embedded-parent-pid` kqueue parent watch on macOS).
   `pgrep -fl dinerod` → empty.

### A4. Orphaned seeder swept at next launch

Simulates the rc37 incident (seeder survived 3 days holding the port).

1. Create an orphan: start any long-running process renamed/symlinked as
   `dinero-seeder` whose parent exits, e.g.
   ```sh
   ln -sf /bin/sleep /tmp/dinero-seeder
   sh -c '/tmp/dinero-seeder 99999 &'    # shell exits → PPID becomes 1
   pgrep -fl dinero-seeder               # note PID; verify PPID is 1: ps -p <pid> -o ppid=
   ```
2. Launch dinero-qt (with no healthy daemon running).
3. Expected: the orphan is gone (`pgrep -fl dinero-seeder` → empty) —
   the GUI sweeps PPID==1 dinero-seeder processes before spawning dinerod.
4. Negative control: a seeder whose parent is still alive (run
   `sh -c '/tmp/dinero-seeder 99999'` in a Terminal you keep open) must
   NOT be killed.

---

## B. Fail-loud startup

### B1. Pre-spawn port check (squatted RPC port)

1. Quit all Dinero processes. Squat the port with a non-HTTP listener:
   `nc -l 127.0.0.1 20998` (leave it running).
2. Launch dinero-qt.
3. Expected: BEFORE the main window appears, a dialog
   **"Port 20998 is already in use — another Dinero process may be running."**
   with buttons **Continue** / **Quit**.
   - Quit → app exits, no GUI window, no dinerod spawned.
   - Continue → startup proceeds; since nc still holds the port, dinerod
     exits early → see B2 dialog (this is the fail-loud chain).
4. Healthy-daemon control: with a real responsive daemon already on 20998,
   NO dialog appears and the GUI just connects (A2 behavior).

### B2. Daemon exits early → immediate error dialog with exit code + log tail

1. With the `nc` squatter from B1 still listening, launch dinero-qt and
   choose **Continue**.
2. Expected within ~10 s: dialog **"Daemon Failed to Start"** showing the
   dinerod exit code, with **Show Details** revealing the last ~20 lines of
   `debug.log`. Buttons **Continue Anyway** / **Quit**.
   - Quit → app exits (exit code 1).
   - Continue Anyway → window opens; the in-window auto-start retry will
     also fail and show a **"Daemon Failed"** dialog (same exit code + log
     tail) instead of waiting silently.
3. Also verify the toolbar path: with the squatter active and the GUI open,
   press **Start Daemon** → dialog **"Port Already in Use"** with
   **Connect to Existing / Start Anyway / Cancel**. "Start Anyway" must lead
   to the fail-loud "Daemon Failed" dialog, not a silent hang.

### B3. Daemon dies AFTER startup → unexpected-exit dialog

1. Launch dinero-qt normally; wait for "Connected".
2. `kill -9 $(pgrep -f 'dinerod --datadir')` (simulate daemon crash).
3. Expected: dialog **"Daemon Stopped Unexpectedly"** with the exit code and
   log tail (Show Details).
4. Negative control: pressing the GUI's **Stop Daemon** button (clean stop)
   must NOT show this dialog.

### B4. 60-second visible wait timeout

1. Squat the port (`nc -l 127.0.0.1 20998`), launch, click **Continue**,
   then **Continue Anyway**, then dismiss the auto-start failure dialog,
   and wait without doing anything.
2. Expected at ~60 s after window creation: dialog
   **"Still Waiting for Daemon"** with buttons
   **Show Log** / **Keep Waiting** / **Quit**.
   - Show Log → opens `debug.log` in the default viewer; watchdog re-arms
     (fires again 60 s later if still disconnected).
   - Keep Waiting → re-arms for another 60 s.
   - Quit → main window closes, app exits, no orphan children
     (`pgrep -fl 'dinerod|dinero-seeder'` → empty).
3. Negative control: on a normal connected launch, NO watchdog dialog ever
   appears.

---

## C. Move-to-Applications prompt (macOS, DMG)

1. Build the DMG (or copy Dinero.app onto a DMG/mounted volume), mount it,
   and launch Dinero.app **from the mounted volume** (path starts with
   `/Volumes/`).
2. Expected at startup (before the port check / main window): informational
   dialog **"Running from Disk Image"** suggesting dragging Dinero.app to
   /Applications, with a **"Don't show this again"** checkbox. No auto-move
   is performed.
3. Quit, relaunch from the DMG without checking the box → prompt appears
   again.
4. Relaunch, check **Don't show this again**, OK. Quit and relaunch from the
   DMG → prompt does **NOT** appear
   (QSettings key `ui/dmg_install_prompt_suppressed_v1` = true).
5. Launch from /Applications → prompt never appears regardless of the
   setting.
