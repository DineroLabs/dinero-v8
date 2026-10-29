#!/usr/bin/env python3
"""Deploy Qt using a private plugin tree; never modify the installed toolkit."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


# Preserve the existing packaging policy, including exclusion of whole plugin
# types. These plugins pull unused PDF/SVG/virtual-keyboard dependencies into Qt.
EXCLUDED_PLUGINS = {
    'imageformats/libqpdf.dylib',
    'imageformats/libqsvg.dylib',
    'iconengines',
    'platforminputcontexts',
}
QT_PATH_KEYS = {
    'QT_INSTALL_PREFIX': 'Prefix',
    'QT_INSTALL_DOCS': 'Documentation',
    'QT_INSTALL_HEADERS': 'Headers',
    'QT_INSTALL_LIBS': 'Libraries',
    'QT_INSTALL_LIBEXECS': 'LibraryExecutables',
    'QT_INSTALL_BINS': 'Binaries',
    'QT_INSTALL_PLUGINS': 'Plugins',
    'QT_INSTALL_QML': 'QmlImports',
    'QT_INSTALL_ARCHDATA': 'ArchData',
    'QT_INSTALL_DATA': 'Data',
    'QT_INSTALL_TRANSLATIONS': 'Translations',
    'QT_INSTALL_EXAMPLES': 'Examples',
    'QT_INSTALL_TESTS': 'Tests',
}


def query_paths(qtpaths: Path) -> dict[str, str]:
    result = subprocess.run([str(qtpaths), '--query'], capture_output=True,
                            text=True, check=True)
    paths = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition(':')
        if separator and key in QT_PATH_KEYS:
            if key in paths or not value or not Path(value).is_absolute():
                raise ValueError(f'Invalid Qt path for {key}')
            paths[key] = value
    missing = QT_PATH_KEYS.keys() - paths.keys()
    if missing:
        raise ValueError(f'Qt path query missing: {", ".join(sorted(missing))}')
    return paths


def ini_path(value: str) -> str:
    # QSettings INI strings need quoting for commas and escaping for backslashes
    # and quotes. Reject control characters instead of creating extra entries.
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError('Control character in Qt path')
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def deploy(macdeployqt: Path, app_bundle: Path) -> int:
    macdeployqt = macdeployqt.resolve(strict=True)
    app_bundle = app_bundle.resolve(strict=True)
    # Locate the companion in the same toolkit, not another qtpaths on PATH.
    qtpaths = macdeployqt.with_name('qtpaths')
    paths = query_paths(qtpaths)
    plugin_root = Path(paths['QT_INSTALL_PLUGINS'])
    if not plugin_root.is_dir() or not app_bundle.is_dir():
        raise ValueError('Qt plugins and app bundle must be existing directories')

    with tempfile.TemporaryDirectory(prefix='dinero-qt-deploy-') as temporary:
        root = Path(temporary)
        binaries = root / 'bin'
        binaries.mkdir()
        # The copied tool's @loader_path/../lib still resolves its original Qt
        # libraries. Only reads use this link; plugins are real private copies.
        (root / 'lib').symlink_to(paths['QT_INSTALL_LIBS'], target_is_directory=True)
        isolated_tool = binaries / 'macdeployqt'
        isolated_query = binaries / 'qtpaths'
        shutil.copy2(macdeployqt, isolated_tool)
        shutil.copy2(qtpaths, isolated_query)

        def excluded(directory: str, names: list[str]) -> list[str]:
            relative = Path(directory).relative_to(plugin_root)
            return [name for name in names
                    if (relative / name).as_posix() in EXCLUDED_PLUGINS
                    or name.endswith('.dineroqt-hidden')]

        isolated_plugins = root / 'plugins'
        # Dereference toolkit symlinks while copying, so deployment cannot write
        # through any plugin link into the installed toolkit.
        shutil.copytree(plugin_root, isolated_plugins, ignore=excluded)
        isolated_paths = dict(paths, QT_INSTALL_PLUGINS=str(isolated_plugins))
        config = '[Paths]\n' + ''.join(
            f'{QT_PATH_KEYS[key]} = {ini_path(value)}\n'
            for key, value in isolated_paths.items())
        (binaries / 'qt.conf').write_text(config, encoding='utf-8')
        # Check the real Qt configuration parser before allowing deployment.
        if query_paths(isolated_query) != isolated_paths:
            raise ValueError('Isolated Qt configuration did not preserve toolkit paths')

        # Qt 6.9.1 rejects -no-codesign; newer tools support it. Final signing is
        # owned by sign_macos_bundle.py after dependency rewriting/removal.
        help_result = subprocess.run([str(isolated_tool), '-help'],
                                     capture_output=True, text=True, check=False)
        help_text = help_result.stdout + help_result.stderr
        # macdeployqt 6.11 returns 1 for its normal help output. Accept that
        # convention only when the expected usage/options were actually read.
        if (help_result.returncode not in (0, 1) or "Usage:" not in help_text
                or "-always-overwrite" not in help_text):
            raise ValueError("Could not inspect macdeployqt options")
        args = [str(isolated_tool), str(app_bundle), '-always-overwrite']
        if '-no-codesign' in help_text:
            args.append('-no-codesign')
        return subprocess.run(args, check=False).returncode


def main() -> int:
    if len(sys.argv) != 3:
        print('usage: run_macdeployqt_sanitized.py <macdeployqt> <app_bundle>', file=sys.stderr)
        return 2
    try:
        return deploy(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'macdeployqt deployment failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
