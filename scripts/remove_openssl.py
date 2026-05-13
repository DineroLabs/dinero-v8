#!/usr/bin/env python3
"""
OpenSSL Removal Script for Dinero Coin
======================================

This script systematically removes OpenSSL dependencies from the entire codebase
and replaces them with our internal Bitcoin Core-style crypto implementation.

Usage:
    python3 scripts/remove_openssl.py [--dry-run] [--verbose]

Features:
- Removes OpenSSL includes from source files
- Replaces OpenSSL function calls with internal crypto
- Updates CMakeLists.txt files to remove OpenSSL dependencies
- Cleans up build system configuration
- Provides detailed reporting of changes made
"""

import os
import re
import sys
import argparse
import subprocess
from pathlib import Path
from typing import List, Dict, Tuple, Set
import json

class OpenSSLRemover:
    def __init__(self, dry_run: bool = False, verbose: bool = False):
        self.dry_run = dry_run
        self.verbose = verbose
        self.project_root = Path(__file__).parent.parent
        self.changes_made = []
        self.errors = []
        
        # OpenSSL includes to remove
        self.openssl_includes = [
            '#include <openssl/',
            '#include "openssl/',
            '#include <OpenSSL/',
            '#include "OpenSSL/'
        ]
        
        # OpenSSL functions to replace
        self.openssl_functions = {
            # Random number generation
            'RAND_bytes': 'CF_GeneratePrivKey',
            'RAND_pseudo_bytes': 'CF_GeneratePrivKey',
            
            # Hash functions
            'SHA256': 'sha256',
            'SHA256_Init': 'sha256',
            'SHA256_Update': 'sha256',
            'SHA256_Final': 'sha256',
            'RIPEMD160': 'ripemd160',
            'RIPEMD160_Init': 'ripemd160',
            'RIPEMD160_Update': 'ripemd160',
            'RIPEMD160_Final': 'ripemd160',
            
            # HMAC functions
            'HMAC': 'hmac_sha512',
            'HMAC_Init': 'hmac_sha512',
            'HMAC_Update': 'hmac_sha512',
            'HMAC_Final': 'hmac_sha512',
            
            # ECDSA functions
            'ECDSA_sign': 'CF_SignDER',
            'ECDSA_verify': 'CF_VerifyDER',
            'ECDSA_do_sign': 'CF_SignDER',
            'ECDSA_do_verify': 'CF_VerifyDER',
            
            # EVP functions
            'EVP_PKEY_new': 'internal_crypto',
            'EVP_PKEY_free': 'internal_crypto',
            'EVP_PKEY_CTX_new': 'internal_crypto',
            'EVP_PKEY_CTX_free': 'internal_crypto',
            'EVP_DigestSignInit': 'internal_crypto',
            'EVP_DigestSignUpdate': 'internal_crypto',
            'EVP_DigestSignFinal': 'internal_crypto',
            'EVP_DigestVerifyInit': 'internal_crypto',
            'EVP_DigestVerifyUpdate': 'internal_crypto',
            'EVP_DigestVerifyFinal': 'internal_crypto',
            
            # BIGNUM functions
            'BN_new': 'internal_crypto',
            'BN_free': 'internal_crypto',
            'BN_bin2bn': 'internal_crypto',
            'BN_bn2bin': 'internal_crypto',
            'BN_hex2bn': 'internal_crypto',
            'BN_cmp': 'internal_crypto',
            'BN_mod': 'internal_crypto',
            'BN_mod_add': 'internal_crypto',
            
            # EC functions
            'EC_KEY_new_by_curve_name': 'internal_crypto',
            'EC_KEY_free': 'internal_crypto',
            'EC_KEY_set_private_key': 'internal_crypto',
            'EC_KEY_set_public_key': 'internal_crypto',
            'EC_POINT_new': 'internal_crypto',
            'EC_POINT_free': 'internal_crypto',
            'EC_POINT_mul': 'internal_crypto',
            'EC_POINT_oct2point': 'internal_crypto',
            
            # PKCS5 functions
            'PKCS5_PBKDF2_HMAC': 'pbkdf2_hmac_sha256',
            
            # BIO functions
            'BIO_new': 'internal_crypto',
            'BIO_free': 'internal_crypto',
            'BIO_new_mem_buf': 'internal_crypto',
            'BIO_read': 'internal_crypto',
            'BIO_write': 'internal_crypto',
        }
        
        # Files to exclude from processing
        self.exclude_patterns = [
            'build/',
            '.git/',
            'node_modules/',
            '*.pyc',
            '__pycache__/',
            '*.o',
            '*.a',
            '*.so',
            '*.dylib',
            '*.dll',
            '*.exe'
        ]
        
        # File extensions to process
        self.source_extensions = {
            '.cpp', '.cc', '.cxx', '.c',
            '.hpp', '.hh', '.hxx', '.h',
            '.cmake', 'CMakeLists.txt'
        }

    def log(self, message: str, level: str = "INFO"):
        """Log a message with timestamp and level."""
        timestamp = subprocess.run(['date'], capture_output=True, text=True).stdout.strip()
        prefix = "[DRY-RUN] " if self.dry_run else ""
        print(f"{prefix}[{timestamp}] {level}: {message}")
        
        if level == "ERROR":
            self.errors.append(message)
        elif level == "CHANGE":
            self.changes_made.append(message)

    def should_exclude_file(self, file_path: Path) -> bool:
        """Check if a file should be excluded from processing."""
        for pattern in self.exclude_patterns:
            if pattern in str(file_path):
                return True
        return False

    def is_source_file(self, file_path: Path) -> bool:
        """Check if a file is a source file that should be processed."""
        return file_path.suffix in self.source_extensions

    def find_source_files(self) -> List[Path]:
        """Find all source files in the project."""
        source_files = []
        
        for root, dirs, files in os.walk(self.project_root):
            # Skip excluded directories
            dirs[:] = [d for d in dirs if not self.should_exclude_file(Path(root) / d)]
            
            for file in files:
                file_path = Path(root) / file
                if not self.should_exclude_file(file_path) and self.is_source_file(file_path):
                    source_files.append(file_path)
        
        return source_files

    def remove_openssl_includes(self, content: str, file_path: Path) -> Tuple[str, List[str]]:
        """Remove OpenSSL includes from file content."""
        original_content = content
        removed_includes = []
        
        # Remove OpenSSL includes
        for include_pattern in self.openssl_includes:
            pattern = re.compile(rf'^\s*{re.escape(include_pattern)}.*$', re.MULTILINE)
            matches = pattern.findall(content)
            if matches:
                removed_includes.extend(matches)
                content = pattern.sub('', content)
        
        # Remove specific OpenSSL includes
        openssl_include_pattern = re.compile(r'^\s*#include\s*[<"](openssl|OpenSSL)/[^>"]*[>"]\s*$', re.MULTILINE)
        matches = openssl_include_pattern.findall(content)
        if matches:
            removed_includes.extend(matches)
            content = openssl_include_pattern.sub('', content)
        
        # Add our crypto include if we removed OpenSSL includes
        if removed_includes and '#include "crypto/dinero_crypto_minimal.h"' not in content:
            # Find a good place to add our include (after other includes)
            include_section = re.search(r'((?:^\s*#include\s+[^#\n]*\n)+)', content, re.MULTILINE)
            if include_section:
                insert_pos = include_section.end()
                content = content[:insert_pos] + '#include "crypto/dinero_crypto_minimal.h"\n' + content[insert_pos:]
            else:
                # Add at the beginning if no include section found
                content = '#include "crypto/dinero_crypto_minimal.h"\n\n' + content
        
        return content, removed_includes

    def replace_openssl_functions(self, content: str, file_path: Path) -> Tuple[str, List[str]]:
        """Replace OpenSSL function calls with internal crypto."""
        original_content = content
        replacements = []
        
        # Replace function calls
        for openssl_func, replacement in self.openssl_functions.items():
            if replacement == 'internal_crypto':
                # These need special handling - just remove for now
                pattern = rf'\b{re.escape(openssl_func)}\s*\('
                if re.search(pattern, content):
                    replacements.append(f"{openssl_func} -> {replacement}")
                    # For now, just comment out the line
                    content = re.sub(pattern, f'// {openssl_func}(', content)
            else:
                # Simple function name replacement
                pattern = rf'\b{re.escape(openssl_func)}\b'
                if re.search(pattern, content):
                    replacements.append(f"{openssl_func} -> {replacement}")
                    content = re.sub(pattern, replacement, content)
        
        # Replace OpenSSL-specific patterns
        openssl_patterns = [
            # Provider initialization
            (r'init_openssl_providers\s*\(\s*\)', '// init_openssl_providers() - removed'),
            (r'OSSL_PROVIDER\s*\*', '// OSSL_PROVIDER* - removed'),
            (r'OSSL_PROVIDER_load', '// OSSL_PROVIDER_load - removed'),
            
            # EVP patterns
            (r'EVP_[A-Za-z_]+', 'internal_crypto'),
            (r'OSSL_[A-Za-z_]+', 'internal_crypto'),
            
            # OpenSSL version checks
            (r'OPENSSL_VERSION_NUMBER', '1'),  # Always true
            (r'OpenSSL_version', '// OpenSSL_version - removed'),
        ]
        
        for pattern, replacement in openssl_patterns:
            if re.search(pattern, content):
                replacements.append(f"{pattern} -> {replacement}")
                content = re.sub(pattern, replacement, content)
        
        return content, replacements

    def update_cmake_files(self, file_path: Path) -> Tuple[str, List[str]]:
        """Update CMakeLists.txt files to remove OpenSSL dependencies."""
        if not file_path.name in ['CMakeLists.txt'] and not file_path.suffix == '.cmake':
            return None, []
        
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        original_content = content
        changes = []
        
        # Remove OpenSSL find_package
        if 'find_package(OpenSSL' in content:
            content = re.sub(r'find_package\s*\(\s*OpenSSL[^)]*\)', '# find_package(OpenSSL) - removed', content)
            changes.append("Removed find_package(OpenSSL)")
        
        # Remove OpenSSL::SSL and OpenSSL::Crypto
        if 'OpenSSL::SSL' in content or 'OpenSSL::Crypto' in content:
            content = re.sub(r'OpenSSL::SSL\s*', 'dinero_crypto ', content)
            content = re.sub(r'OpenSSL::Crypto\s*', 'dinero_crypto ', content)
            changes.append("Replaced OpenSSL::SSL/OpenSSL::Crypto with dinero_crypto")
        
        # Remove OpenSSL include directories
        if 'OPENSSL_INCLUDE_DIR' in content:
            content = re.sub(r'include_directories\s*\(\s*\${\s*OPENSSL_INCLUDE_DIR\s*}\s*\)', 
                           '# include_directories(${OPENSSL_INCLUDE_DIR}) - removed', content)
            changes.append("Removed OPENSSL_INCLUDE_DIR")
        
        # Remove OpenSSL variables
        openssl_vars = ['OPENSSL_LIBRARIES', 'OPENSSL_FOUND', 'OPENSSL_VERSION']
        for var in openssl_vars:
            if var in content:
                content = re.sub(rf'\$\{{\s*{var}\s*}}', 'dinero_crypto', content)
                changes.append(f"Replaced {var} with dinero_crypto")
        
        return content if content != original_content else None, changes

    def process_file(self, file_path: Path) -> Dict:
        """Process a single file to remove OpenSSL dependencies."""
        result = {
            'file': str(file_path),
            'changes': [],
            'errors': [],
            'modified': False
        }
        
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            original_content = content
            
            # Remove OpenSSL includes
            content, removed_includes = self.remove_openssl_includes(content, file_path)
            if removed_includes:
                result['changes'].append(f"Removed {len(removed_includes)} OpenSSL includes")
                result['modified'] = True
            
            # Replace OpenSSL functions
            content, replacements = self.replace_openssl_functions(content, file_path)
            if replacements:
                result['changes'].append(f"Made {len(replacements)} function replacements")
                result['modified'] = True
            
            # Update CMake files
            if file_path.name == 'CMakeLists.txt' or file_path.suffix == '.cmake':
                cmake_content, cmake_changes = self.update_cmake_files(file_path)
                if cmake_content:
                    content = cmake_content
                    result['changes'].extend(cmake_changes)
                    result['modified'] = True
            
            # Write changes if file was modified
            if result['modified'] and not self.dry_run:
                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(content)
                self.log(f"Updated {file_path}", "CHANGE")
            
            result['content'] = content
            
        except Exception as e:
            error_msg = f"Error processing {file_path}: {str(e)}"
            result['errors'].append(error_msg)
            self.log(error_msg, "ERROR")
        
        return result

    def generate_report(self, results: List[Dict]) -> str:
        """Generate a comprehensive report of all changes made."""
        total_files = len(results)
        modified_files = sum(1 for r in results if r['modified'])
        total_changes = sum(len(r['changes']) for r in results)
        total_errors = sum(len(r['errors']) for r in results)
        
        report = f"""
OpenSSL Removal Report
======================

Summary:
- Total files processed: {total_files}
- Files modified: {modified_files}
- Total changes made: {total_changes}
- Errors encountered: {total_errors}

"""
        
        if modified_files > 0:
            report += "Modified Files:\n"
            report += "===============\n"
            for result in results:
                if result['modified']:
                    report += f"\n{result['file']}:\n"
                    for change in result['changes']:
                        report += f"  - {change}\n"
        
        if total_errors > 0:
            report += "\nErrors:\n"
            report += "=======\n"
            for result in results:
                for error in result['errors']:
                    report += f"  - {error}\n"
        
        return report

    def run(self):
        """Main execution method."""
        self.log("Starting OpenSSL removal process...")
        
        if self.dry_run:
            self.log("DRY RUN MODE - No files will be modified")
        
        # Find all source files
        source_files = self.find_source_files()
        self.log(f"Found {len(source_files)} source files to process")
        
        # Process each file
        results = []
        for file_path in source_files:
            if self.verbose:
                self.log(f"Processing {file_path}")
            
            result = self.process_file(file_path)
            results.append(result)
            
            if result['modified']:
                self.log(f"Modified {file_path}", "CHANGE")
        
        # Generate and display report
        report = self.generate_report(results)
        print(report)
        
        # Save report to file
        report_file = self.project_root / "openssl_removal_report.txt"
        if not self.dry_run:
            with open(report_file, 'w', encoding='utf-8') as f:
                f.write(report)
            self.log(f"Report saved to {report_file}")
        
        # Summary
        if self.dry_run:
            self.log("DRY RUN COMPLETED - Review changes above")
        else:
            self.log("OpenSSL removal completed successfully!")
        
        return len([r for r in results if r['modified']]) > 0

def main():
    parser = argparse.ArgumentParser(description='Remove OpenSSL dependencies from Dinero Coin')
    parser.add_argument('--dry-run', action='store_true', 
                       help='Show what would be changed without modifying files')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Enable verbose output')
    
    args = parser.parse_args()
    
    remover = OpenSSLRemover(dry_run=args.dry_run, verbose=args.verbose)
    success = remover.run()
    
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
