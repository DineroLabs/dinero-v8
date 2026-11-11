#!/usr/bin/env python3
"""Require a bounded, verified server-service uninstall before deleting metadata.

The Windows installer smoke runs the real uninstaller. This source check also
guards the slow-stop path that is hard to force reliably on a hosted runner.
"""

import argparse
from pathlib import Path
import re


def validate(source: str) -> None:
    match = re.search(r'Section "Uninstall"(.*?)SectionEnd', source, re.S)
    if not match:
        raise ValueError("server uninstaller section is missing")
    commands = "\n".join(line.strip() for line in match.group(1).splitlines()
                         if line.strip() and not line.lstrip().startswith(";"))
    if re.search(r'(?m)^Sleep\s+30000\s*$', commands):
        raise ValueError("fixed 30-second sleep can finish before the service exits")
    required = (
        'service_wait:',
        'sc.exe" query ${SVC_NAME}',
        'Get-Service -Name ${SVC_NAME}',
        'Goto service_wait',
        'service_stopped:',
        'sc.exe" delete ${SVC_NAME}',
        'service_delete_wait:',
        'Goto service_delete_wait',
        'service_deleted:',
        'remove_install_dir:',
        'Delete "$INSTDIR\\dinerod.exe"',
        'RMDir /r "$INSTDIR"',
        'IfFileExists "$INSTDIR" 0 files_removed',
        'Goto remove_install_dir',
        'files_removed:',
        'DeleteRegKey HKLM "${APP_REGKEY}"',
    )
    cursor = 0
    for item in required:
        position = commands.find(item, cursor)
        if position < 0:
            raise ValueError(f"server uninstall lacks ordered stop/removal check: {item}")
        cursor = position + len(item)
    if ('-NoProfile -NonInteractive' not in commands or '-ErrorAction Stop' not in commands or
            'catch { exit 2 }' not in commands or '${AndIf} $R0 != 1072' not in commands):
        raise ValueError('service status must be locale-independent and tolerate already-marked deletion')
    if commands.count('SetErrorLevel') < 3 or commands.count('Abort "') < 3:
        raise ValueError("service or file-removal timeout can report success")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("script", type=Path)
    args = parser.parse_args()
    try:
        validate(args.script.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Windows server uninstall contract failed: {exc}\n")
    print("Windows server uninstall contract passed")
