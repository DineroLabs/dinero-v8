# Sparkle Auto-Update Integration Guide for Dinero

This document explains how Sparkle auto-updates are integrated into Dinero and how to use the system for releasing updates.

## Overview

Sparkle is the industry-standard auto-update framework for macOS applications, used by thousands of professional apps including Visual Studio Code, Transmit, HandBrake, and 1Password.

### What Sparkle Provides

- **Automatic Update Checking**: Checks for new versions on startup or manually
- **Secure Updates**: Cryptographically signed updates with EdDSA signatures
- **Native macOS UI**: Standard system dialogs for update prompts
- **Delta Updates**: Optional smaller downloads (only the changes)
- **Safe Installation**: Automatic backup and rollback on failure

## Integration Status

### ✅ Completed

1. **Sparkle Framework Downloaded** (v2.6.4)
   - Location: `/third_party/Sparkle.framework`
   - Includes signing tools in `/third_party/bin/`

2. **CMakeLists.txt Updated**
   - Sparkle framework detection added
   - Automatic linking on macOS
   - `HAVE_SPARKLE` compile definition

### 📝 TODO: Code Implementation

The following code changes need to be made to the GUI to activate Sparkle:

---

## Step 1: Add Sparkle Headers to main.cpp

**File**: `gui/src/main.cpp`

```cpp
#include <QApplication>
#include "mainwindow.h"

#ifdef HAVE_SPARKLE
#  ifdef __OBJC__
#    import <Sparkle/Sparkle.h>
#  else
#    include <objc/objc-runtime.h>  // For non-Objective-C++ files
#  endif
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

#ifdef HAVE_SPARKLE
    // Initialize Sparkle updater on macOS
    // This starts automatic update checking
    #ifdef __OBJC__
    [[SPUStandardUpdaterController sharedUpdaterController] startUpdater];
    #endif
#endif

    MainWindow w;
    w.show();

    return app.exec();
}
```

**Note**: You may need to rename `main.cpp` to `main.mm` for Objective-C++ compilation on macOS.

---

## Step 2: Add "Check for Updates" Menu Item

**File**: `gui/src/mainwindow.cpp`

Add to the existing menu bar:

```cpp
#ifdef HAVE_SPARKLE
#  ifdef __OBJC__
#    import <Sparkle/Sparkle.h>
#  endif
#endif

void MainWindow::createMenuBar()
{
    // Existing menu code...

#ifdef HAVE_SPARKLE
    // Add "Help" menu with "Check for Updates"
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));

    QAction *checkForUpdatesAction = helpMenu->addAction(tr("Check for Updates..."));
    connect(checkForUpdatesAction, &QAction::triggered, this, &MainWindow::checkForUpdates);
#endif
}

#ifdef HAVE_SPARKLE
void MainWindow::checkForUpdates()
{
#  ifdef __OBJC__
    [[SPUStandardUpdaterController sharedUpdaterController] checkForUpdates:nil];
#  endif
}
#endif
```

**File**: `gui/src/mainwindow.h`

Add to class definition:

```cpp
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
#ifdef HAVE_SPARKLE
    void checkForUpdates();
#endif
    // ... existing slots
};
```

---

## Step 3: Create Info.plist for macOS App Bundle

**File**: `gui/Info.plist.in` (new file)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>Dinero</string>

    <key>CFBundleIdentifier</key>
    <string>com.dinero-coin.Dinero</string>

    <key>CFBundleName</key>
    <string>Dinero</string>

    <key>CFBundleDisplayName</key>
    <string>Dinero</string>

    <key>CFBundleVersion</key>
    <string>@DINERO_VERSION@</string>

    <key>CFBundleShortVersionString</key>
    <string>@DINERO_VERSION@</string>

    <key>CFBundlePackageType</key>
    <string>APPL</string>

    <key>CFBundleIconFile</key>
    <string>AppIcon</string>

    <!-- Sparkle Configuration -->
    <key>SUFeedURL</key>
    <string>https://updates.dinero-coin.com/appcast.xml</string>

    <key>SUPublicEDKey</key>
    <string>@SPARKLE_PUBLIC_KEY@</string>

    <key>SUEnableAutomaticChecks</key>
    <true/>

    <key>SUScheduledCheckInterval</key>
    <integer>86400</integer>  <!-- Check daily -->

    <key>SUAllowsAutomaticUpdates</key>
    <true/>
