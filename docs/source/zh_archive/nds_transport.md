---
orphan: true
---

# NDS Transport

[English documentation](../design/transfer-engine/transport/nds_transport.md)

## 背景

将 KV 缓存卸载到本机存储的应用，需要在 SSD 与 Ascend HBM 之间搬运数据。
NPU Direct Storage（NDS）为这类读写提供直接 DMA 通路，并支持一个 KV block
分散在多个层缓冲区中的场景。

## 概述

NDS Transport 的代码位于 `mooncake-transfer-engine/tent/src/transport/nds`，
通过第三方标准接口 `ascend_nds.h` / `libascend_nds.so`，为 TENT 提供本机文件
读写。该库调用 XDS；Mooncake 使用现有公开接口，第三方头文件和 ABI 保持原样。

- 每个请求描述一个连续缓冲区，一个 batch 可提交多个 HBM 分片。
- 使用 TENT 的内存注册、File Segment、批量提交和状态查询接口。
- 需要同时启用 `USE_NDS` 编译选项和 `transports/nds/enable` 运行时配置。
- 提供独立 benchmark 和实验性单机 vLLM KV 适配器。

NDS 负责本机存储读写，Ascend Direct 负责内存传输。

## 前置依赖

### 硬件与软件

| 组件 | 要求 |
| --- | --- |
| 主机 | Linux、Ascend NPU，以及可用的 CANN 和驱动 |
| 存储 | XDS 支持的本机文件系统上的普通文件；已在 NVMe/ext4 上验证初始化 extent 的直通读写 |
| NDS SDK | UBSIO `feat/nds-adapter-v2` 分支，提供 `ascend_nds.h` 和 `libascend_nds.so` |
| XDS | `mycastiel/xds` 的 `loop_topo` 分支，用户态库与内核模块配套 |
| Mooncake 构建依赖 | 按[编译指南](../getting_started/build.md)准备，并安装所选 Ascend 平台的构建依赖 |
| Python 工具环境 | benchmark 需要配套的 `torch`、`torch_npu`、`numpy`；KV 适配器另需 vLLM-Ascend |

NDS SDK、XDS 用户态 ABI 和驱动须保持兼容。CPU/mock 测试使用独立测试 SDK，
不需要 NPU 硬件。

### 安装 NDS SDK

在 UBSIO 仓库构建适配库，将公开 SDK 目录传给 Mooncake：

```bash
bash /path/to/ubs-io/ubsio-nds/build.sh -t release
export NDS_ROOT=/path/to/ubs-io/ubsio-nds/dist
test -r "$NDS_ROOT/include/ascend_nds.h"
test -r "$NDS_ROOT/lib/libascend_nds.so"
```

按 XDS 的部署说明安装配套的 `libnds.so` 和内核模块。NDS SDK 与 XDS 作为
Mooncake 的外部依赖分别部署。

## 编译

在用于跑测的 Python 环境中，从 Mooncake 仓库根目录执行：

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

示例通过 `USE_ASCEND_DIRECT=ON` 启用 Ascend 平台，需要对应的 CANN/ADXL/HIXL
构建依赖。仅设置 `USE_NDS=ON` 不会启用 Ascend 内存探测。
`ascend_direct` Transport 可以在运行时保持关闭。显式选择 Python 时，将
`PYTHON_EXECUTABLE` 和 `Python3_EXECUTABLE` 都设置为 benchmark 使用的解释器。

