#!/bin/bash

# Smart deployment script - only uploads modified files
# Usage: ./deploy_modified.sh [hours] [server]
#   hours: Number of hours to look back for modifications (default: 24)
#   server: 'california', 'virginia', or 'both' (default: both)

set -e

HOURS=${1:-24}
SERVER=${2:-both}
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
CA_SERVER="172.93.160.131"
VA_SERVER="173.249.195.59"

echo "═══════════════════════════════════════════════════════"
echo "  Smart Deployment - Modified Files Only"
echo "═══════════════════════════════════════════════════════"
echo "Looking for files modified in the last $HOURS hours..."
echo ""

# Find all modified source files
MODIFIED_FILES=$(find src include tools -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -mtime -${HOURS}h 2>/dev/null)

if [ -z "$MODIFIED_FILES" ]; then
    echo "❌ No modified files found in the last $HOURS hours"
    echo ""
    echo "Tip: Use 'find src include -type f -mtime -1' to see what would be found"
    exit 0
fi

echo "📝 Found modified files:"
echo "$MODIFIED_FILES" | while read file; do
    echo "   - $file"
done
echo ""

# Count files
FILE_COUNT=$(echo "$MODIFIED_FILES" | wc -l | tr -d ' ')
echo "Total: $FILE_COUNT file(s) to deploy"
echo ""

# Function to deploy to a server
deploy_to_server() {
    local SERVER_NAME=$1
    local SERVER_IP=$2

    echo "═══════════════════════════════════════════════════════"
    echo "  Deploying to $SERVER_NAME ($SERVER_IP)"
    echo "═══════════════════════════════════════════════════════"

    # Create a temporary file list for rsync
    TEMP_FILE_LIST="/tmp/deploy_files_$$.txt"
    echo "$MODIFIED_FILES" > "$TEMP_FILE_LIST"

    # Upload only modified files
    echo "📤 Uploading modified files..."
    rsync -avz --progress \
        -e "ssh -i $SSH_KEY" \
        --files-from="$TEMP_FILE_LIST" \
        ./ root@$SERVER_IP:/root/DineroCoin/

    rm -f "$TEMP_FILE_LIST"

    echo ""
    echo "🔨 Determining which object files to rebuild..."

    # Extract .cpp files that were modified (need recompilation)
    CPP_FILES=$(echo "$MODIFIED_FILES" | grep "\.cpp$" || true)

    if [ -n "$CPP_FILES" ]; then
        echo "Files requiring recompilation:"
        echo "$CPP_FILES" | while read cpp; do
            echo "   - $cpp"
        done
        echo ""

        # Build list of object files to remove
        OBJ_FILES=""
        while read cpp; do
            # Convert src/daemon/main.cpp to CMakeFiles/dinerod.dir/src/daemon/main.cpp.o
            OBJ_FILE="CMakeFiles/dinerod.dir/${cpp}.o"
            OBJ_FILES="$OBJ_FILES $OBJ_FILE"
        done <<< "$CPP_FILES"

        echo "🗑️  Removing stale object files on $SERVER_NAME..."
        ssh -i "$SSH_KEY" root@$SERVER_IP "
            cd /root/DineroCoin/build-linux
            for obj in $OBJ_FILES; do
                if [ -f \"\$obj\" ]; then
                    echo \"  Removing: \$obj\"
                    rm -f \"\$obj\"
                fi
            done
        "
        echo ""

        echo "🔨 Rebuilding dinerod on $SERVER_NAME..."
        ssh -i "$SSH_KEY" root@$SERVER_IP "
            cd /root/DineroCoin/build-linux &&
            make dinerod -j\$(nproc) 2>&1 | tail -20
        "
    else
        echo "ℹ️  No .cpp files modified - only headers updated"
        echo "   A full rebuild may still be needed if headers changed function signatures"
    fi

    echo ""
    echo "✅ Deployment to $SERVER_NAME complete!"
    echo ""
}

# Deploy based on server parameter
if [ "$SERVER" == "california" ] || [ "$SERVER" == "both" ]; then
    deploy_to_server "California" "$CA_SERVER"
fi

if [ "$SERVER" == "virginia" ] || [ "$SERVER" == "both" ]; then
    deploy_to_server "Virginia" "$VA_SERVER"
fi

echo "═══════════════════════════════════════════════════════"
echo "  ✅ Smart Deployment Complete!"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "Next steps:"
echo "  1. Restart daemons on the server(s)"
echo "  2. Check logs to verify the fix is working"
echo ""