</dict>
</plist>
```

---

## Step 4: Update CMakeLists.txt for Info.plist

Add to `gui/CMakeLists.txt`:

```cmake
if(APPLE AND HAVE_SPARKLE)
  # Configure Info.plist with version and Sparkle public key
  set(DINERO_VERSION "0.1.0")
  set(SPARKLE_PUBLIC_KEY "PASTE_PUBLIC_KEY_HERE")  # Generated in next step

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    ${CMAKE_BINARY_DIR}/gui/Info.plist
    @ONLY
  )

  # Set the Info.plist for the app bundle
  set_target_properties(dinero-qt PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/gui/Info.plist
  )
endif()
```

---

## Step 5: Generate EdDSA Signing Keys

Run once to generate your signing key pair:

```bash
cd /Users/haydarevich/Documents/DineroCoin/third_party
./bin/generate_keys
```

This creates two files:
- `dsa_priv.pem` - **KEEP SECRET!** Used to sign releases
- `dsa_pub.pem` - Public key embedded in Info.plist

**Copy the public key** from `dsa_pub.pem` and paste it into:
1. `gui/CMakeLists.txt` → `SPARKLE_PUBLIC_KEY`
2. Or directly into `Info.plist` → `<key>SUPublicEDKey</key>`

**⚠️ CRITICAL**: Store `dsa_priv.pem` securely. Anyone with this key can sign fake updates!

---

## Step 6: Create Update Feed (appcast.xml)

**File**: `appcast.xml` (host on your server)

```xml
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Dinero Updates</title>
    <link>https://dinero-coin.com/</link>
    <description>Dinero App Updates</description>
    <language>en</language>

    <item>
      <title>Version 0.1.1</title>
      <description><![CDATA[
        <h2>What's New in v0.1.1</h2>
        <ul>
          <li>Improved P2P connection stability</li>
          <li>Fixed wallet sync issues</li>
          <li>Performance improvements</li>
        </ul>
      ]]></description>
      <pubDate>Sat, 29 Oct 2025 12:00:00 +0000</pubDate>
      <enclosure
        url="https://updates.dinero-coin.com/Dinero-0.1.1-macOS.zip"
        sparkle:version="0.1.1"
        sparkle:shortVersionString="0.1.1"
        sparkle:edSignature="SIGNATURE_GOES_HERE"
        length="70000000"
        type="application/zip" />
    </item>

    <!-- Previous versions below -->
    <item>
      <title>Version 0.1.0</title>
      <description><![CDATA[
        <h2>Initial Release</h2>
        <ul>
          <li>Full node wallet</li>
          <li>Built-in miner</li>
          <li>Self-contained distribution</li>
        </ul>
      ]]></description>
      <pubDate>Mon, 21 Oct 2025 10:00:00 +0000</pubDate>
      <enclosure
        url="https://updates.dinero-coin.com/Dinero-0.1.0-macOS.zip"
        sparkle:version="0.1.0"
        sparkle:shortVersionString="0.1.0"
        length="67000000"
        type="application/zip" />
    </item>
  </channel>
</rss>
```

---

## Releasing a New Version

### 1. Build the new version

```bash
cd /Users/haydarevich/Documents/DineroCoin
# Update version in CMakeLists.txt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

### 2. Bundle dependencies

```bash
cd /Users/haydarevich/Desktop/Mac_Dinero_v0.1.1_NEW_VERSION
./bundle-dependencies.sh
```