| CMake 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USE_TENT` | `OFF` | 构建 TENT，NDS 必需 |
| `USE_NDS` | `OFF` | 构建 NDS Transport |
| `NDS_ROOT` | 未设置 | 包含 `include/` 和 `lib/` 或 `lib64/` 的 SDK 前缀 |
| `NDS_INCLUDE_DIR` / `NDS_LIBRARY` | 自动查找 | 分别显式指定公开头文件目录和适配库路径 |
| `TENT_METRICS_ENABLED` | `OFF` | 编译 TENT 指标支持 |

原生安装包含 `libtent_shared.so` 和 `tent` 诊断命令。C/C++ 使用
`mooncake-transfer-engine/tent/include` 中的头文件；benchmark 使用
`build-nds/mooncake-transfer-engine/tent/src/python` 下构建的现有绑定，无需 NDS wheel。

## 环境变量配置

| 变量 | 使用条件 | 说明 |
| --- | --- | --- |
| `LD_LIBRARY_PATH` | 库不在动态加载器默认搜索路径时 | TENT、NDS SDK、XDS、CANN 的库目录 |
| `PYTHONPATH` | 使用 Python 工具时 | 构建生成的 `tent` 模块目录；vLLM 另需适配器目录 |
| `MC_TENT_CONF` | 配置 TENT 应用时 | TENT JSON 文件路径或内联 JSON |
| `MC_USE_TENT` | 使用经典 TE 兼容入口时 | 设置为 `1`；直接调用 `tent.TransferEngine()` 不需要 |
| `MOONCAKE_TENT_NDS_CONFIG` | 使用 vLLM 适配器时 | 存储范围及元数据配置文件路径 |

```bash
export LD_LIBRARY_PATH="$NDS_INSTALL_PREFIX/lib:$NDS_ROOT/lib:/path/to/xds/file_p2p:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="$PWD/build-nds/mooncake-transfer-engine/tent/src/python:${PYTHONPATH:-}"
export MC_TENT_CONF=/absolute/path/nds-tent.json
```

### TENT 配置文件

将以下内容保存为 `nds-tent.json`，使用 NDS 执行本机 NPU 文件 IO：

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

| NDS 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `enable` | `false` | 在当前 engine 中启用 NDS |
| `io_batch_depth` | `128` | 每个 batch 已提交但尚未回收的 IO 项数，范围 1–1024 |
| `io_bytes` | `16777216` | 单个 IO 项的最大字节数，按 512 B 对齐，范围 512 B–64 MiB |
| `register_memory` | `false` | `false` 使用一次性映射；`true` 通过适配库预注册 HBM |

修改上述配置后重新创建 engine。独立 benchmark 按命令行参数生成自己的 TENT
配置，使用 `--io-depth` 和 `--io-bytes` 设置其 IO 参数。

## 使用方法

### 应用调用

1. 在创建 `tent.TransferEngine()` 前选择 NPU。
2. 注册 HBM，指定 `MemoryOptions.type = NDS` 和 `location = "npu:<device>"`。
3. 用 `file:///absolute/path` 打开已实际写入并落盘的普通文件。
4. 分配 transfer batch 并提交读写请求。读写的 `source` 均为 HBM 地址，
   `target_offset` 为存储字节偏移。
5. 查询到完成后检查字节数，再释放 batch；释放内存分配前先注销缓冲区，并关闭 segment。

分片 key 的每个 HBM 分片对应一个普通
`tent.Request(opcode, address, segment, file_offset, length)`。按分片长度累加
文件偏移，使整个 key 占据连续存储区间，再将这些请求作为一个 batch 提交。
使用 `policy_name="nds_storage"` 选择仅允许 NDS 的策略；
`transport_hint=NDS` 表达允许范围内的传输优先选择。独立 benchmark 包含可执行的完整调用示例。

### vLLM 适配器

`mooncake-integration/vllm/tent_nds_connector.py` 为 vLLM-Ascend 的
`AscendStoreConnector` 提供 `tent_nds` 后端。将适配器目录加入 `PYTHONPATH`：

```bash
export PYTHONPATH="$PWD/mooncake-integration/vllm:$PYTHONPATH"
export MOONCAKE_TENT_NDS_CONFIG=/absolute/path/nds-kv.json
```

准备独立文件，实际写入整个数据区域并落盘。例如，以下命令创建 64 GiB 文件，
`oflag=excl` 确保已有文件不会被覆盖：

```bash
dd if=/dev/zero of=/absolute/path/nds-kv.bin bs=4M count=16384 oflag=excl conv=fsync
```

每轮验证使用新的元数据路径。以下是 `nds-kv.json` 示例：

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

