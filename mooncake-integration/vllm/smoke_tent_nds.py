# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0
"""Validate the NDS KV backend using MOONCAKE_TENT_NDS_CONFIG and real HBM."""

import argparse
import json
from types import SimpleNamespace
import uuid

import torch
import torch_npu  # noqa: F401

from tent_nds_connector import AscendStoreConnector, TentNdsBackend
from vllm.distributed.kv_transfer.kv_connector.factory import KVConnectorFactory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=int, default=0)
    args = parser.parse_args()
    connector = KVConnectorFactory.get_connector_class(
        SimpleNamespace(
            kv_connector="AscendStoreConnector",
            kv_connector_module_path="tent_nds_connector",
        )
    )
    assert connector is AscendStoreConnector
    torch.npu.set_device(args.device)
    parallel = SimpleNamespace(rank=0)
    backend = TentNdsBackend(parallel)
    scheduler = TentNdsBackend.create_scheduler_client(parallel)
    expected = [
        ((torch.arange(32768) + layer) % 251).to(torch.uint8) for layer in range(124)
    ]
    buffers = [data.to(f"npu:{args.device}") for data in expected]
    torch.npu.synchronize()
    pointers = [buffer.data_ptr() for buffer in buffers]
    sizes = [32768] * len(buffers)
    key = "smoke-" + uuid.uuid4().hex
    try:
        backend.register_buffer(pointers, sizes)
        assert scheduler.exists([key]) == [0]
        assert backend.put([key], [pointers], [sizes]) == [0]
        assert scheduler.exists([key]) == [1]
        for buffer in buffers:
            buffer.zero_()
        torch.npu.synchronize()
        assert backend.get([key], [pointers], [sizes]) == [0]
        assert all(
            torch.equal(buffer.cpu(), data) for buffer, data in zip(buffers, expected)
        )
        assert backend.get([key + "-missing"], [pointers], [sizes]) == [-2]
        invalid = pointers.copy()
        invalid[-1] += sizes[-1]
        failed_key = key + "-invalid"
        try:
            backend.put([failed_key], [invalid], [sizes])
        except RuntimeError:
            pass
        else:
            raise AssertionError("Unregistered fragment was accepted")
        assert scheduler.exists([failed_key]) == [0]
        assert scheduler.device is None
        print(
            json.dumps(
                {
                    "success": True,
                    "verified_bytes": sum(sizes),
                    "fragments": len(sizes),
                    "fragment_bytes": 32768,
                    "missing_key": -2,
                    "failed_write_published": False,
                }
            ),
            flush=True,
        )
    finally:
        if backend.engine is not None:
            for ptr, length in list(backend.buffers.items()):
                backend.engine.unregister_local_memory(ptr, length)
            backend.buffers.clear()
            if backend.segment is not None:
                backend.engine.close_segment(backend.segment)
            backend.engine = None


if __name__ == "__main__":
    main()
