# Isolated Qt plugin deployment

The macOS deployment helper previously renamed plugin files and directories in
`/opt/homebrew/share/qt/plugins`, deleted stale `.dineroqt-hidden` paths, and ignored
restoration errors. An interrupted deployment could therefore leave the shared
Qt installation modified. No installed-plugin damage is claimed as reproduced.

The replacement queries the companion `qtpaths` from the selected deployment
tool's own installation. It copies both tools and a filtered plugin tree into a
private temporary directory. A local `qt.conf` preserves the toolkit paths and
overrides only Plugins; the copied query tool must resolve exactly those paths
before deployment starts. A library link supports the tool's loader-relative Qt
framework reference. Plugin symlinks are dereferenced into private copies.

The existing exclusions remain: PDF/SVG image plugins, icon engines and platform
input contexts. Old hidden backup entries are excluded from the copy and left
untouched in the installation. No installed toolkit file is renamed or deleted.
Qt path strings are quoted and escaped, and control characters are refused.

The deployment result is propagated. Temporary files are removed on ordinary
success and exception paths; forcible termination can leave a private temporary
directory, without requiring restoration of the shared toolkit. Final signing
remains in the existing separate signing helper. Qt versions without
`-no-codesign` keep the existing compatibility behavior. Qt 6.11's status 1 for
normal help is accepted only with valid usage/options text.

Qualification: eight offline Python methods cover original preservation, copied
plugin filtering, failed deployment, copy failure, bad resolved configuration,
old/new signing-option behavior, failed help, and malformed query/INI paths.
The native packaging check uses a relocated copy of an already qualified Mac
bundle with existing frameworks retained and plugins removed before deployment.
It does not rebuild production C++, start a GUI/node or open sockets. Results
of native deployment, final signing and early-exit capability inspection must
be checked in their actual terminal receipts; this document is not that receipt.
This is not final installer/platform/release qualification.

Primary references:
- https://doc.qt.io/qt-6/qt-conf.html
- https://raw.githubusercontent.com/qt/qtbase/v6.11.0/src/tools/macdeployqt/macdeployqt/main.cpp
- https://raw.githubusercontent.com/qt/qtbase/v6.11.0/src/tools/macdeployqt/shared/shared.cpp
