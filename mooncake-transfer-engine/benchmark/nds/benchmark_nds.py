#!/usr/bin/env python3
# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0
"""Verify and benchmark TENT NDS against an explicitly selected storage range."""

import argparse
import fcntl
import json
import mmap
import os
from pathlib import Path
import stat
import time

import numpy as np
import tent
import torch
import torch_npu  # noqa: F401


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", required=True)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument(
        "--backend",
        choices=("tent", "facade"),
        default="tent",
        help="TENT application path or UBSIO ndsFile API baseline",
    )
    parser.add_argument("--offset", type=int, default=0, help="Byte offset")
    parser.add_argument("--bytes", type=int, default=32 * 1024 * 1024)
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument(
        "--fragments",
        type=int,
        default=1,
        help="Separate layer buffers per key; MiniMax uses 124",
    )
    parser.add_argument(
        "--key-stride",
        type=int,
        default=0,
        help="Disk bytes between keys; 0 packs keys contiguously",
    )
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--io-depth", type=int, default=128)
    parser.add_argument("--io-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument(
        "--mode", choices=("read", "write", "roundtrip"), default="roundtrip"
    )
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if min(args.bytes, args.batch, args.fragments, args.iterations, args.timeout) <= 0:
        parser.error("bytes, batch, iterations and timeout must be positive")
    if args.offset < 0 or args.warmup < 0 or args.bytes % 512 or args.offset % 512:
        parser.error("offset and bytes must be 512-byte aligned and nonnegative")
    if not 1 <= args.io_depth <= 1024:
        parser.error("io-depth must be between 1 and 1024")
    if not 512 <= args.io_bytes <= 64 * 1024 * 1024 or args.io_bytes % 512:
        parser.error("io-bytes must be aligned and between 512 and 64 MiB")
    if args.key_stride == 0:
        args.key_stride = args.bytes * args.fragments
    if args.key_stride < args.bytes * args.fragments or args.key_stride % 512:
        parser.error("key-stride must cover one key and be 512-byte aligned")
    return args


def configure(args):
    config = {
        "metadata_type": "p2p",
        "rpc_server_hostname": "127.0.0.1",
        "rpc_server_port": 0,
        "log_level": "info",
        "merge_requests": False,
        "transports": {
            name: {"enable": False}
            for name in (
                "tcp",
                "rdma",
                "shm",
                "io_uring",
                "gds",
                "nvlink",
                "mnnvl",
                "ascend_direct",
            )
        },
    }
    config["transports"]["nds"] = {
        "enable": True,
        "io_batch_depth": args.io_depth,
        "io_bytes": args.io_bytes,
        "register_memory": False,
    }
    os.environ["MC_TENT_CONF"] = json.dumps(config)
    return config


def transfer(engine, segment, buffers, args, opcode):
    if args.backend == "facade":
        return engine.transfer(buffers, 0 if opcode == tent.OpCode.READ else 1)
    bytes_per_task = args.bytes
    requests = [
        tent.Request(
            opcode,
            buffer.data_ptr() + key * args.bytes,
            segment,
            args.offset + key * args.key_stride + layer * args.bytes,
            args.bytes,
            transport_hint=tent.TransportType.NDS,
        )
        for key in range(args.batch)
        for layer, buffer in enumerate(buffers)
    ]
    batch = engine.allocate_transfer_batch(len(requests))
    start = time.perf_counter()
    try:
        engine.submit_transfer(batch, requests)
        submitted = time.perf_counter()
        deadline = start + args.timeout
        while True:
            status = engine.get_transfer_status_overall(batch)
            if status.state == tent.TransferStatusEnum.COMPLETED:
                break
            if status.state != tent.TransferStatusEnum.PENDING:
                raise RuntimeError(
                    f"NDS transfer failed: {status.state}, {status.bytes}"
                )
            if time.perf_counter() >= deadline:
                raise TimeoutError("NDS transfer exceeded deadline")
        elapsed = time.perf_counter() - start
        statuses = engine.get_transfer_status_list(batch)
        if [s.bytes for s in statuses] != [bytes_per_task] * len(requests):
            raise RuntimeError("Incorrect per-task completed byte count")
        return {
            "seconds": elapsed,
            "submit_seconds": submitted - start,
            "poll_seconds": elapsed - (submitted - start),
        }
    finally:
        engine.free_transfer_batch(batch)


