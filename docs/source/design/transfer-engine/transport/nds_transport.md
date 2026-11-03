# NDS Transport

[中文文档](../../../zh_archive/nds_transport.md)

## Background

Applications that offload KV caches to local storage need to transfer data
between SSDs and Ascend HBM. NPU Direct Storage (NDS) provides a direct DMA path
for this workload, including KV blocks distributed across multiple layer buffers.

## Overview

NDS Transport is located at
`mooncake-transfer-engine/tent/src/transport/nds`. It provides local regular-file
reads and writes through TENT using the third-party
`ascend_nds.h` / `libascend_nds.so` interface. The library calls XDS; Mooncake
uses the existing public API without changing the third-party header or ABI.

- Each request describes a contiguous buffer; a batch can contain multiple HBM fragments.
- Uses TENT memory registration, File Segments, batch submission and status queries.
- Requires both the `USE_NDS` build option and the `transports/nds/enable` runtime setting.
- Provides a standalone benchmark and an experimental single-host vLLM KV adapter.

NDS handles local storage transfers. Ascend Direct handles memory transfers.

## Prerequisites

### Hardware and software

| Component | Requirement |
| --- | --- |
| Host | Linux with an Ascend NPU and a working CANN/driver installation |
| Storage | A regular file on a local filesystem supported by XDS; initialized extents on NVMe/ext4 are verified on hardware |
| NDS SDK | UBSIO `feat/nds-adapter-v2`, providing `ascend_nds.h` and `libascend_nds.so` |
| XDS | `mycastiel/xds`, branch `loop_topo`, with matching userspace library and kernel module |
| Mooncake dependencies | Follow the [build guide](../../../getting_started/build.md), including the dependencies for the selected Ascend platform |
| Python tools | A compatible `torch`, `torch_npu` and `numpy` environment for the benchmark; vLLM-Ascend for the KV adapter |

Keep the NDS SDK, XDS userspace ABI and kernel driver compatible. CPU/mock tests
run without NPU hardware and use a separate test SDK.

### Installing the NDS SDK

Build the facade in the UBSIO checkout and point Mooncake at its SDK directory:

```bash
bash /path/to/ubs-io/ubsio-nds/build.sh -t release
export NDS_ROOT=/path/to/ubs-io/ubsio-nds/dist
test -r "$NDS_ROOT/include/ascend_nds.h"
test -r "$NDS_ROOT/lib/libascend_nds.so"
```

Deploy the matching XDS `libnds.so` and kernel module using the XDS deployment
instructions. The NDS SDK and XDS are external dependencies of Mooncake.

## Building

Run from the Mooncake repository root, in the Python environment used for testing:

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export NDS_INSTALL_PREFIX="$HOME/.local/mooncake-nds"

cmake -S . -B build-nds -G Ninja \
  -DUSE_TENT=ON -DUSE_NDS=ON -DNDS_ROOT="$NDS_ROOT" \
  -DUSE_ASCEND_DIRECT=ON -DUSE_CUDA=OFF \
  -DWITH_STORE=OFF -DWITH_STORE_RUST=OFF \
  -DBUILD_UNIT_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF \
  -DTENT_METRICS_ENABLED=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$NDS_INSTALL_PREFIX"
