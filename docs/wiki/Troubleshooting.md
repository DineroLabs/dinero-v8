# Troubleshooting

## Wallet or Node Shows an Old Height

Capture:

- release version
- current height
- best hash
- peer count
- startup command line
- recent logs around peer connection and block download

## Mobile Local Validation Does Not Advance

Capture:

- local validated height
- best header height
- bridge peer count
- `getdata` lines
- `utxoblk` receive lines
- `ConnectTip` or validation lines
- snapshot/bootstrap restore lines

## SV2 Mining Connects But Does Not Mine

Capture:

- pool endpoint
- pinned server public key
- selected backend
- miner binary path
- `link connected` / channel open / job received lines
- share accepted/rejected lines

## Disk Space Problems

Operator nodes and archival bridge nodes need disk headroom. If a node is close to full, do not wait for it to fail silently. Move the role to a larger host or prune only if that node is not expected to serve historical data.
