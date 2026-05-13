#!/bin/bash
# Deploy Dinero to all Linux nodes (Virginia + California)
# Usage: ./deploy_all.sh [--restart] [--verify]

set -e

VIRGINIA="173.249.195.59"
CALIFORNIA="172.93.160.131"
USER="root"  # Adjust if needed
RESTART=false
VERIFY_ONLY=false

# Parse arguments
for arg in "$@"; do
    case $arg in
        --restart)
            RESTART=true
            ;;
        --verify)
            VERIFY_ONLY=true
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Usage: $0 [--restart] [--verify]"
            exit 1
            ;;
    esac
done

# Function to get git commit from a server
get_server_commit() {
    local SERVER=$1
    local NAME=$2
    
    ssh ${USER}@${SERVER} "cd ~/DineroCoin && git rev-parse HEAD 2>/dev/null || echo 'error'" || echo "error"
}

# Function to get running daemon version
get_daemon_version() {
    local SERVER=$1
    
    ssh ${USER}@${SERVER} "~/DineroCoin/build/bin/dinero-cli -rpcport=20998 node.info 2>/dev/null | grep '\"git_commit\"' | cut -d'\"' -f4" || echo "not_running"
}

# Function to verify version consistency
verify_versions() {
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "🔍 Verifying version consistency across all nodes"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    # Get local Mac commit
    LOCAL_COMMIT=$(git rev-parse HEAD)
    echo "📍 Local (Mac):      $LOCAL_COMMIT"
    
    # Get Virginia commits
    VA_REPO_COMMIT=$(get_server_commit "$VIRGINIA" "Virginia")
    VA_DAEMON_VERSION=$(get_daemon_version "$VIRGINIA")
    echo "📍 Virginia (repo):  $VA_REPO_COMMIT"
    echo "   Virginia (daemon): $VA_DAEMON_VERSION"
    
    # Get California commits
    CA_REPO_COMMIT=$(get_server_commit "$CALIFORNIA" "California")
    CA_DAEMON_VERSION=$(get_daemon_version "$CALIFORNIA")
    echo "📍 California (repo):  $CA_REPO_COMMIT"
    echo "   California (daemon): $CA_DAEMON_VERSION"
    
    echo ""
    
    # Check consistency
    ALL_OK=true
    
    if [[ "$VA_REPO_COMMIT" != "$LOCAL_COMMIT" ]]; then
        echo "⚠️  WARNING: Virginia repo is out of sync with local Mac"
        ALL_OK=false
    fi
    
    if [[ "$CA_REPO_COMMIT" != "$LOCAL_COMMIT" ]]; then
        echo "⚠️  WARNING: California repo is out of sync with local Mac"
        ALL_OK=false
    fi
    
    if [[ "$VA_REPO_COMMIT" != "$CA_REPO_COMMIT" ]]; then
        echo "⚠️  WARNING: Virginia and California repos are out of sync"
        ALL_OK=false
    fi
    
    if [[ "$VA_DAEMON_VERSION" != "not_running" ]] && [[ "$VA_DAEMON_VERSION" != "$VA_REPO_COMMIT" ]]; then
        echo "⚠️  WARNING: Virginia daemon is running old version (needs restart)"
        ALL_OK=false
    fi
    
    if [[ "$CA_DAEMON_VERSION" != "not_running" ]] && [[ "$CA_DAEMON_VERSION" != "$CA_REPO_COMMIT" ]]; then
        echo "⚠️  WARNING: California daemon is running old version (needs restart)"
        ALL_OK=false
    fi
    
    if [[ "$ALL_OK" == "true" ]]; then
        echo "✅ All nodes are synchronized and running matching versions!"
    else
        echo ""
        echo "❌ Version mismatch detected. Run without --verify to deploy, or use --restart to update daemons."
        return 1
    fi
    
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# If verify-only mode, just check and exit
if [[ "$VERIFY_ONLY" == "true" ]]; then
    verify_versions
    exit $?
fi

deploy_to_server() {
    local SERVER=$1
    local NAME=$2
    
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "🚀 Deploying to $NAME ($SERVER)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    ssh ${USER}@${SERVER} "
        set -e
        cd ~/DineroCoin || exit 1
        
        echo '📥 Pulling latest code...'
        git pull
        
        echo '🔨 Building native Linux binaries...'
        cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF
        cmake --build build --target dinerod dinero-cli -j\$(nproc)
        
        # Show built version
        BUILT_COMMIT=\$(git rev-parse HEAD)
        echo \"✅ Build complete on $NAME (commit: \${BUILT_COMMIT:0:12})\"
        
        if [[ '$RESTART' == 'true' ]]; then
            echo '🔄 Restarting dinerod...'
            pkill -9 dinerod || true
            sleep 2
            nohup ~/DineroCoin/build/bin/dinerod -daemon \
                -datadir=~/dinero-data \
                -rpcport=20998 \
                -port=20999 \
                > ~/dinero-data/daemon.log 2>&1 &
            sleep 3
            echo '✅ Daemon restarted on $NAME'
        fi
    " || {
        echo "❌ Deployment failed on $NAME ($SERVER)"
        return 1
    }
    
    echo "✅ $NAME deployment complete"
    echo ""
}

# Deploy to both servers
deploy_to_server "$VIRGINIA" "Virginia"
deploy_to_server "$CALIFORNIA" "California"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ All deployments complete!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Run verification after deployment
echo ""
verify_versions