容量按所有 rank 的 KV 分配总量估算。示例预留 64 GiB，文件需覆盖该容量与起始
偏移；简短的后端 smoke 可以使用更小的独立范围。

vLLM 的 connector 配置为：

```json
{
  "kv_connector": "AscendStoreConnector",
  "kv_connector_module_path": "tent_nds_connector",
  "kv_role": "kv_both",
  "kv_connector_extra_config": {"backend": "tent_nds", "use_layerwise": false}
}
```

该适配器支持单机 whole-key 传输，在完整写入后发布 key；不提供淘汰、崩溃恢复
或分布式元数据，也不作为 Mooncake Store 的 SSD 分层。

## 功能与性能验证

### 安装检查

```bash
"$NDS_INSTALL_PREFIX/bin/tent" diagnostics version --json
python3 -c 'import tent, torch, torch_npu; print(tent.__file__); print(torch.npu.is_available())'
```

确认 Python 加载了预期构建目录的模块，并包含 `tent.TransportType.NDS`。
`engine.available()` 表示整个 engine 的状态；NDS 初始化及数据传输通过后续 IO
测试确认。

### CPU 测试

`chore/tent-nds-validation` 在特性分支 `feat/tent-nds` 上以独立提交保存测试、
benchmark、CI 和文档。无 NPU 环境时，在验证分支的仓库根目录执行：

```bash
python3 scripts/ci/run_tent_nds_cpu.py --mode on --build-root /var/tmp/tent-nds-on
python3 scripts/ci/run_tent_nds_cpu.py --mode off --build-root /var/tmp/tent-nds-off
```

脚本构建并测试 TENT，检查安装、SDK 缺失行为及元数据并发。构建根目录下的
`result.json`、`ctest.xml`、`missing-sdk.log` 保存结果。mock SDK 仅存在于测试
目录，CPU 检查不代表真实 DMA 功能或硬件带宽。

### 普通文件基础读写

在受支持的本机文件系统中选择新的测试文件。benchmark 创建文件，实际写入请求
覆盖的数据区域并落盘，然后执行 NDS IO。已有文件需覆盖请求范围。测试会覆盖
该范围，`--mode read` 也包含准备写入。

```bash
python3 mooncake-transfer-engine/benchmark/nds/benchmark_nds.py \
  --file /absolute/path/nds-smoke.bin \
  --device 0 --bytes 4096 --fragments 1 --batch 2 \
  --mode roundtrip --warmup 0 --iterations 1 --json nds-smoke.json
```

成功条件为返回码 `0`、`success: true` 和 `verified_bytes: 8192`。工具校验 CPU
写入后由 NDS 读取、NDS 写入后由 POSIX `O_DIRECT` 读取，以及测量前后的 NDS
读写回环；失败时返回 `1`，JSON 中记录 `error`。

### MiniMax 形状 benchmark

MiniMax TP8 的单卡 KV 布局按每 key 124 个 32 KiB 分片、4 MiB 存储 stride
构造。使用相同形状，分别下发 350 或 950 keys：

```bash
python3 mooncake-transfer-engine/benchmark/nds/benchmark_nds.py \
  --file /absolute/path/nds-read-350.bin \
  --device 0 --bytes 32768 --fragments 124 --batch 350 \
  --key-stride 4194304 --io-depth 512 --mode read \
  --warmup 2 --iterations 30 --json nds-read-350.json
```

较大压力使用 `--file /absolute/path/nds-read-950.bin --batch 950 --json nds-read-950.json`。
预留足够的 HBM 和存储，文件容量须覆盖 `offset + batch * key_stride`。

| 参数或结果 | 含义 |
| --- | --- |
| `--bytes` / `--fragments` | 每个分片的字节数、每个 key 的分片数 |
| `--batch` | 每批提交的 key 数，不是调用线程数 |
| `--io-depth` | 未完成的标准 IO 项上限，不是 key 数 |
| `--backend facade` | 通过同一 UBSIO 公开接口进行对照测量 |
| `measurements.read.gb_per_second` | 十进制读带宽，单位 GB/s |
| `mean_ms`、`p50_ms`、`p99_ms`、`samples` | 各 measurement 中的批次延迟统计与原始样本 |