cmake --build build-nds --parallel 2 --target tent_shared tentpy tent_cli
cmake --install build-nds/mooncake-common/src
cmake --install build-nds/mooncake-transfer-engine/tent
```

`USE_ASCEND_DIRECT=ON` enables the Ascend platform used by this example and
requires its CANN/ADXL/HIXL dependencies. `USE_NDS=ON` alone does not enable the
Ascend memory probe. The `ascend_direct` transport can remain disabled at runtime.
When selecting Python explicitly, set both `PYTHON_EXECUTABLE` and
`Python3_EXECUTABLE` to the interpreter used by the benchmark.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `USE_TENT` | `OFF` | Build the TENT engine; required by NDS |
| `USE_NDS` | `OFF` | Build the NDS transport |
| `NDS_ROOT` | Unset | SDK prefix containing `include/` and `lib/` or `lib64/` |
| `NDS_INCLUDE_DIR` / `NDS_LIBRARY` | Auto-detected | Explicit public-header directory and facade library path |
| `TENT_METRICS_ENABLED` | `OFF` | Compile TENT metrics |

The native installation contains `libtent_shared.so` and the `tent` diagnostic
command. C/C++ consumers use headers from `mooncake-transfer-engine/tent/include`.
The benchmark uses the existing binding built under
`build-nds/mooncake-transfer-engine/tent/src/python`; no NDS wheel is needed.

## Environment Variables

| Variable | Required | Description |
| --- | --- | --- |
| `LD_LIBRARY_PATH` | For libraries outside the loader search path | TENT, NDS SDK, XDS and CANN library directories |
| `PYTHONPATH` | For Python tools | Built `tent` module; also the adapter directory when using vLLM |
| `MC_TENT_CONF` | For application configuration | TENT JSON file path or inline JSON |
| `MC_USE_TENT` | For the classic TE compatibility entry | Set to `1`; direct `tent.TransferEngine()` calls do not need it |
| `MOONCAKE_TENT_NDS_CONFIG` | For the vLLM adapter | Path to its storage-range and metadata configuration |

```bash
export LD_LIBRARY_PATH="$NDS_INSTALL_PREFIX/lib:$NDS_ROOT/lib:/path/to/xds/file_p2p:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="$PWD/build-nds/mooncake-transfer-engine/tent/src/python:${PYTHONPATH:-}"
export MC_TENT_CONF=/absolute/path/nds-tent.json
```

### TENT configuration file

Save the following as `nds-tent.json`. It selects NDS for local NPU file I/O:

```json
{
  "metadata_type": "p2p",
  "rpc_server_hostname": "127.0.0.1",
  "rpc_server_port": 0,
  "merge_requests": false,
  "transports": {
    "tcp": {"enable": false},
    "rdma": {"enable": false},
    "shm": {"enable": false},
    "io_uring": {"enable": false},
    "ascend_direct": {"enable": false},
    "nds": {
      "enable": true,
      "io_batch_depth": 128,
      "io_bytes": 16777216,
      "register_memory": false
    }
  },
  "policy": [
    {"name": "nds_storage", "segment_type": "file",
     "transports": ["nds"]}
  ]
}
```

| NDS setting | Default | Description |
| --- | --- | --- |
| `enable` | `false` | Enable NDS in this engine |
| `io_batch_depth` | `128` | Submitted but unreaped I/O entries per batch; 1–1024 |
| `io_bytes` | `16777216` | Maximum bytes per entry; 512-byte aligned, from 512 B to 64 MiB |
| `register_memory` | `false` | `false`: one-shot mappings; `true`: pin registered HBM through the facade |

Recreate the engine after changing these settings. The standalone benchmark
constructs its own TENT configuration from command-line arguments; use
`--io-depth` and `--io-bytes` to configure it.

## Usage

### Application calls

1. Select the NPU before creating `tent.TransferEngine()`.
2. Register HBM with `MemoryOptions.type = NDS` and `location = "npu:<device>"`.
3. Open an initialized, flushed regular file with `file:///absolute/path`.
4. Allocate a transfer batch and submit read or write requests. `source` is HBM
   for both operations; `target_offset` is the storage byte offset.
5. Poll until completion, check the completed byte count, then free the batch.
   Unregister the buffers before releasing their allocations and close the segment.

For a fragmented key, create one ordinary `tent.Request(opcode, address,
segment, file_offset, length)` per registered HBM fragment. Advance the file
offset by each fragment length so the key occupies a contiguous storage range.
Submit these requests together in one batch. Set `policy_name="nds_storage"`
to use the policy permitting only NDS. `transport_hint=NDS` expresses a preference
within the allowed transports. The benchmark provides an executable example.

### vLLM adapter

`mooncake-integration/vllm/tent_nds_connector.py` provides the `tent_nds` backend
for vLLM-Ascend's `AscendStoreConnector`. Add its directory to `PYTHONPATH`:

```bash
export PYTHONPATH="$PWD/mooncake-integration/vllm:$PYTHONPATH"
export MOONCAKE_TENT_NDS_CONFIG=/absolute/path/nds-kv.json
```

Prepare a dedicated file by writing every data block and flushing it. For example,
the following creates a new 64 GiB file; `oflag=excl` protects existing files:

