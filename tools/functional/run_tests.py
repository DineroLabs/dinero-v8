#!/usr/bin/env python3
"""
DineroCoin Functional Test Runner

Discovers and runs all functional tests.

Usage:
    python run_tests.py                    # Run all tests
    python run_tests.py test_basic         # Run specific test
    python run_tests.py --list             # List all tests
    python run_tests.py --parallel 4       # Run 4 tests in parallel
"""

import os
import sys
import subprocess
import argparse
import time
import glob
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Tuple


# Colors for terminal output
class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def find_tests(test_dir: Path, pattern: str = "test_*.py") -> List[Path]:
    """Find all test files"""
    tests = list(test_dir.glob(pattern))
    # Exclude __pycache__ and framework
    tests = [t for t in tests if '__pycache__' not in str(t)]
    return sorted(tests)


def run_test(test_path: Path, dinerod: str, verbose: bool) -> Tuple[str, bool, str, float]:
    """
    Run a single test.

    Returns: (test_name, passed, output, duration)
    """
    start = time.time()
    test_name = test_path.stem

    cmd = [
        sys.executable,
        str(test_path),
        f"--dinerod={dinerod}",
    ]

    if verbose:
        cmd.append("--verbose")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,  # 5 minute timeout per test
            cwd=str(test_path.parent),
        )

        duration = time.time() - start
        passed = result.returncode == 0
        skipped = result.returncode == 77

        output = result.stdout + result.stderr

        if skipped:
            return (test_name, None, output, duration)  # None = skipped
        return (test_name, passed, output, duration)

    except subprocess.TimeoutExpired:
        duration = time.time() - start
        return (test_name, False, "TIMEOUT after 300s", duration)

    except Exception as e:
        duration = time.time() - start
        return (test_name, False, str(e), duration)


def main():
    parser = argparse.ArgumentParser(description="Run DineroCoin functional tests")

    parser.add_argument(
        "tests",
        nargs="*",
        help="Specific tests to run (default: all)"
    )
    parser.add_argument(
        "--dinerod",
        default="",
        help="Path to dinerod binary"
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available tests"
    )
    parser.add_argument(
        "--parallel", "-j",
        type=int,
        default=1,
        help="Number of tests to run in parallel"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output"
    )

    args = parser.parse_args()

    # Find test directory
    test_dir = Path(__file__).parent

    # Find dinerod
    dinerod = args.dinerod
    if not dinerod:
        candidates = [
            test_dir / "../../build/dinerod",
            Path.home() / "Documents/DineroCoin/build/dinerod",
        ]
        for c in candidates:
            if c.exists():
                dinerod = str(c.resolve())
                break

    if not dinerod or not os.path.exists(dinerod):
        print(f"{Colors.RED}Error: dinerod not found. Use --dinerod=/path/to/dinerod{Colors.RESET}")
        sys.exit(1)

    # Find tests
    all_tests = find_tests(test_dir)

    if args.list:
        print("Available tests:")
        for t in all_tests:
            print(f"  {t.stem}")
        sys.exit(0)

    # Filter tests if specific ones requested
    if args.tests:
        tests_to_run = []
        for name in args.tests:
            matches = [t for t in all_tests if name in t.stem]
            tests_to_run.extend(matches)
        if not tests_to_run:
            print(f"{Colors.RED}No matching tests found{Colors.RESET}")
            sys.exit(1)
    else:
        tests_to_run = all_tests

    print(f"{Colors.BOLD}DineroCoin Functional Tests{Colors.RESET}")
    print(f"Using dinerod: {dinerod}")
    print(f"Running {len(tests_to_run)} test(s)...")
    print()

    # Run tests
    results = []
    start_time = time.time()

    if args.parallel > 1:
        with ThreadPoolExecutor(max_workers=args.parallel) as executor:
            futures = {
                executor.submit(run_test, t, dinerod, args.verbose): t
                for t in tests_to_run
            }
            for future in as_completed(futures):
                results.append(future.result())
    else:
        for test_path in tests_to_run:
            result = run_test(test_path, dinerod, args.verbose)
            results.append(result)

            # Print result immediately
            name, passed, output, duration = result
            if passed is None:
                status = f"{Colors.YELLOW}SKIP{Colors.RESET}"
            elif passed:
                status = f"{Colors.GREEN}PASS{Colors.RESET}"
            else:
                status = f"{Colors.RED}FAIL{Colors.RESET}"

            print(f"  {status} {name} ({duration:.1f}s)")

            if not passed and passed is not None and args.verbose:
                print(output)

    total_time = time.time() - start_time

    # Summary
    print()
    print(f"{Colors.BOLD}{'='*60}{Colors.RESET}")

    passed = sum(1 for _, p, _, _ in results if p is True)
    failed = sum(1 for _, p, _, _ in results if p is False)
    skipped = sum(1 for _, p, _, _ in results if p is None)

    print(f"Passed:  {Colors.GREEN}{passed}{Colors.RESET}")
    print(f"Failed:  {Colors.RED}{failed}{Colors.RESET}")
    print(f"Skipped: {Colors.YELLOW}{skipped}{Colors.RESET}")
    print(f"Time:    {total_time:.1f}s")

    if failed > 0:
        print()
        print(f"{Colors.RED}Failed tests:{Colors.RESET}")
        for name, p, output, _ in results:
            if p is False:
                print(f"  - {name}")
                if args.verbose:
                    for line in output.split('\n')[-10:]:
                        print(f"      {line}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == '__main__':
    main()
