#!/usr/bin/env python3
"""
RPC Documentation Generator for DineroCoin

Fetches RPC method metadata from rpc.discover and generates comprehensive
markdown documentation at docs/RPC_API.md
"""

import json
import subprocess
import sys
from datetime import datetime
from collections import defaultdict

def get_rpc_discover_data(cli_path, rpc_port, datadir):
    """Fetch rpc.discover output from running daemon"""
    try:
        cmd = [cli_path, f"-rpcport={rpc_port}", f"-datadir={datadir}", "rpc.discover"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)

        if result.returncode != 0:
            print(f"Error: Failed to run rpc.discover: {result.stderr}", file=sys.stderr)
            return None

        return json.loads(result.stdout)
    except subprocess.TimeoutExpired:
        print("Error: Command timed out", file=sys.stderr)
        return None
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON from rpc.discover: {e}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return None

def format_parameter(param):
    """Format a parameter for documentation"""
    required = " *(required)*" if param.get("required", False) else " *(optional)*"
    param_type = param.get("type", "any")
    desc = param.get("description", "No description")

    return f"  - **`{param['name']}`** ({param_type}){required}: {desc}"

def format_method(method):
    """Format a single method as markdown"""
    name = method.get("name", "Unknown")
    category = method.get("category", "core")
    description = method.get("description", "*No description available*")

    md = f"### {name}\n\n"
    md += f"**Category:** `{category}`\n\n"
    md += f"{description}\n\n"

    # Parameters
    if "parameters" in method and method["parameters"]:
        md += "**Parameters:**\n\n"
        for param in method["parameters"]:
            md += format_parameter(param) + "\n"
        md += "\n"
    else:
        md += "**Parameters:** None\n\n"

    # Returns
    if "returns" in method:
        ret = method["returns"]
        ret_type = ret.get("type", "any")
        ret_desc = ret.get("description", "")
        md += f"**Returns:** `{ret_type}` - {ret_desc}\n\n"
    else:
        md += "**Returns:** *No return information*\n\n"

    # Help text
    if "help" in method and method["help"]:
        md += f"**Additional Help:**\n\n```\n{method['help']}\n```\n\n"

    # Example
    md += "**Example:**\n\n"
    if method.get("parameters"):
        # Generate example with parameters
        param_examples = []
        for param in method["parameters"]:
            if param["type"] == "string":
                param_examples.append(f'"{param["name"]}_value"')
            elif param["type"] == "number":
                param_examples.append("0")
            elif param["type"] == "boolean":
                param_examples.append("true")
            else:
                param_examples.append(f'"{param["name"]}"')
        md += f"```bash\ndinero-cli {name} {' '.join(param_examples)}\n```\n\n"
    else:
        md += f"```bash\ndinero-cli {name}\n```\n\n"

    md += "---\n\n"
    return md

def generate_documentation(data, output_file):
    """Generate markdown documentation from rpc.discover data"""

    methods = data.get("methods", [])
    count = data.get("count", len(methods))
    version = data.get("version", "unknown")
    schema = data.get("rpc_schema", "unknown")

    # Group methods by category
    by_category = defaultdict(list)
    for method in methods:
        category = method.get("category", "core")
        by_category[category].append(method)

    # Sort categories
    category_order = ["blockchain", "wallet", "network", "mining", "websocket", "discovery", "core"]
    sorted_categories = []
    for cat in category_order:
        if cat in by_category:
            sorted_categories.append(cat)

    # Add any remaining categories
    for cat in sorted(by_category.keys()):
        if cat not in sorted_categories:
            sorted_categories.append(cat)

    # Generate markdown
    md = "# DineroCoin RPC API Reference\n\n"
    md += f"**Auto-generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n"
    md += f"**Total Methods:** {count}\n\n"
    md += f"**Discovery Version:** {version}\n\n"
    md += f"**Schema:** {schema}\n\n"
    md += "---\n\n"

    # Table of contents
    md += "## Table of Contents\n\n"
    for category in sorted_categories:
        md += f"- [{category.title()}](#{category})\n"
    md += "\n---\n\n"

    # Category sections
    for category in sorted_categories:
        cat_methods = by_category[category]
        md += f"## {category.title()}\n\n"
        md += f"**Methods in this category:** {len(cat_methods)}\n\n"

        # Sort methods alphabetically within category
        cat_methods.sort(key=lambda m: m.get("name", ""))

        for method in cat_methods:
            md += format_method(method)

    # Footer
    md += "\n---\n\n"
    md += "## Notes\n\n"
    md += "- This documentation is auto-generated from `rpc.discover`\n"
    md += "- To regenerate: `python3 scripts/generate_rpc_docs.py`\n"
    md += "- For live testing, use: `dinero-cli help <method>` or `dinero-cli <method>`\n"
    md += f"- Documentation generated from daemon version with {count} RPC methods\n"

    # Write to file
    try:
        with open(output_file, 'w') as f:
            f.write(md)
        print(f"✅ Documentation generated: {output_file}")
        print(f"📊 Total methods documented: {count}")
        print(f"📁 Categories: {', '.join(sorted_categories)}")
        return True
    except Exception as e:
        print(f"Error writing to {output_file}: {e}", file=sys.stderr)
        return False

def main():
    # Configuration
    CLI_PATH = "/Users/haydarevich/Documents/DineroCoin/build/dinero-cli"
    RPC_PORT = "19993"
    DATADIR = "/tmp/dinero-doc-gen"
    OUTPUT_FILE = "/Users/haydarevich/Documents/DineroCoin/docs/RPC_API.md"

    print("🔍 Fetching RPC metadata from daemon...")
    data = get_rpc_discover_data(CLI_PATH, RPC_PORT, DATADIR)

    if not data:
        print("❌ Failed to fetch RPC data. Make sure dinerod is running:", file=sys.stderr)
        print(f"   {CLI_PATH.replace('dinero-cli', 'dinerod')} --regtest --rpcport={RPC_PORT} --datadir={DATADIR} --daemon", file=sys.stderr)
        sys.exit(1)

    print(f"📦 Found {data.get('count', 0)} RPC methods")
    print("📝 Generating documentation...")

    if generate_documentation(data, OUTPUT_FILE):
        print(f"✅ Success! Documentation written to: {OUTPUT_FILE}")
        sys.exit(0)
    else:
        print("❌ Failed to generate documentation", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