TENT 计时范围从提交到完成，包含请求校验和 IO 规划；张量分配、请求对象构造、
准备写入、预热、数据校验和 batch 释放不计入纯读测量。facade 与 TENT 结果分别
比较。数据校验通过后，单卡纯读带宽验收目标为 6.5 GB/s 以上。

NDS 使用该独立入口；当前 tebench 的协议路径也未完整接入 Ascend 和 MemFabric。

### vLLM 后端 smoke

安装 vLLM-Ascend 并配置 `nds-kv.json` 后，可以不加载模型先验证适配器：

```bash
python3 mooncake-integration/vllm/smoke_tent_nds.py --device 0
```

预期输出包含 `success: true`、`fragments: 124`、`missing_key: -2` 和
`failed_write_published: false`。随后验证 MiniMax TP8 生成和重复前缀对应的外部
KV 复用，关闭本地 prefix caching，并检查 `log_dir` 中各 rank 的成功读写事件。
后端 smoke 不代表模型端到端功能已通过。端到端仅验功能，性能通过独立 benchmark 测量。

## 注意事项

1. **对齐与边界：** 地址、长度和文件偏移按 512 B 对齐。每个分片均需位于已注册
   HBM 内，请求范围需同时满足本机 File Segment 与实际存储容量。
2. **文件准备与独占：** 使用前实际写入整个数据区域并执行 `fsync`。仅使用
   `fallocate` 或 `truncate` 不足以初始化文件：unwritten extent 的 NDS 回环可能
   成功，但 POSIX 读取仍返回零。保持文件映射稳定，避免截断、打洞、并发 buffered IO
   和重叠写。XDS 不维护页缓存一致性，交叉校验使用 `O_DIRECT`。
3. **缓冲区生命周期：** 完成或成功注销之前保持内存有效。错误清理和注销可能等待
   DMA 排空；注销失败时先重试，成功后再释放内存。
4. **失败处理：** NDS 排空已提交的 DMA 后报告失败，已发生的部分写入不回滚。
   读写请求均遵循现有文件 Transport 回退策略；若需要固定 NDS 路径，使用仅包含
   NDS 的策略。
5. **可观测性：** 使用启用了指标的构建，设置 `metrics/enabled` 后观察
   `transport="nds"` 的请求、字节、失败和物理尝试计数。日志级别使用 TENT 的 `log_level`。
6. **自动化：** CPU CI 复用上述脚本。手动 `.github/workflows/nds-hardware.yml`
   需要准备好的 `nds-file` runner、`NDS_ROOT`、`NDS_PYTHON`、`NDS_TEST_DIRECTORY`、
   空闲的 `NDS_DEVICE`、`NDS_HARDWARE_ENABLED=true` 和明确的文件写入确认。
   无真实环境时保持关闭。

## 故障排查

| 现象 | 检查与处理 |
| --- | --- |
| CMake 找不到 NDS SDK | 检查 `NDS_ROOT`，或显式设置 `NDS_INCLUDE_DIR` 和 `NDS_LIBRARY` |
| `import tent` 失败或加载了其他版本 | 检查 Python 版本、`PYTHONPATH`、`tent.__file__` 和动态库依赖 |
| `ndsFileDriverOpen` 失败 | 检查真实适配库与 XDS 的加载路径、驱动版本及设备权限 |
| NDS 不可用，或注册/选路失败 | 检查两个启用开关、Ascend 平台构建、当前 NPU、内存位置、对齐和策略 |
| 请求失败或完成字节数不足 | 查看 JSON 的 `error`、存储容量、提交范围和驱动日志；排空前保留缓冲区 |
| 元数据身份不匹配 | 使用新的元数据路径及匹配文件范围、模型和 KV 布局的 namespace |
| 小粒度带宽偏低 | 先确认数据校验通过，再对照相同的分片、batch、深度、计时范围和设备负载 |

## 相关文档

- [Mooncake 编译指南](../getting_started/build.md)
- [TENT 概述](../design/tent/overview.md)