```bash
dd if=/dev/zero of=/absolute/path/nds-kv.bin bs=4M count=16384 oflag=excl conv=fsync
```

Use a fresh metadata path for each validation run. Example `nds-kv.json`:

```json
{
  "storage_path": "/absolute/path/nds-kv.bin",
  "storage_offset": 0,
  "storage_capacity": 68719476736,
  "allocation_alignment": 4194304,
  "namespace": "minimax-tp8-functional-run",
  "metadata_path": "/absolute/path/run/metadata.sqlite",
  "tent_config_path": "/absolute/path/nds-tent.json",
  "log_dir": "/absolute/path/run/events",
  "io_timeout_seconds": 120
}
```

Size the range for the combined KV allocation of all ranks. The example reserves
64 GiB, so the file must cover that capacity plus the starting offset; a short
backend smoke test can use a smaller dedicated range.

The vLLM connector configuration is:

```json
{
  "kv_connector": "AscendStoreConnector",
  "kv_connector_module_path": "tent_nds_connector",
  "kv_role": "kv_both",
  "kv_connector_extra_config": {"backend": "tent_nds", "use_layerwise": false}
}
```

This adapter supports single-host whole-key transfers and publishes keys only
after complete writes. It does not provide eviction, crash recovery or distributed
metadata. It is not a Mooncake Store SSD tier.

## Verification

### Installation check

```bash
"$NDS_INSTALL_PREFIX/bin/tent" diagnostics version --json
python3 -c 'import tent, torch, torch_npu; print(tent.__file__); print(torch.npu.is_available())'
```

Confirm that Python loads the binding from the intended build directory and
that `tent.TransportType.NDS` is present. `engine.available()` describes the
engine as a whole; verify NDS initialization and data transfer with the I/O test.

### CPU tests

The `chore/tent-nds-validation` branch contains tests, benchmark tools, CI and
documentation as separate commits on top of `feat/tent-nds`. Run the CPU checks
from that branch without NPU hardware:

```bash
python3 scripts/ci/run_tent_nds_cpu.py --mode on --build-root /var/tmp/tent-nds-on
python3 scripts/ci/run_tent_nds_cpu.py --mode off --build-root /var/tmp/tent-nds-off
```

The script builds and tests TENT, checks installation and missing-SDK behavior,
and runs metadata concurrency checks. Reports are `result.json`, `ctest.xml` and
`missing-sdk.log` under the build root. The mock SDK stays in the test directory;
these checks do not validate DMA or hardware bandwidth.

### Basic file I/O

Choose a new test file on a supported local filesystem. The benchmark creates
the file, initializes the requested data ranges and flushes them before NDS I/O.
Existing files must cover the requested range. The test overwrites that range,
including when `--mode read` is selected.

```bash
python3 mooncake-transfer-engine/benchmark/nds/benchmark_nds.py \
  --file /absolute/path/nds-smoke.bin \
  --device 0 --bytes 4096 --fragments 1 --batch 2 \
  --mode roundtrip --warmup 0 --iterations 1 --json nds-smoke.json
```

Success requires exit code `0`, `success: true` and `verified_bytes: 8192`.
The tool verifies CPU-written data through NDS, NDS-written data through POSIX
`O_DIRECT`, and the NDS round trip before and after measurement. A failure returns `1` and
records an `error` in the JSON result.

### MiniMax-shaped benchmark

The single-card MiniMax TP8 KV layout uses 124 fragments of 32 KiB per key with
a 4 MiB storage stride. Run the same shape with 350 or 950 keys:

```bash
python3 mooncake-transfer-engine/benchmark/nds/benchmark_nds.py \
  --file /absolute/path/nds-read-350.bin \
  --device 0 --bytes 32768 --fragments 124 --batch 350 \
  --key-stride 4194304 --io-depth 512 --mode read \
  --warmup 2 --iterations 30 --json nds-read-350.json
```

Use `--file /absolute/path/nds-read-950.bin --batch 950 --json nds-read-950.json`
for the larger case. Reserve sufficient
HBM and storage: the range must cover
`offset + batch * key_stride`.