def run(args, result):
    path = Path(args.file).resolve()
    if path.exists():
        if not stat.S_ISREG(path.stat().st_mode):
            raise ValueError("Target must be a regular file")
        if path.stat().st_size < args.offset + args.key_stride * args.batch:
            raise ValueError("File is too small for the requested storage range")
    else:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            os.posix_fallocate(fd, 0, args.offset + args.key_stride * args.batch)
        finally:
            os.close(fd)

    with open(path, "rb") as storage_lock:
        fcntl.flock(storage_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        run_buffers(args, result, path)


def run_buffers(args, result, path):
    layer_bytes = args.bytes * args.batch
    total = layer_bytes * args.fragments
    torch.npu.set_device(args.device)
    rng = np.random.default_rng(42)
    expected = [
        torch.from_numpy(rng.integers(0, 256, layer_bytes, dtype=np.uint8))
        for _ in range(args.fragments)
    ]
    buffers = [data.to(f"npu:{args.device}") for data in expected]
    torch.npu.synchronize()
    result["configuration"] = configure(args)
    result["workload"] = {
        "keys": args.batch,
        "fragments_per_key": args.fragments,
        "fragment_bytes": args.bytes,
        "key_bytes": args.bytes * args.fragments,
        "key_stride_bytes": args.key_stride,
        "calling_threads": 1,
        "timing_scope": "TENT submit and overall completion; excludes request construction, allocation, validation and batch release",
    }
    result["tent_module"] = tent.__file__
    result["torch_version"] = torch.__version__
    if args.backend == "facade":
        from facade_nds import FacadeNds

        engine = FacadeNds(args)
        result["workload"]["timing_scope"] = (
            "UBSIO facade submit and rolling completion; excludes IO plan construction, allocation and validation"
        )
    else:
        engine = tent.TransferEngine()
    if not engine.available():
        raise RuntimeError("TENT is unavailable")
    registered = []
    segment = None
    try:
        options = tent.MemoryOptions()
        options.type = tent.TransportType.NDS
        options.location = f"npu:{args.device}"
        for buffer in buffers:
            engine.register_local_memory_ex(buffer.data_ptr(), layer_bytes, options)
            registered.append(buffer)
        # Write and flush real file data: fallocate alone leaves unwritten
        # extents whose contents are not visible through filesystem reads.
        with open(path, "r+b", buffering=0) as storage:
            for key in range(args.batch):
                for layer, data in enumerate(expected):
                    offset = args.offset + key * args.key_stride + layer * args.bytes
                    seed = np.bitwise_xor(
                        data[key * args.bytes : (key + 1) * args.bytes].numpy(), 255
                    ).tobytes()
                    if os.pwrite(storage.fileno(), seed, offset) != args.bytes:
                        raise RuntimeError("Short file initialization write")
            os.fsync(storage.fileno())
            os.posix_fadvise(
                storage.fileno(),
                args.offset,
                args.key_stride * args.batch,
                os.POSIX_FADV_DONTNEED,
            )
        segment = engine.open_segment("file://" + str(path))
        transfer(engine, segment, buffers, args, tent.OpCode.READ)
        if not all(
            np.array_equal(buffer.cpu().numpy(), np.bitwise_xor(data.numpy(), 255))
            for buffer, data in zip(buffers, expected)
        ):
            raise RuntimeError("CPU file write to NDS read verification failed")
        result["cpu_to_nds_verified_bytes"] = total
        for buffer, data in zip(buffers, expected):
            buffer.copy_(data)
        torch.npu.synchronize()
        transfer(engine, segment, buffers, args, tent.OpCode.WRITE)
        direct_fd = os.open(path, os.O_RDONLY | os.O_DIRECT | os.O_CLOEXEC)
        try:
            with mmap.mmap(-1, args.bytes) as direct_buffer:
                for key in range(args.batch):
                    for layer, data in enumerate(expected):
                        offset = (
                            args.offset + key * args.key_stride + layer * args.bytes
                        )
                        count = os.preadv(direct_fd, [direct_buffer], offset)
                        wanted = data[key * args.bytes : (key + 1) * args.bytes]
                        if (
                            count != args.bytes
                            or direct_buffer[:] != wanted.numpy().tobytes()
                        ):
                            raise RuntimeError(
                                "NDS write to POSIX direct read verification failed"
                            )
        finally:
            os.close(direct_fd)
        result["nds_to_posix_verified_bytes"] = total
        for buffer in buffers:
            buffer.zero_()
        torch.npu.synchronize()
        transfer(engine, segment, buffers, args, tent.OpCode.READ)
        if not all(
            torch.equal(buffer.cpu(), data) for buffer, data in zip(buffers, expected)
        ):
            raise RuntimeError("NDS round-trip data verification failed")
        result["verified_bytes"] = total
        modes = ["read", "write"] if args.mode == "roundtrip" else [args.mode]
        result["measurements"] = {}
        for mode in modes:
            opcode = tent.OpCode.READ if mode == "read" else tent.OpCode.WRITE
            for _ in range(args.warmup):
                transfer(engine, segment, buffers, args, opcode)
            samples = [
                transfer(engine, segment, buffers, args, opcode)
                for _ in range(args.iterations)
            ]
            seconds = [sample["seconds"] for sample in samples]
            result["measurements"][mode] = {
                "iterations": len(samples),
                "bytes_per_iteration": total,
                "total_bytes": total * len(samples),
                "elapsed_seconds": sum(seconds),
                "gb_per_second": total * len(samples) / sum(seconds) / 1e9,
                "mean_ms": float(np.mean(seconds) * 1000),
                "p50_ms": float(np.percentile(seconds, 50) * 1000),
                "p99_ms": float(np.percentile(seconds, 99) * 1000),
                "samples": samples,
            }
        transfer(engine, segment, buffers, args, tent.OpCode.READ)
        if not all(
            torch.equal(buffer.cpu(), data) for buffer, data in zip(buffers, expected)
        ):
            raise RuntimeError("Post-benchmark data verification failed")
        result["success"] = True
    finally:
        # Unregistration fences any unfinished DMA before releasing the tensor.
        for buffer in registered:
            engine.unregister_local_memory(buffer.data_ptr(), layer_bytes)
        if segment is not None:
            engine.close_segment(segment)
        elif args.backend == "facade":
            engine.close()


def main():
    os.umask(0o077)
    args = arguments()
    result = {
        "success": False,
        "arguments": {
            k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()
        },
    }
    try:
        run(args, result)
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
