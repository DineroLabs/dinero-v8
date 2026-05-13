#!/bin/bash
# Move Duplicate Files to duplicates/ folder
# SAFE: Files are moved, not deleted - can be restored

set -e

PROJECT_ROOT="/Users/haydarevich/Documents/DineroCoin"
cd "$PROJECT_ROOT"

echo "🧹 Dinero Duplicate File Organizer"
echo "=================================="
echo ""

# Create duplicates directory structure
echo "📁 Creating duplicates directory structure..."
mkdir -p duplicates/{consensus,privacy,wallet,cli,mining,storage,daemon,auth,rpc,core}

# Count files before
TOTAL_BEFORE=$(find src -type f \( -name "*.bak" -o -name "*.backup" -o -name "*_backup*" -o -name "*_old*" \) 2>/dev/null | wc -l | tr -d ' ')
echo "📊 Found $TOTAL_BEFORE duplicate/backup files"
echo ""

# Function to move file and create parent dirs
move_file() {
    local src=$1
    local category=$2
    
    # Get relative path from src/
    local rel_path=$(echo "$src" | sed 's|src/||')
    local dir_path=$(dirname "$rel_path")
    local filename=$(basename "$rel_path")
    
    # Create destination directory
    mkdir -p "duplicates/$category/$dir_path"
    
    # Move file
    if [ -f "$src" ]; then
        mv "$src" "duplicates/$category/$dir_path/$filename"
        echo "  ✓ Moved: $filename"
    fi
}

# Move consensus backups
echo "🔄 Moving consensus backups..."
find src/consensus -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "consensus"
done

# Move privacy/coinjoin backups
echo "🔄 Moving privacy/coinjoin backups..."
find src/core/privacy src/privacy -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "privacy"
done

# Move wallet backups
echo "🔄 Moving wallet backups..."
find src/core/wallet src/daemon -name "*wallet*.bak" -o -name "*wallet*.backup" 2>/dev/null | while read file; do
    move_file "$file" "wallet"
done

# Move CLI backups
echo "🔄 Moving CLI backups..."
if [ -f "src/cli/main_backup.cpp" ]; then
    mv src/cli/main_backup.cpp duplicates/cli/
    echo "  ✓ Moved: main_backup.cpp"
fi
find src/cli -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "cli"
done

# Move mining backups
echo "🔄 Moving mining backups..."
find src/mining src/daemon -name "*mining*.bak" -o -name "*mining*.backup" -o -name "*gbt*.bak" -o -name "*gbt*.backup" 2>/dev/null | while read file; do
    move_file "$file" "mining"
done

# Move storage backups
echo "🔄 Moving storage backups..."
find src/storage -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "storage"
done

# Move daemon backups (except wallet/mining already moved)
echo "🔄 Moving daemon backups..."
if [ -f "src/daemon/main.cpp.backup" ]; then
    mv src/daemon/main.cpp.backup duplicates/daemon/
    echo "  ✓ Moved: main.cpp.backup"
fi
find src/daemon -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    # Skip if already moved (wallet/mining)
    if [[ ! "$file" =~ wallet ]] && [[ ! "$file" =~ mining ]] && [[ ! "$file" =~ gbt ]]; then
        move_file "$file" "daemon"
    fi
done

# Move auth backups
echo "🔄 Moving auth backups..."
find src/auth -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "auth"
done

# Move RPC backups
echo "🔄 Moving RPC backups..."
find src/daemon/rpc src/rpc -name "*.bak" -o -name "*.backup" 2>/dev/null | while read file; do
    move_file "$file" "rpc"
done

# Move alternative main.cpp files (CONDITIONAL - ask user first)
echo ""
echo "📋 Alternative main.cpp files found:"
ls -lh src/daemon/main*.cpp 2>/dev/null || true
echo ""
read -p "❓ Move main_clean.cpp and main_simple.cpp to duplicates? (y/n) " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    if [ -f "src/daemon/main_clean.cpp" ]; then
        mv src/daemon/main_clean.cpp duplicates/daemon/
        echo "  ✓ Moved: main_clean.cpp"
    fi
    if [ -f "src/daemon/main_simple.cpp" ]; then
        mv src/daemon/main_simple.cpp duplicates/daemon/
        echo "  ✓ Moved: main_simple.cpp"
    fi
else
    echo "  ⏭️  Skipped main_clean.cpp and main_simple.cpp"
fi

echo ""
echo "📄 Creating duplicates README..."

cat > duplicates/README.md << 'EOF'
# Duplicate Files Archive

**Date Archived**: $(date)
**Project**: Dinero Cryptocurrency

## Purpose

This directory contains backup and duplicate files moved during codebase cleanup.  
**Files are preserved here** - not deleted - in case they're needed for reference.

## Organization

```
duplicates/
├── consensus/      # Consensus algorithm backups
├── privacy/        # Coinjoin/privacy backups
├── wallet/         # Wallet implementation backups
├── cli/            # CLI tool backups
├── mining/         # Mining engine backups
├── storage/        # Database/storage backups
├── daemon/         # Daemon core backups (includes main_*.cpp variants)
├── auth/           # Authentication backups
└── rpc/            # RPC handler backups
```

## Restoration

To restore any file:
```bash
# Find the file
find duplicates/ -name "filename.cpp.bak"

# Copy back to original location
cp duplicates/path/to/file.cpp.bak src/path/to/file.cpp
```

## Notes

- **main.cpp variants**: 
  - `main.cpp` (307KB) = ACTIVE - Most complete, full wallet RPC
  - `main_clean.cpp` (61KB) = ARCHIVED - Minimal, missing wallet
  - `main_simple.cpp` (8KB) = ARCHIVED - Very basic
  
- **.bak files**: Usually created during edits - may contain older logic
- **.backup files**: Manual backups - may have important changes

## Commit Message

```
chore: organize duplicate/backup files into duplicates/ folder

- Moved 61+ .bak/.backup files to duplicates/
- Preserved alternative main.cpp variants for reference
- No files deleted - all restorable
- Cleaner project structure for production
```

## Safety

✅ **Reversible**: All files preserved, can be restored  
✅ **Organized**: Categorized by module  
✅ **Documented**: This README explains all moves  
⚠️ **Review before delete**: Some .bak files may have important code

---

**Last Updated**: $(date)
EOF

echo "✅ Created duplicates/README.md"

# Count files after
TOTAL_AFTER=$(find duplicates -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.bak" -o -name "*.backup" \) 2>/dev/null | wc -l | tr -d ' ')

echo ""
echo "✅ Duplicate organization complete!"
echo "=================================="
echo "📊 Files moved: $TOTAL_AFTER"
echo "📁 Location: $(pwd)/duplicates/"
echo ""
echo "🔍 To restore any file:"
echo "   cp duplicates/path/to/file src/path/to/file"
echo ""
echo "📋 Next steps:"
echo "   1. Verify build: cmake --build build"
echo "   2. Check duplicates/README.md"
echo "   3. Commit: git add duplicates/ && git commit -m 'chore: organize duplicates'"
echo ""

