#!/bin/bash
# Rebuild and test Dinero-qt GUI

set -e

echo "🖥️  Dinero-qt GUI Rebuild & Test"
echo "==============================="
echo ""

cd /Users/haydarevich/Documents/DineroCoin

echo "📋 Step 1: Check if daemon is running"
if pgrep -f "dinerod" > /dev/null; then
    echo "✅ Daemon is running"
else
    echo "⚠️  Daemon not running locally"
    echo "   Starting daemon..."
    nohup ./build/dinerod -datadir=./data -rpcport=20998 -port=20999 > /dev/null 2>&1 &
    sleep 3
    if pgrep -f "dinerod" > /dev/null; then
        echo "✅ Daemon started (PID: $(pgrep -f dinerod))"
    else
        echo "❌ Failed to start daemon"
        exit 1
    fi
fi

echo ""
echo "📋 Step 2: Check Qt installation"
if [ -d "$HOME/Qt/6.9.1/macos" ]; then
    echo "✅ Qt 6.9.1 found"
    QT_PATH="$HOME/Qt/6.9.1/macos"
elif [ -d "/opt/homebrew/opt/qt@6" ]; then
    echo "✅ Qt 6 found (Homebrew)"
    QT_PATH="/opt/homebrew/opt/qt@6"
else
    echo "❌ Qt 6 not found"
    echo "   Install with: brew install qt@6"
    exit 1
fi

echo ""
echo "📋 Step 3: Rebuild GUI"
cd gui

if [ ! -d "build" ]; then
    mkdir build
fi

cd build

echo "  Configuring CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_PATH" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    > cmake_config.log 2>&1

if [ $? -ne 0 ]; then
    echo "❌ CMake configuration failed"
    tail -20 cmake_config.log
    exit 1
fi

echo "  Building..."
cmake --build . -j8 > build.log 2>&1

if [ $? -ne 0 ]; then
    echo "❌ Build failed"
    tail -50 build.log
    exit 1
fi

echo "✅ GUI built successfully!"

echo ""
echo "📋 Step 4: Test GUI"
if [ -f "dinero-qt" ]; then
    ls -lh dinero-qt
    echo ""
    echo "🚀 Launching GUI (will open in window)..."
    echo "   Close the window to continue..."
    ./dinero-qt &
    GUI_PID=$!
    
    sleep 5
    
    if ps -p $GUI_PID > /dev/null; then
        echo "✅ GUI launched successfully (PID: $GUI_PID)"
        echo ""
        echo "GUI is running. Test these features:"
        echo "  1. Overview tab - Check blockchain info"
        echo "  2. Wallet tab - Create/restore wallet"
        echo "  3. Send tab - Try sending (if you have balance)"
        echo "  4. Mining tab - Start/stop miner"
        echo ""
        echo "Press Enter when done testing..."
        read
        
        kill $GUI_PID 2>/dev/null || true
    else
        echo "⚠️  GUI closed immediately (check for errors)"
    fi
else
    echo "❌ dinero-qt binary not found"
    exit 1
fi

echo ""
echo "========================================="
echo "📊 Summary"
echo "========================================="
echo ""
echo "✅ GUI Location: gui/build/dinero-qt"
echo "✅ Daemon: Running"
echo "✅ Build: Success"
echo ""
echo "To launch GUI manually:"
echo "  cd gui/build"
echo "  ./dinero-qt"
echo ""