### 3. Create ZIP archive

```bash
cd /Users/haydarevich/Desktop
zip -r -y Dinero-0.1.1-macOS.zip Mac_Dinero_v0.1.1_NEW_VERSION
```

### 4. Sign the update

```bash
cd /Users/haydarevich/Documents/DineroCoin/third_party
./bin/sign_update /Users/haydarevich/Desktop/Dinero-0.1.1-macOS.zip dsa_priv.pem
```

This outputs the EdDSA signature. Copy it.

### 5. Update appcast.xml

Add new `<item>` entry at the top with:
- New version number
- Release notes
- Download URL
- EdDSA signature from step 4
- File size (in bytes)

### 6. Deploy

Upload both files to your update server:
```bash
scp Dinero-0.1.1-macOS.zip root@updates.dinero-coin.com:/var/www/updates/
scp appcast.xml root@updates.dinero-coin.com:/var/www/updates/
```

### 7. Test

Open Dinero v0.1.0 and click "Check for Updates". It should detect v0.1.1 and offer to install it.

---

##Auto-Generate Appcast (Alternative)

Sparkle includes a tool to generate appcast.xml automatically:

```bash
cd /Users/haydarevich/Desktop
# Collect all release ZIPs in a directory
mkdir releases
cp Dinero-0.1.0-macOS.zip releases/
cp Dinero-0.1.1-macOS.zip releases/

# Generate appcast
cd /Users/haydarevich/Documents/DineroCoin/third_party
./bin/generate_appcast --ed-key-file dsa_priv.pem \
  /Users/haydarevich/Desktop/releases/
```

This creates `appcast.xml` with all versions, signatures, and file sizes.

---

## Security Notes

1. **Private Key Protection**
   - Store `dsa_priv.pem` offline or in secure vault
   - Never commit to Git
   - Only use on trusted build machines

2. **HTTPS Required**
   - Update feed MUST be HTTPS (`https://updates.dinero-coin.com`)
   - Prevents man-in-the-middle attacks

3. **Signature Verification**
   - Sparkle verifies EdDSA signature before installing
   - Unsigned updates will be rejected
   - Users are protected even if your server is compromised

---

## Benefits for Dinero

1. **Zero-Friction Updates**
   - Users click "Install" and the app updates itself
   - No manual DMG downloads or re-installation

2. **Rapid Deployment**
   - Push critical P2P fixes immediately
   - Wallet updates reach all users within 24 hours

3. **Security & Trust**
   - Cryptographically signed updates
   - No risk of fake/malicious updates

4. **Professional Polish**
   - Matches user expectations from mainstream Mac apps
   - Builds trust in the Dinero brand

5. **Network Health**
   - Keep all nodes on the latest consensus rules
   - Prevent network fragmentation

---

## Troubleshooting

### "Sparkle framework not found"
- Ensure `/third_party/Sparkle.framework` exists
- Run `cmake` again to detect framework

### "App crashes on launch"
- Check that `Info.plist` is correctly configured
- Verify public key format (no line breaks, base64)

### "Update check does nothing"
- Check `SUFeedURL` is correct and accessible
- Test appcast.xml URL in browser
- Check Xcode console for Sparkle logs

### "Update rejected / won't install"
- Signature mismatch - verify you used correct private key
- Check public key in Info.plist matches `dsa_pub.pem`
- Ensure ZIP file wasn't corrupted during upload

---

## Resources

- Sparkle Documentation: https://sparkle-project.org/documentation/
- Sparkle GitHub: https://github.com/sparkle-project/Sparkle
- EdDSA Signatures: https://sparkle-project.org/documentation/security/

---

## Next Steps

1. Implement the code changes above in `mainwindow.cpp/h` and `main.cpp`
2. Generate EdDSA keys with `generate_keys`
3. Configure `Info.plist` with public key
4. Test update flow with a mock v0.1.1 release
5. Setup production update server at `updates.dinero-coin.com`

