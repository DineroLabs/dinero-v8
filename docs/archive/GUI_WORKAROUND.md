# GUI Issue & Workaround

## Problem
The GUI is crashing with a segfault after loading QML. The QML MinerPane loads successfully but then crashes during initialization.

## Quick Workaround
Disable QML mining tab temporarily to get the GUI working:

1. Edit `gui/CMakeLists.txt` - set `HAVE_QT_QUICK` to `OFF`
2. Rebuild
3. GUI will use widgets-based mining tab instead (shows message about using command-line miner)

## To Use Now
Instead of GUI mining, use command-line:

```bash
# Terminal 1: GUI (for wallet management)
./build-gui/dinero-qt

# Terminal 2: Mining (command-line)
./build-clean/dinero-miner --rpc http://127.0.0.1:20998/ --address YOUR_DIN_ADDRESS --threads 8
```

## Root Cause
The QML crash is happening after StandardPaths fix. Possible causes:
1. QML engine initialization issue
2. MinerController registration problem  
3. Context property conflict

## Permanent Fix Needed
Investigate with debugger to find exact crash location in QML initialization.

