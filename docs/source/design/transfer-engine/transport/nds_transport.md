# NDS Transport

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
`Python3_EXECUTABLE` to the interpreter used by the application.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `USE_TENT` | `OFF` | Build the TENT engine; required by NDS |
| `USE_NDS` | `OFF` | Build the NDS transport |
| `NDS_ROOT` | Unset | SDK prefix containing `include/` and `lib/` or `lib64/` |
| `NDS_INCLUDE_DIR` / `NDS_LIBRARY` | Auto-detected | Explicit public-header directory and facade library path |
| `TENT_METRICS_ENABLED` | `OFF` | Compile TENT metrics |

The native installation contains `libtent_shared.so` and the `tent` diagnostic
command. C/C++ consumers use headers from `mooncake-transfer-engine/tent/include`.
Python applications use the existing binding built under
`build-nds/mooncake-transfer-engine/tent/src/python`; no NDS wheel is needed.

## Environment Variables

| Variable | Required | Description |
| --- | --- | --- |
| `LD_LIBRARY_PATH` | For libraries outside the loader search path | TENT, NDS SDK, XDS and CANN library directories |
| `PYTHONPATH` | For Python tools | Built `tent` module for Python applications |
| `MC_TENT_CONF` | For application configuration | TENT JSON file path or inline JSON |
| `MC_USE_TENT` | For the classic TE compatibility entry | Set to `1`; direct `tent.TransferEngine()` calls do not need it |

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

Recreate the engine after changing these settings.

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
within the allowed transports.

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

Run the CPU checks from the repository root without NPU hardware:

```bash
python3 scripts/ci/run_tent_nds_cpu.py --mode on --build-root /var/tmp/tent-nds-on
python3 scripts/ci/run_tent_nds_cpu.py --mode off --build-root /var/tmp/tent-nds-off
```

The script builds and tests TENT, checks installed consumers and missing-SDK
behavior. Reports are `result.json`, `ctest.xml` and
`missing-sdk.log` under the build root. The mock SDK stays in the test directory;
these checks do not validate DMA or hardware bandwidth.

### Hardware validation

With the production NDS SDK and an idle NPU, use the application calls above
with a dedicated test file. Verify CPU-written data through NDS, NDS-written
data through POSIX `O_DIRECT`, and an NDS write/read round trip. Check completed
byte counts and compare the data before releasing buffers. Cover both one-shot
and registered-memory modes, multiple fragments and batches larger than the
configured I/O depth. Record the Mooncake commit, NDS SDK, XDS and CANN versions
with the results.

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
6. **Automation:** CPU CI uses the same script above. Hardware validation must
   run separately with the production NDS SDK and an idle NPU; CPU/mock results
   do not establish hardware correctness or performance.

## Troubleshooting

| Symptom | Check or action |
| --- | --- |
| CMake reports a missing NDS SDK | Verify `NDS_ROOT`, or set `NDS_INCLUDE_DIR` and `NDS_LIBRARY` explicitly |
| `import tent` fails or loads another build | Check the Python version, `PYTHONPATH`, `tent.__file__` and dependent libraries |
| `ndsFileDriverOpen` fails | Check real facade/XDS library resolution, driver version and device permissions |
| NDS is unavailable or registration/selection fails | Check both enable switches, the Ascend platform build, selected NPU, memory location, alignment and policy |
| Transfer fails or completes fewer bytes | Inspect the transfer status, storage capacity, submitted range and driver I/O logs; retain buffers until drained |
| Small-I/O bandwidth is low | Confirm data checks pass; compare the same fragment layout, batch size, depth, timing scope and device load |

## See Also

- [Mooncake build guide](../../../getting_started/build.md)
- [TENT overview](../../tent/overview.md)
