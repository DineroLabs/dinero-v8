#!/usr/bin/env python3
"""
JSON Compatibility Fix Script for DineroCoin

This script replaces JsonCpp method calls with nlohmann::json equivalents
throughout the codebase to fix compilation errors.
"""

import os
import re
import sys
from pathlib import Path

# Mapping of JsonCpp methods to nlohmann::json equivalents
JSON_METHOD_REPLACEMENTS = {
    # Type checking methods
    r'\.isObject\(\)': '.is_object()',
    r'\.isArray\(\)': '.is_array()',
    r'\.isString\(\)': '.is_string()',
    r'\.isNumeric\(\)': '.is_number()',
    r'\.isInt\(\)': '.is_number_integer()',
    r'\.isUInt\(\)': '.is_number_unsigned()',
    r'\.isDouble\(\)': '.is_number_float()',
    r'\.isBool\(\)': '.is_boolean()',
    r'\.isNull\(\)': '.is_null()',
    r'\.isMember\(': '.contains(',
    
    # Value extraction methods
    r'\.asString\(\)': '.get<std::string>()',
    r'\.asInt\(\)': '.get<int>()',
    r'\.asUInt\(\)': '.get<unsigned int>()',
    r'\.asInt64\(\)': '.get<int64_t>()',
    r'\.asUInt64\(\)': '.get<uint64_t>()',
    r'\.asDouble\(\)': '.get<double>()',
    r'\.asFloat\(\)': '.get<float>()',
    r'\.asBool\(\)': '.get<bool>()',
    
    # Array/object operations
    r'\.append\(': '.push_back(',
    r'\.size\(\)': '.size()',
    r'\.empty\(\)': '.empty()',
    r'\.clear\(\)': '.clear()',
    
    # Object creation
    r'Json::Value\(Json::objectValue\)': 'nlohmann::json::object()',
    r'Json::Value\(Json::arrayValue\)': 'nlohmann::json::array()',
    r'nlohmann::json\(Json::objectValue\)': 'nlohmann::json::object()',
    r'nlohmann::json\(Json::arrayValue\)': 'nlohmann::json::array()',
    
    # Writer/Reader replacements
    r'Json::StyledWriter\(\)\.write\(': 'dump(',
    r'Json::FastWriter\(\)\.write\(': 'dump(',
    r'Json::Reader\s+(\w+);\s*(\w+)\.parse\(([^,]+),\s*([^)]+)\)': r'try { \4 = nlohmann::json::parse(\3); } catch(...) { return false; }',
    
    # Type constants
    r'Json::UInt': 'unsigned int',
    r'Json::UInt64': 'uint64_t',
    r'Json::Int64': 'int64_t',
    r'Json::arrayValue': 'nlohmann::json::array()',
    r'Json::objectValue': 'nlohmann::json::object()',
    r'Json::nullValue': 'nlohmann::json(nullptr)',
}

# Additional complex patterns that need special handling
COMPLEX_REPLACEMENTS = [
    # Handle Json::Reader parse patterns
    (r'Json::Reader\s+(\w+);\s*if\s*\(\s*\1\.parse\(([^,]+),\s*([^)]+)\)\s*\)', 
     r'try { \3 = nlohmann::json::parse(\2); } catch(...) { /* parse failed */ }'),
    
    # Handle get with default values
    (r'\.get\("([^"]+)",\s*([^)]+)\)\.as(\w+)\(\)', 
     r'.value("\1", \2)'),
    
    # Handle array indexing with asType calls
    (r'\[(\d+)\]\.as(\w+)\(\)', 
     r'[\1].get<\2>()'),
]

def should_skip_file(filepath):
    """Check if file should be skipped"""
    skip_patterns = [
        '/node_modules/',
        '/.git/',
        '/build/',
        '/cmake/',
        '/third_party/',
        '/external/',
        '.git',
        '.DS_Store',
        '__pycache__',
    ]
    
    skip_extensions = [
        '.o', '.a', '.so', '.dylib', '.dll',
        '.exe', '.bin', '.obj', '.lib',
        '.png', '.jpg', '.jpeg', '.gif',
        '.pdf', '.zip', '.tar', '.gz',
        '.log', '.tmp', '.bak',
    ]
    
    filepath_str = str(filepath)
    
    # Skip by pattern
    for pattern in skip_patterns:
        if pattern in filepath_str:
            return True
    
    # Skip by extension
    for ext in skip_extensions:
        if filepath_str.endswith(ext):
            return True
    
    return False

def fix_json_methods_in_file(filepath):
    """Fix JSON method calls in a single file"""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        original_content = content
        changes_made = 0
        
        # Apply complex replacements first
        for pattern, replacement in COMPLEX_REPLACEMENTS:
            new_content = re.sub(pattern, replacement, content, flags=re.MULTILINE)
            if new_content != content:
                changes_made += len(re.findall(pattern, content, flags=re.MULTILINE))
                content = new_content
        
        # Apply simple method replacements
        for pattern, replacement in JSON_METHOD_REPLACEMENTS.items():
            new_content = re.sub(pattern, replacement, content)
            if new_content != content:
                changes_made += len(re.findall(pattern, content))
                content = new_content
        
        # Write back if changes were made
        if content != original_content:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            return changes_made
        
        return 0
        
    except Exception as e:
        print(f"Error processing {filepath}: {e}")
        return 0

def find_cpp_files(root_dir):
    """Find all C++ source files"""
    cpp_extensions = ['.cpp', '.hpp', '.h', '.cc', '.cxx', '.c++']
    cpp_files = []
    
    for root, dirs, files in os.walk(root_dir):
        # Skip certain directories
        dirs[:] = [d for d in dirs if not should_skip_file(Path(root) / d)]
        
        for file in files:
            filepath = Path(root) / file
            if not should_skip_file(filepath) and filepath.suffix in cpp_extensions:
                cpp_files.append(filepath)
    
    return cpp_files

def main():
    if len(sys.argv) > 1:
        root_dir = sys.argv[1]
    else:
        root_dir = '/Users/haydarevich/Documents/DineroCoin'
    
    if not os.path.exists(root_dir):
        print(f"Error: Directory {root_dir} does not exist")
        return 1
    
    print(f"Fixing JSON compatibility issues in {root_dir}")
    print("=" * 60)
    
    cpp_files = find_cpp_files(root_dir)
    print(f"Found {len(cpp_files)} C++ files to process")
    
    total_changes = 0
    files_modified = 0
    
    for filepath in cpp_files:
        changes = fix_json_methods_in_file(filepath)
        if changes > 0:
            files_modified += 1
            total_changes += changes
            print(f"✓ {filepath.relative_to(root_dir)}: {changes} changes")
    
    print("=" * 60)
    print(f"Summary:")
    print(f"  Files processed: {len(cpp_files)}")
    print(f"  Files modified: {files_modified}")
    print(f"  Total changes: {total_changes}")
    
    if total_changes > 0:
        print("\n✓ JSON compatibility fixes applied successfully!")
        print("You should now be able to build the project.")
    else:
        print("\n• No JSON compatibility issues found.")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
