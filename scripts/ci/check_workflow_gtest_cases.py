#!/usr/bin/env python3
"""Preflight literal workflow case gates against compiled GTest inventories.

This reads case names, not test results. Runtime execution/count assertions stay
in the workflow. Unsupported/non-GTest predicates are left to those assertions.
No workflow Python is executed by this checker.
"""
import argparse
import ast
import fnmatch
import json
from pathlib import Path
import re
import shlex
import subprocess

OK_REGEX = r"\[       OK \] "


class UnknownExpression(Exception):
    pass


def value(node, env, cases):
    """Return (reference value, depends on compiled case names)."""
    if isinstance(node, ast.Name):
        if node.id not in env:
            raise UnknownExpression(node.id)
        return env[node.id]
    try:
        return ast.literal_eval(node), False
    except (ValueError, TypeError):
        pass
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        a, x = value(node.left, env, cases)
        b, y = value(node.right, env, cases)
        return a + b, x or y
    if isinstance(node, ast.JoinedStr):
        parts = [value(n.value if isinstance(n, ast.FormattedValue) else n, env, cases)
                 for n in node.values]
        return "".join(str(x) for x, _ in parts), any(y for _, y in parts)
    if isinstance(node, ast.ListComp) and len(node.generators) == 1:
        gen = node.generators[0]
        if not isinstance(gen.target, ast.Name) or gen.ifs or gen.is_async:
            raise UnknownExpression("comprehension")
        items, dependent = value(gen.iter, env, cases)
        result = []
        for item in items:
            local = dict(env)
            local[gen.target.id] = item, dependent
            entry, dep = value(node.elt, local, cases)
            result.append(entry)
            dependent |= dep
        return result, dependent
    if isinstance(node, ast.Call):
        if isinstance(node.func, ast.Name) and node.func.id in {"sorted", "set", "list", "tuple", "len"}:
            data, dep = value(node.args[0], env, cases)
            operation = {"sorted": sorted, "set": set, "list": list, "tuple": tuple, "len": len}[node.func.id]
            return operation(data), dep
        if isinstance(node.func, ast.Attribute) and node.func.attr == "findall":
            pattern, _ = value(node.args[0], env, cases)
            if isinstance(pattern, str) and pattern.startswith(OK_REGEX):
                pattern = pattern[len(OK_REGEX):]
                if pattern.endswith(r" \("):
                    pattern = pattern[:-len(r" \(")]
                return re.findall(pattern, "\n".join(sorted(cases))), True
    raise UnknownExpression(ast.dump(node))


def selected_cases(prefix, tests, binaries):
    step = prefix.rsplit("      - name:", 1)[-1]
    commands = [line.strip() for line in step.splitlines()
                if line.strip().startswith("ctest --test-dir build-orchard-root ") and "-R " in line]
    if not commands:
        return []
    args = shlex.split(commands[-1])
    regex = args[args.index("-R") + 1]
    if "${" in regex or "$(" in regex:
        return []
    result = []
    for test in tests:
        command = test.get("command", [])
        if not re.search(regex, test["name"]) or not command or command[0] not in binaries:
            continue
        pattern = next((arg.split("=", 1)[1] for arg in command if arg.startswith("--gtest_filter=")), "*")
        positive, _, negative = pattern.partition("-")
        for case in binaries[command[0]]:
            if any(fnmatch.fnmatchcase(case, part) for part in positive.split(":")) and not (
                    negative and any(fnmatch.fnmatchcase(case, part) for part in negative.split(":"))):
                result.append(case)
    return result


def audit(source, tests, binaries):
    failures = []
    comparisons = 0
    for block in re.finditer(r"          python3 - <<'([A-Z_]+)'\n(.*?)          \1", source, re.S):
        cases = selected_cases(source[:block.start()], tests, binaries)
        if not cases:
            continue
        code = "\n".join(line[10:] if line.startswith("          ") else line for line in block[2].splitlines())
        tree = ast.parse(code)
        base = source[:block.start()].count("\n")

        def walk(nodes, env):
            nonlocal comparisons
            for node in nodes:
                if isinstance(node, ast.Assign):
                    try:
                        data = value(node.value, env, cases)
                    except (UnknownExpression, TypeError, ValueError, KeyError):
                        continue
                    for target in node.targets:
                        if isinstance(target, ast.Name):
                            env[target.id] = data
                elif isinstance(node, ast.For) and isinstance(node.target, ast.Name):
                    try:
                        entries, dep = value(node.iter, env, cases)
                    except (UnknownExpression, TypeError, ValueError, KeyError):
                        continue
                    for entry in entries:
                        local = dict(env)
                        local[node.target.id] = entry, dep
                        walk(node.body, local)
                elif isinstance(node, (ast.Assert, ast.If)):
                    expression = node.test
                    if isinstance(expression, ast.Compare) and len(expression.ops) == 1 and (
                            isinstance(node, ast.Assert) and isinstance(expression.ops[0], ast.Eq) or
                            isinstance(node, ast.If) and isinstance(expression.ops[0], ast.NotEq)):
                        try:
                            a, x = value(expression.left, env, cases)
                            b, y = value(expression.comparators[0], env, cases)
                        except (UnknownExpression, TypeError, ValueError, KeyError):
                            pass
                        else:
                            if x or y:
                                comparisons += 1
                                if a != b:
                                    failures.append(f"line {base + node.lineno}: compiled cases {a!r} differ from gate {b!r}")
        walk(tree.body, {})
    return failures, comparisons


def list_cases(binary):
    output = subprocess.check_output([binary, "--gtest_list_tests"], text=True, timeout=10)
    result = []
    suite = None
    for line in output.splitlines():
        clean = line.split(" #", 1)[0].strip()
        if line and not line[0].isspace() and clean.endswith("."):
            suite = clean[:-1]
        elif line.startswith("  ") and suite and clean:
            result.append(suite + "." + clean)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--workflow", required=True)
    args = parser.parse_args()
    inventory = json.loads(subprocess.check_output(
        ["ctest", "--test-dir", args.build_dir, "--show-only=json-v1"], text=True))["tests"]
    executables = sorted({test["command"][0] for test in inventory if test.get("command") and
                          any(arg.startswith("--gtest_filter=") for arg in test["command"])})
    # Some later feature targets are not built at the early gate. Their runtime
    # checks still execute later; this preflight only covers available binaries.
    binaries = {exe: list_cases(exe) for exe in executables if Path(exe).is_file()}
    failures, comparisons = audit(Path(args.workflow).read_text(), inventory, binaries)
    if not comparisons:
        raise SystemExit("No compiled case gates audited")
    for failure in failures:
        print(failure)
    print(f"Compiled inventory: {comparisons} equality gates; "
          f"{len(binaries)} available binaries; no tests executed by preflight")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
