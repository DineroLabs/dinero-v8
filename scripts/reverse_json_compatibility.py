#!/usr/bin/env python3
"""
Reverse JSON compatibility script for DineroCoin
Reverts nlohmann::json changes back to original format
"""

import os
import re
import sys
from pathlib import Path

def reverse_json_replacements():
    """Define reverse mappings from nlohmann::json back to original format"""
    return [
        # Reverse nlohmann::json method calls to original
        (r'\.is_array\(\)', r'.isArray()'),
        (r'\.is_object\(\)', r'.isObject()'),
        (r'\.is_string\(\)', r'.isString()'),
        (r'\.is_number\(\)', r'.isNumber()'),
        (r'\.is_boolean\(\)', r'.isBool()'),
        (r'\.is_null\(\)', r'.isNull()'),
        (r'\.is_number_integer\(\)', r'.isInt()'),
        (r'\.is_number_float\(\)', r'.isDouble()'),
        
        # Reverse getter methods
        (r'\.get<std::string>\(\)', r'.asString()'),
        (r'\.get<int>\(\)', r'.asInt()'),
        (r'\.get<double>\(\)', r'.asDouble()'),
        (r'\.get<float>\(\)', r'.asFloat()'),
        (r'\.get<bool>\(\)', r'.asBool()'),
        (r'\.get<uint64_t>\(\)', r'.asUInt64()'),
        (r'\.get<int64_t>\(\)', r'.asInt64()'),
        
        # Reverse object/array creation
        (r'nlohmann::json::object\(\)', r'Json::Value(Json::objectValue)'),
        (r'nlohmann::json::array\(\)', r'Json::Value(Json::arrayValue)'),
        (r'nlohmann::json\(nullptr\)', r'Json::Value(Json::nullValue)'),
        
        # Reverse serialization
        (r'\.dump\(\)', r'Json::FastWriter().write()'),
        (r'\.dump\(2\)', r'Json::StyledWriter().write()'),
        
        # Reverse parsing
        (r'nlohmann::json::parse\(([^)]+)\)', r'Json::Reader().parse(\1, result)'),
        
        # Reverse type declarations
        (r'nlohmann::json', r'Json::Value'),
        
        # Reverse includes
        (r'#include <nlohmann/json\.hpp>', r'#include <json/json.h>'),
        (r'#include "compat/json_compat\.h"', r'#include <json/json.h>'),
        
        # Reverse namespace usage
        (r'using namespace din::json;', r'// JSON compatibility restored'),
        
        # Reverse adapter function calls back to direct JSON usage
        (r'din::json::parse\(([^)]+)\)', r'Json::Reader().parse(\1, result)'),
        (r'din::json::dumpCompact\(([^)]+)\)', r'Json::FastWriter().write(\1)'),
        (r'din::json::dumpPretty\(([^)]+)\)', r'Json::StyledWriter().write(\1)'),
        (r'din::json::createObject\(\)', r'Json::Value(Json::objectValue)'),
        (r'din::json::createArray\(\)', r'Json::Value(Json::arrayValue)'),
        (r'din::json::createNull\(\)', r'Json::Value(Json::nullValue)'),
        (r'din::json::contains\(([^,]+),\s*([^)]+)\)', r'\1.isMember(\2)'),
        (r'din::json::getValue\(([^,]+),\s*([^,]+),\s*([^)]+)\)', r'\1.get(\2, \3)'),
        (r'din::json::objectSet\(([^,]+),\s*([^,]+),\s*([^)]+)\)', r'\1[\2] = \3'),
        (r'din::json::objectGet\(([^,]+),\s*([^)]+)\)', r'\1[\2]'),
        (r'din::json::arraySize\(([^)]+)\)', r'\1.size()'),
        (r'din::json::arrayGet\(([^,]+),\s*([^)]+)\)', r'\1[\2]'),
        (r'din::json::arrayPush\(([^,]+),\s*([^)]+)\)', r'\1.append(\2)'),
        (r'din::json::isObject\(([^)]+)\)', r'\1.isObject()'),
        (r'din::json::isArray\(([^)]+)\)', r'\1.isArray()'),
        (r'din::json::isString\(([^)]+)\)', r'\1.isString()'),
        (r'din::json::isNumber\(([^)]+)\)', r'\1.isNumeric()'),
        (r'din::json::isBool\(([^)]+)\)', r'\1.isBool()'),
        (r'din::json::isNull\(([^)]+)\)', r'\1.isNull()'),
        
        # Reverse Value type back to Json::Value
        (r'din::json::Value', r'Json::Value'),
        (r'Value([^a-zA-Z_])', r'Json::Value\1'),
        
        # Reverse tryParse back to Reader pattern
        (r'din::json::tryParse\(([^,]+),\s*([^)]+)\)', r'Json::Reader().parse(\1, \2)'),
    ]

def process_file(file_path, replacements, dry_run=False):
    """Process a single file with the given replacements"""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        original_content = content
        changes_made = 0
        
        # Apply all replacements
        for pattern, replacement in replacements:
            new_content = re.sub(pattern, replacement, content)
            if new_content != content:
                changes_made += len(re.findall(pattern, content))
                content = new_content
        
        if changes_made > 0:
            print(f"{'[DRY RUN] ' if dry_run else ''}Processing {file_path}: {changes_made} changes")
            
            if not dry_run:
                # Write the modified content back
                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(content)
        
        return changes_made
        
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return 0

def find_cpp_files(root_dir):
    """Find all C++ source and header files"""
    cpp_extensions = {'.cpp', '.hpp', '.h', '.cc', '.cxx'}
    cpp_files = []
    
    for root, dirs, files in os.walk(root_dir):
        # Skip build directories and third-party code
        dirs[:] = [d for d in dirs if not d.startswith('.') and d not in ['build', 'build-debug', '_deps', 'third_party', 'node_modules']]
        
        for file in files:
            if any(file.endswith(ext) for ext in cpp_extensions):
                cpp_files.append(os.path.join(root, file))
    
    return cpp_files

def main():
    """Main function"""
    if len(sys.argv) > 1 and sys.argv[1] == '--dry-run':
        dry_run = True
        print("DRY RUN MODE - No files will be modified")
    else:
        dry_run = False
    
    # Get the DineroCoin root directory
    script_dir = Path(__file__).parent
    dinero_root = script_dir.parent
    
    print(f"Reversing JSON compatibility changes in: {dinero_root}")
    
    # Get replacement patterns
    replacements = reverse_json_replacements()
    
    # Find all C++ files
    cpp_files = find_cpp_files(dinero_root)
    
    total_changes = 0
    files_modified = 0
    
    print(f"Found {len(cpp_files)} C++ files to process")
    
    # Process each file
    for file_path in cpp_files:
        changes = process_file(file_path, replacements, dry_run)
        if changes > 0:
            total_changes += changes
            files_modified += 1
    
    print(f"\nReverse JSON compatibility complete!")
    print(f"Files modified: {files_modified}")
    print(f"Total changes: {total_changes}")
    
    if dry_run:
        print("\nThis was a dry run. Use without --dry-run to apply changes.")
    else:
        print("\nJSON compatibility has been reversed back to original format.")
        print("You may need to:")
        print("1. Install JsonCpp library if not present")
        print("2. Update CMakeLists.txt to link JsonCpp instead of nlohmann/json")
        print("3. Remove the compat/json_compat.h adapter layer")

if __name__ == "__main__":
    main()
