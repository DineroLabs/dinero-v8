# Mining and SV2 Pool Mining

Dinero supports solo mining and SV2 pool mining paths.

## Solo Mining

Solo mining asks your local daemon for block templates and submits blocks directly to your node.

This is useful for testing and for operators who intentionally want direct block submission.

## SV2 Pool Mining

SV2 pool mining connects a miner to a pool service, receives jobs over the SV2 protocol, and submits shares or blocks to the pool.

The desktop wallet can act as a controller that launches the bundled SV2 miner binary and displays session status.

Useful SV2 debugging signals:

- pool host and port
- pinned pool public key
- selected backend: `cpu`, `auto`, `metal`, `opencl`, or `cuda`
- `link connected`
- channel open
- job received
- share accepted
- hashrate

## Stratum V1

Stratum V1 is legacy/maintenance-only. Prefer SV2 for current pool-mining work unless a maintainer specifically asks you to test V1.
