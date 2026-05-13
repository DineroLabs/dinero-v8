#!/usr/bin/env python3
"""
Refactor Transaction struct fields to Bitcoin standard naming.
Changes:
  - inputs -> vin
  - outputs -> vout
  - locktime -> lockTime
"""

import re
import sys
from pathlib import Path

def refactor_transaction_fields(file_path, dry_run=True):
    """
    Refactor Transaction struct fields to Bitcoin standard names.
    Returns True if changes were made.
    """
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"⚠️  Could not read {file_path}: {e}")
        return False
    
    original = content
    changes = []
    
    # Pattern 1: .inputs -> .vin (member access)
    new_content = re.sub(r'\.inputs\b', '.vin', content)
    if new_content != content:
        changes.append(".inputs → .vin")
        content = new_content
    
    new_content = re.sub(r'->inputs\b', '->vin', content)
    if new_content != content:
        changes.append("->inputs → ->vin")
        content = new_content
    
    # Pattern 2: .outputs -> .vout
    new_content = re.sub(r'\.outputs\b', '.vout', content)
    if new_content != content:
        changes.append(".outputs → .vout")
        content = new_content
    
    new_content = re.sub(r'->outputs\b', '->vout', content)
    if new_content != content:
        changes.append("->outputs → ->vout")
        content = new_content
    
    # Pattern 3: .locktime -> .lockTime
    new_content = re.sub(r'\.locktime\b', '.lockTime', content)
    if new_content != content:
        changes.append(".locktime → .lockTime")
        content = new_content
    
    new_content = re.sub(r'->locktime\b', '->lockTime', content)
    if new_content != content:
        changes.append("->locktime → ->lockTime")
        content = new_content
    
    # Pattern 4: Struct member declarations
    new_content = re.sub(
        r'std::vector<TxInput>\s+inputs;',
        'std::vector<TxInput> vin;',
        content
    )
    if new_content != content:
        changes.append("member: inputs → vin")
        content = new_content
    
    new_content = re.sub(
        r'std::vector<TxOutput>\s+outputs;',
        'std::vector<TxOutput> vout;',
        content
    )
    if new_content != content:
        changes.append("member: outputs → vout")
        content = new_content
    
    new_content = re.sub(
        r'uint32_t\s+locktime;',
        'uint32_t lockTime;',
        content
    )
    if new_content != content:
        changes.append("member: locktime → lockTime")
        content = new_content
    
    new_content = re.sub(
        r'int32_t\s+locktime;',
        'int32_t lockTime;',
        content
    )
    if new_content != content:
        changes.append("member: locktime → lockTime")
        content = new_content
    
    if content != original:
        if dry_run:
            print(f"📝 {file_path}")
            for change in changes:
                print(f"   • {change}")
        else:
            try:
                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(content)
                print(f"✅ {file_path}")
            except Exception as e:
                print(f"❌ Failed to write {file_path}: {e}")
                return False
        return True
    return False

def main():
    # Find all relevant files
    include_files = list(Path('include').rglob('*.h')) + list(Path('include').rglob('*.hpp'))
    src_files = list(Path('src').rglob('*.cpp')) + list(Path('src').rglob('*.hpp'))
    
    all_files = include_files + src_files
    
    # Filter out backup files
    all_files = [f for f in all_files if '.bak' not in str(f) and 'duplicates' not in str(f)]
    
    print("═══════════════════════════════════════════════════════════════")
    print("🐍 TRANSACTION FIELD REFACTORING SCRIPT")
    print("═══════════════════════════════════════════════════════════════")
    print(f"Found {len(all_files)} files to check")
    print()
    print("Changes to make:")
    print("  • inputs  → vin")
    print("  • outputs → vout")
    print("  • locktime → lockTime")
    print("═══════════════════════════════════════════════════════════════")
    print()
    
    # DRY RUN
    print("🔍 DRY RUN - Analyzing files...")
    print()
    changed_files = 0
    for f in all_files:
        if refactor_transaction_fields(f, dry_run=True):
            changed_files += 1
    
    print()
    print("═══════════════════════════════════════════════════════════════")
    print(f"📊 Summary: {changed_files} files need changes")
    print("═══════════════════════════════════════════════════════════════")
    
    if changed_files == 0:
        print("✅ No changes needed!")
        return 0
    
    print()
    response = input("Apply these changes? [y/N]: ").strip().lower()
    
    if response != 'y':
        print("❌ Aborted by user")
        return 1
    
    # APPLY CHANGES
    print()
    print("✍️  Applying changes...")
    print()
    success = 0
    for f in all_files:
        if refactor_transaction_fields(f, dry_run=False):
            success += 1
    
    print()
    print("═══════════════════════════════════════════════════════════════")
    print(f"✅ COMPLETE! Updated {success} files")
    print("═══════════════════════════════════════════════════════════════")
    print()
    print("Next steps:")
    print("  1. Test build: cmake --build build -j8")
    print("  2. Check for any issues")
    print("  3. Commit changes: git add -A && git commit -m 'Refactor to Bitcoin-standard field names'")
    print()
    
    return 0

if __name__ == '__main__':
    sys.exit(main())

