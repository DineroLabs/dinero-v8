# Shielded candidate hardware profile

This is the initial qualification target, not a claim that unmeasured devices
are supported. Current measurements are of a proof test process on Apple M4 Max
(macOS arm64, 128 GiB installed RAM); they do not measure all daemon memory.

| Role | Initial qualification target | Release acceptance |
|---|---|---|
| Validator / archival / reindex node | 64-bit CPU, at least 4 modern physical cores, 16 GiB RAM; SSD with measured chain/undo retention capacity plus rebuild headroom | All consensus/lifecycle tests; proof process ≤2 GiB RSS; maximum eight-proof block verification ≤30 s; total daemon peak under 75% of installed RAM under steady relay and replay/reindex, without OOM. Disk requirement is measured from actual retained history, not guessed. |
| Desktop wallet with local proving | 64-bit CPU, at least 8 modern physical cores, 16 GiB RAM (32 GiB when sharing the host with an archival node) | Same proof RSS bound; supported transfer build ≤120 s, individual verification ≤30 s; one wallet proof operation at a time; successful UI/daemon responsiveness and recovery under load. |
| Mobile | No local proving support claim in the initial profile | Separate device measurements and lifecycle qualification required. Verification-only clients have a separate resource/authority model and do not inherit desktop approval. |
| Physical hardware wallet | Shielded signing not advertised in initial scope | Capability rejection stays enabled. Real firmware/device proof and authority tests required before support is advertised. |

The core counts are starting points for qualification, not substitutes for
per-core throughput. The measured latency/RSS gates decide whether a particular
host qualifies. The process limits are engineering targets, not consensus rules;
changing them must not alter block validity or silently reject slow proofs.
The wallet runtime currently serializes operations through its runtime mutex;
that does not limit simultaneous validation on other threads or processes.
Do not assume a global one-proof concurrency cap exists.

## Reproduce

Build `test_shielded_validation` from a clean, recorded revision with the release
toolchain, then run (substitute that build revision for `VERIFIED_BUILD_SHA`):

```
python3 tools/shielded_resource_qualification.py \
  --binary build/test_shielded_validation \
  --binary-source-commit VERIFIED_BUILD_SHA \
  --output /absolute/path/to/evidence --repetitions 3
```

Each shape gets a fresh process. The report contains all samples, p50/p95/max,
binary identity, explicitly supplied binary source revision, separate harness
revision, host architecture and explicit budget results. An omitted binary
source revision is reported as unknown; the checkout is not proof of its origin. With three
samples, p95 is the largest sample, not a statistically precise tail estimate.
Repeat with at least 20 samples and representative concurrent node load for
fleet sign-off. A zero/unsupported RSS measurement does not pass. Windows
release-build smoke is separate from Windows proof-memory qualification.

## Current local proof measurements

Three repetitions per shape, including two distinct 2-in/2-out transactions
which together consume the eight-proof block limit. All sample processes perform
real proof construction, range/binding/proof verification and resource checks.
The block case also checks cross-transaction nullifier uniqueness.

Measured normal recipient-plus-change transfers remain 167,935 bytes. Across
the local run, maximum eight-proof block verification was about 11.7 s, and peak
process RSS about 804 MB (767 MiB). This leaves proof-computation margin against
the proposed 20-second block budget on this host. It does not prove total-node
memory, p95 under production load, or the suggested minimum RAM/core machines.
See the accompanying qualification JSON for exact values and sample provenance.

Linux CI now runs the same capacity gate. Windows and both macOS architectures
also run release artifact builds; build success alone is not a proof-capacity
result. Keep the qualification ledger explicit about this distinction.

## Linux CI proof qualification

The four-logical-CPU Ubuntu x86-64 runner also passed all eighteen fresh-process
measurements ([run 34316707990](https://github.com/DineroLabs/dinero-v8/actions/runs/34316707990)).
Normal 1-in/2-out maximum build/verify times were 25.365/6.585 seconds; the
eight-proof block maximum verification was 19.014 seconds. Peak proof-process
RSS was 858,759,168 bytes (819 MiB). The block result has only about 5% margin
against the 20-second target: it qualifies that measured proof run, not loaded
production operation on every four-core host. Retain the concurrent-load and
20-sample fleet requirements. Exact merge-ref/binary identity and all samples
are in `SHIELDED_RESOURCE_QUALIFICATION_LINUX.json`.


## Budget revision after the final-candidate measurement

The initial 20-second proof budget was provisional. On runtime candidate
`e79295a8b`, Linux run [34325068921](https://github.com/DineroLabs/dinero-v8/actions/runs/34325068921)
validated every proof but measured maximum-block verification at 20.433, 20.495
and 20.451 seconds, consistently failing that budget. Peak RSS was 858,959,872
bytes. The [original failed report](../audits/SHIELDED_RESOURCE_QUALIFICATION_E792_20S_FAILED.json)
is retained; it is not reclassified as a flaky test or a passing 20-second run.

The designated review adopts the **initial-desktop-v2** engineering profile:
maximum proof-component block verification 30 seconds, one quarter of the
120-second target interval in chain parameters. This leaves 90 seconds of the
average interval for other processing and propagation, and about 46% timing
headroom over the slowest observed proof sample. It is a throughput allocation,
not a guarantee that other work finishes within that reserve or that a block
never arrives sooner. The whole-node 75%-RAM, loaded recovery/relay, and
20-sample fleet tests remain mandatory before claiming production-host support.

Transaction proof time, RSS limits, consensus proof/byte caps and activation
heights are unchanged. The qualifier records the CPU model on new runs and
reports the failing shape and limits explicitly. Earlier 20-second measurements
remain historical evidence under their original budget. This revision does not
certify every four-core machine; measured loaded behavior selects actual hosts.

## Final runtime candidate on this Mac

A fresh 18-process run of runtime `e79295a8b` under the v2 qualifier passes
on Apple M4 Max. Maximum normal transfer construction/verification was
11.834/4.077 seconds; maximum eight-proof block verification was 11.868 seconds;
peak process RSS was 804,225,024 bytes (767 MiB). The
[complete report](../audits/SHIELDED_RESOURCE_QUALIFICATION_E792_MACOS.json)
records the test-binary hash and separates its source revision from the harness.
Other development tests were running, but this is not a controlled representative
production-load or minimum-machine qualification.
