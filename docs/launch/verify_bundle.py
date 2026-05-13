#!/usr/bin/env python3
"""
DineroCoin Mainnet Launch Kit - Bundle Verification Tool

Recomputes SHA256 hashes of all launch files and verifies against
NOTARIZATION_BUNDLE.json. Provides clear PASS/FAIL output for transparency audits.

Usage:
    python3 verify_bundle.py
    python3 verify_bundle.py --verbose
"""

import json
import hashlib
import os
import sys
from pathlib import Path

# ANSI color codes for terminal output
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
BLUE = '\033[94m'
BOLD = '\033[1m'
RESET = '\033[0m'

def compute_sha256(filepath):
    """Compute SHA256 hash of a file."""
    try:
        with open(filepath, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"{RED}Error reading {filepath}: {e}{RESET}", file=sys.stderr)
        return None

def verify_bundle(verbose=False):
    """Verify all launch files against NOTARIZATION_BUNDLE.json."""
    
    # Get script directory
    script_dir = Path(__file__).parent.absolute()
    bundle_file = script_dir / "NOTARIZATION_BUNDLE.json"
    
    if not bundle_file.exists():
        print(f"{RED}❌ NOTARIZATION_BUNDLE.json not found in {script_dir}{RESET}")
        return False
    
    # Load bundle
    try:
        with open(bundle_file, 'r') as f:
            bundle = json.load(f)
    except json.JSONDecodeError as e:
        print(f"{RED}❌ Invalid JSON in NOTARIZATION_BUNDLE.json: {e}{RESET}")
        return False
    except Exception as e:
        print(f"{RED}❌ Error reading bundle: {e}{RESET}")
        return False
    
    # Extract bundle metadata
    version = bundle.get('version', 'unknown')
    checksum_consensus = bundle.get('checksum_consensus', 'unknown')
    bundle_hash = bundle.get('signatures', {}).get('sha256_bundle', 'unknown')
    files = bundle.get('files', {})
    
    if verbose:
        print(f"{BLUE}{'=' * 70}{RESET}")
        print(f"{BOLD}{BLUE}DineroCoin Mainnet Launch Kit - Bundle Verification{RESET}")
        print(f"{BLUE}{'=' * 70}{RESET}")
        print()
        print(f"Version: {version}")
        print(f"Consensus Checksum: {checksum_consensus}")
        print(f"Bundle Hash: {bundle_hash}")
        print()
    
    # Verify each file
    print(f"{BOLD}File Verification:{RESET}")
    print("-" * 70)
    
    all_pass = True
    results = []
    
    for filename, file_info in files.items():
        expected_hash = file_info.get('sha256', '')
        expected_size = file_info.get('size_bytes', 0)
        filepath = script_dir / filename
        
        # Compute actual hash
        actual_hash = compute_sha256(filepath)
        
        if actual_hash is None:
            status = f"{RED}❌ FILE NOT FOUND{RESET}"
            all_pass = False
            actual_size = "N/A"
        else:
            # Get actual file size
            actual_size = os.path.getsize(filepath) if filepath.exists() else 0
            
            # Verify hash match
            hash_match = actual_hash == expected_hash
            size_match = actual_size == expected_size
            
            if hash_match and size_match:
                status = f"{GREEN}✅ PASS{RESET}"
            else:
                status = f"{RED}❌ FAIL{RESET}"
                all_pass = False
                if not hash_match:
                    status += f" {RED}(hash mismatch){RESET}"
                if not size_match:
                    status += f" {RED}(size mismatch){RESET}"
        
        results.append({
            'filename': filename,
            'expected_hash': expected_hash,
            'actual_hash': actual_hash or 'N/A',
            'expected_size': expected_size,
            'actual_size': actual_size if isinstance(actual_size, int) else 'N/A',
            'status': status,
            'pass': actual_hash == expected_hash if actual_hash else False
        })
    
    # Print results table
    print(f"{'Filename':<30} {'Status':<15} {'Hash Match':<12} {'Size Match':<12}")
    print("-" * 70)
    
    for r in results:
        hash_match = "✅" if r['actual_hash'] == r['expected_hash'] else "❌"
        size_match = "✅" if r['actual_size'] == r['expected_size'] else "❌"
        print(f"{r['filename']:<30} {r['status']:<15} {hash_match:<12} {size_match:<12}")
        
        if verbose and not r['pass']:
            print(f"  Expected: {r['expected_hash']}")
            print(f"  Actual:   {r['actual_hash']}")
            print(f"  Expected size: {r['expected_size']} bytes")
            print(f"  Actual size:   {r['actual_size']} bytes")
            print()
    
    print("-" * 70)
    print()
    
    # Verify bundle hash itself
    print(f"{BOLD}Bundle Integrity:{RESET}")
    print("-" * 70)
    
    # Recompute bundle hash (excluding signatures.sha256_bundle)
    bundle_copy = bundle.copy()
    bundle_copy['signatures'] = {k: v for k, v in bundle['signatures'].items() 
                                if k != 'sha256_bundle'}
    bundle_json = json.dumps(bundle_copy, indent=2, sort_keys=False)
    computed_bundle_hash = hashlib.sha256(bundle_json.encode()).hexdigest()
    
    bundle_match = computed_bundle_hash == bundle_hash
    if bundle_match:
        print(f"{GREEN}✅ Bundle hash verified: {computed_bundle_hash}{RESET}")
    else:
        print(f"{RED}❌ Bundle hash mismatch!{RESET}")
        print(f"  Expected: {bundle_hash}")
        print(f"  Computed: {computed_bundle_hash}")
        all_pass = False
    
    print()
    
    # Final summary
    print(f"{BOLD}{'=' * 70}{RESET}")
    if all_pass:
        print(f"{GREEN}{BOLD}✅ ALL VERIFICATIONS PASSED{RESET}")
        print(f"{GREEN}The launch kit is cryptographically authentic and verified.{RESET}")
    else:
        print(f"{RED}{BOLD}❌ VERIFICATION FAILED{RESET}")
        print(f"{RED}One or more files do not match the notarization bundle.{RESET}")
        print(f"{RED}This launch kit may be compromised or corrupted.{RESET}")
    print(f"{BOLD}{'=' * 70}{RESET}")
    
    return all_pass

if __name__ == "__main__":
    verbose = '--verbose' in sys.argv or '-v' in sys.argv
    
    success = verify_bundle(verbose=verbose)
    
    sys.exit(0 if success else 1)

