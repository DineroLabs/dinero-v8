#!/bin/bash
# One-time setup: Initialize Git repo on Virginia as central hub

set -e

VIRGINIA="173.249.195.59"
USER="root"  # Adjust if needed

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔧 Setting up bare Git repo on Virginia server"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# 1. Create bare repo on Virginia
ssh ${USER}@${VIRGINIA} "
    mkdir -p ~/repos
    cd ~/repos
    if [[ ! -d dinero.git ]]; then
        git init --bare dinero.git
        echo '✅ Bare repo created at ~/repos/dinero.git'
    else
        echo '⚠️  Bare repo already exists'
    fi
"

# 2. Add remote on Mac
if git remote | grep -q "^virginia$"; then
    echo "⚠️  Remote 'virginia' already exists, updating URL..."
    git remote set-url virginia ssh://${USER}@${VIRGINIA}/~/repos/dinero.git
else
    echo "➕ Adding remote 'virginia'..."
    git remote add virginia ssh://${USER}@${VIRGINIA}/~/repos/dinero.git
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ Setup complete!"
echo ""
echo "Next steps:"
echo "  1. Commit your changes: git commit -am 'Latest consensus fixes'"
echo "  2. Push to Virginia:    git push virginia main"
echo "  3. SSH to each server and clone:"
echo "     ssh ${USER}@${VIRGINIA}"
echo "     git clone ~/repos/dinero.git DineroCoin"
echo "     cd DineroCoin && ./linux_build.sh"
echo ""
echo "  4. SSH to California and sync:"
echo "     ssh root@172.93.160.131"
echo "     git clone ssh://${USER}@${VIRGINIA}/~/repos/dinero.git DineroCoin"
echo "     cd DineroCoin && ./linux_build.sh"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

