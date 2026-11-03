# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0
"""Validate simultaneous scheduler/worker metadata startup on a fresh database."""

import argparse
import json
import multiprocessing as mp
import os
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import uuid


def mock_vendor_imports():
    """CPU-only metadata tests need interface names, never a serving runtime."""
    prefix = "vllm_ascend.distributed.kv_transfer.kv_pool.ascend_store"
    for leaf in ("backend", "backend.base", "ascend_store_connector"):
        parts = (prefix + "." + leaf).split(".")
        for index in range(1, len(parts) + 1):
            name = ".".join(parts[:index])
            module = sys.modules.setdefault(name, ModuleType(name))
            module.__path__ = []
    sys.modules[prefix + ".backend"].backend_map = {}
    sys.modules[prefix + ".backend.base"].Backend = object
    sys.modules[prefix + ".ascend_store_connector"].AscendStoreConnector = object


def worker(config_path, rank, barrier, results, cpu_mode=False):
    try:
        os.environ["MOONCAKE_TENT_NDS_CONFIG"] = config_path
        if cpu_mode:
            mock_vendor_imports()
        from tent_nds_connector import TentNdsBackend

        barrier.wait(timeout=120)
        for _ in range(3):
            backend = TentNdsBackend.create_scheduler_client(SimpleNamespace(rank=rank))
            assert backend.exists(["missing"]) == [0]
            assert backend.engine is None and backend.device is None
        results.put({"rank": rank, "success": True})
    except Exception as error:
        results.put({"rank": rank, "success": False, "error": repr(error)})
        raise


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cpu",
        action="store_true",
        help="Isolated metadata-only test without vLLM or NPU",
    )
    args = parser.parse_args()
    temporary = (
        tempfile.TemporaryDirectory(prefix="tent-nds-metadata-") if args.cpu else None
    )
    if temporary:
        directory = Path(temporary.name)
        storage = directory / "storage"
        storage.write_bytes(bytes(4096))
        source = directory / "config.json"
        source.write_text(
            json.dumps(
                {
                    "storage_path": str(storage),
                    "storage_offset": 0,
                    "storage_capacity": 4096,
                    "log_dir": str(directory / "events"),
                }
            )
        )
        mock_vendor_imports()
        from tent_nds_connector import TentNdsBackend

        valid = json.loads(source.read_text())
        invalid_path = directory / "invalid.json"
        for invalid in (
            {**valid, "storage_capacity": 8192},
            {**valid, "storage_path": str(directory)},
        ):
            invalid_path.write_text(json.dumps(invalid))
            os.environ["MOONCAKE_TENT_NDS_CONFIG"] = str(invalid_path)
            try:
                TentNdsBackend.create_scheduler_client(SimpleNamespace(rank=0))
            except ValueError:
                pass
            else:
                raise AssertionError("Invalid regular-file range was accepted")
    else:
        source = Path(os.environ["MOONCAKE_TENT_NDS_CONFIG"])
    config = json.loads(source.read_text())
    identifier = "metadata-concurrency-" + uuid.uuid4().hex
    config["namespace"] = identifier
    config["metadata_path"] = str(source.parent / (identifier + ".sqlite"))
    config["log_dir"] = str(Path(config["log_dir"]) / identifier)
    path = source.parent / (identifier + ".json")
    path.write_text(json.dumps(config))
    ctx = mp.get_context("spawn")
    barrier = ctx.Barrier(8)
    results = ctx.Queue()
    processes = [
        ctx.Process(target=worker, args=(str(path), rank, barrier, results, args.cpu))
        for rank in range(8)
    ]
    for process in processes:
        process.start()
    observed = [results.get(timeout=180) for _ in processes]
    for process in processes:
        process.join(timeout=30)
        assert process.exitcode == 0, process.exitcode
    assert len(observed) == 8 and all(row["success"] for row in observed), observed
    if temporary:
        assert storage.read_bytes() == bytes(4096)
        temporary.cleanup()
    print(
        json.dumps(
            {
                "success": True,
                "cpu_metadata_only": args.cpu,
                "processes": 8,
                "opens_per_process": 3,
                "ranks": sorted(row["rank"] for row in observed),
            }
        ),
        flush=True,
    )


if __name__ == "__main__":
    main()
