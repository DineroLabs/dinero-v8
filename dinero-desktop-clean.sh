#!/bin/bash
# Dinero Desktop Clean Launch Script
# Prevents Qt library conflicts

echo "🚀 Launching Dinero Desktop v0.9.0-beta.1..."

# Clear Qt environment variables
unset QT_PLUGIN_PATH
unset QT_DEBUG_PLUGINS  
unset QTDIR
unset DYLD_FRAMEWORK_PATH
unset DYLD_LIBRARY_PATH

# Launch with minimal environment
env -i PATH="/usr/bin:/bin" open "$(dirname "$0")/build/src/gui-desktop/dinero-desktop.app"

echo "✅ Dinero Desktop launched successfully!"
