# Dinero v8.2.7

## Desktop wallet

- When setup recognizes data from an older snapshot, it offers a separate folder for a new wallet. The original folder keeps its wallets and keys. If you already received DIN, keep your original wallet and use the recovery guidance. Starting a separate wallet does not restore older balances.
- New-wallet setup shows Orchard history verification progress and waits and retries when the node confirms that it has not created a wallet yet. You can close the wizard while it waits.
- Choose your language in Settings or in the older-snapshot setup dialog. The list includes Slovenian. Recovery words and protocol identifiers retain their original form.
- The Orchard page shows the minimum fee when a payment’s proposed fee is too low to sign and submit. You can cancel an unsigned queued payment. Switching wallets clears the previous wallet’s Orchard amount.
- Pool mining displays submitted block candidates separately from blocks the pool confirms.

## Downloads

| Platform | Desktop | Node and operator tools |
|---|---|---|
| macOS Apple Silicon | [DMG](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-macOS-arm64.dmg) · [ZIP](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-macOS-arm64-qt.zip) | [operator archive](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-operator-v8.2.7-macOS-arm64.tar.gz) |
| macOS Intel | [DMG](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-macOS-x86_64.dmg) · [ZIP](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-macOS-x86_64-qt.zip) | [operator archive](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-operator-v8.2.7-macOS-x86_64.tar.gz) |
| macOS Intel Ventura (Qt 6.5) | [DMG](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-ventura-qt6.5-macOS-x86_64.dmg) · [ZIP](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-v8.2.7-ventura-qt6.5-macOS-x86_64-qt.zip) | [operator archive](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-operator-v8.2.7-ventura-macOS-x86_64.tar.gz) |
| Windows x64 | [wallet installer](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-8.2.7-windows-x86_64-Setup.exe) | [server installer](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/Dinero-Server-8.2.7-windows-x86_64-Setup.exe) · [portable ZIP](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-v8.2.7-windows-x86_64-msvc.zip) |
| Linux x64 | [AppImage](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-v8.2.7-linux-x86_64.AppImage) · [desktop .deb](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-qt-desktop_8.2.7-1_amd64.deb) · [portable Qt archive](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-v8.2.7-linux-x86_64-qt.tar.gz) | [server archive](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-linux-x86_64-8.2.7.tar.gz) · [server .deb](https://github.com/DineroLabs/dinero-v8/releases/download/v8.2.7/dinero-core_8.2.7-1_amd64.deb) |

## Snapshot

The bundled primary snapshot starts at block 169,068. The release also includes the earlier recovery snapshots for installations that used those bases.