| Option or result | Meaning |
| --- | --- |
| `--bytes` / `--fragments` | Bytes per fragment and fragments per key |
| `--batch` | Keys submitted in each batch, not calling threads |
| `--io-depth` | Maximum outstanding facade entries; not the number of keys |
| `--backend facade` | Reference measurement through the same UBSIO public API |
| `measurements.read.gb_per_second` | Decimal read bandwidth in GB/s |
| `mean_ms`, `p50_ms`, `p99_ms`, `samples` | Batch latency statistics and individual samples within each measurement |

The TENT timing window covers submission through completion, including request
validation and I/O planning. Tensor allocation, request construction, preparation
writes, warmup, data verification and batch release are outside the measured read
window. Compare facade and TENT results separately. The single-card pure-read
acceptance target is at least 6.5 GB/s after data verification passes.

NDS uses this standalone entry. The current tebench protocol path does not fully
integrate Ascend or MemFabric either.

### vLLM backend smoke test

With vLLM-Ascend installed and `nds-kv.json` configured, test the adapter without
loading a model:

```bash
python3 mooncake-integration/vllm/smoke_tent_nds.py --device 0
```

Expected output includes `success: true`, `fragments: 124`, `missing_key: -2`
and `failed_write_published: false`. Then validate MiniMax TP8 generation and
external KV reuse with repeated prefixes, local prefix caching disabled, and
successful per-rank write/read events in `log_dir`. Backend smoke alone does not
establish model-level functionality. End-to-end acceptance is functional;
performance uses the standalone benchmark.

## Important Notes

1. **Alignment and bounds:** Addresses, sizes and file offsets must be 512-byte
   aligned. Every fragment must fit registered HBM, and the request must fit the
   local File Segment and actual storage capacity.
2. **File preparation and ownership:** Write and `fsync` the complete data range
   before NDS use. `fallocate` or `truncate` alone is insufficient: unwritten extents
   can pass an NDS round trip while POSIX reads still return zeros. Keep file mappings
   stable; avoid truncation, hole punching, concurrent buffered I/O and overlapping
   writes. XDS does not maintain page-cache coherence; cross-check with `O_DIRECT`.
3. **Buffer lifetime:** Keep allocations alive until completion or successful
   unregister. Error cleanup and unregister can block while DMA drains. Retry a
   failed unregister before releasing its buffer.
4. **Failure handling:** NDS drains submitted DMA before reporting failure; partial
   writes are not rolled back. Reads and writes follow the existing file-transport
   failover policy. Use a policy whose transport list contains only NDS to keep
   transfers on this transport.
5. **Observability:** Enable `metrics/enabled` in a metrics-enabled build to inspect
   `transport="nds"` request, byte, failure and attempt counters. Use TENT's
   `log_level` for diagnostics.
6. **Automation:** CPU CI uses the same script above. The manual
   `.github/workflows/nds-hardware.yml` entry requires a prepared `nds-file` runner,
   `NDS_ROOT`, `NDS_PYTHON`, `NDS_TEST_DIRECTORY`, an idle `NDS_DEVICE`,
   `NDS_HARDWARE_ENABLED=true` and explicit file-write confirmation. Keep it disabled
   without suitable hardware.

## Troubleshooting

| Symptom | Check or action |
| --- | --- |
| CMake reports a missing NDS SDK | Verify `NDS_ROOT`, or set `NDS_INCLUDE_DIR` and `NDS_LIBRARY` explicitly |
| `import tent` fails or loads another build | Check the Python version, `PYTHONPATH`, `tent.__file__` and dependent libraries |
| `ndsFileDriverOpen` fails | Check real facade/XDS library resolution, driver version and device permissions |
| NDS is unavailable or registration/selection fails | Check both enable switches, the Ascend platform build, selected NPU, memory location, alignment and policy |
| Transfer fails or completes fewer bytes | Inspect the JSON `error`, storage capacity, submitted range and driver I/O logs; retain buffers until drained |
| Metadata identity mismatch | Use a fresh metadata path and a namespace matching the file range, model and KV layout |
| Small-I/O bandwidth is low | Confirm data checks pass; compare the same fragment layout, batch size, depth, timing scope and device load |

## See Also

- [Mooncake build guide](../../../getting_started/build.md)
- [TENT overview](../../tent/overview.md)
