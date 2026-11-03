# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0
"""Single-host experimental KV pool using Mooncake TENT NDS for all payload IO.

Metadata is shared through SQLite. Keys are immutable and published only after
their complete NDS write. Storage is a bounded range in a regular file whose
data extents are initialized and flushed before use. This adapter has no eviction
or distributed metadata service.
"""

from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import stat
import threading
import time
import uuid

from vllm_ascend.distributed.kv_transfer.kv_pool.ascend_store.backend import backend_map
from vllm_ascend.distributed.kv_transfer.kv_pool.ascend_store.backend.base import (
    Backend,
)
from vllm_ascend.distributed.kv_transfer.kv_pool.ascend_store.ascend_store_connector import (
    AscendStoreConnector as AscendStoreConnector,
)

__all__ = ["AscendStoreConnector", "TentNdsBackend"]

backend_map["tent_nds"] = {"name": "TentNdsBackend", "path": __name__}


class TentNdsBackend(Backend):
    requires_exists_before_put = True

    def __init__(
        self, parallel_config, lazy_init=False, extra_config=None, metadata_only=False
    ):
        del lazy_init
        if extra_config and extra_config.get("use_layerwise"):
            raise ValueError("TENT NDS supports whole-key KV transfer")
        self.config = json.loads(
            Path(os.environ["MOONCAKE_TENT_NDS_CONFIG"]).read_text()
        )
        self.path = str(Path(self.config["storage_path"]).resolve())
        self.offset = int(self.config["storage_offset"])
        self.capacity = int(self.config["storage_capacity"])
        self.allocation_alignment = int(self.config.get("allocation_alignment", 512))
        if self.allocation_alignment <= 0 or self.allocation_alignment % 512:
            raise ValueError("Allocation alignment must be a positive multiple of 512")
        if self.offset < 0 or self.capacity <= 0 or (self.offset | self.capacity) % 512:
            raise ValueError("NDS storage range must be nonnegative and sector aligned")
        if self.offset + self.capacity >= 2**63:
            raise ValueError("NDS storage range exceeds signed file offsets")
        storage = os.stat(self.path)
        if not stat.S_ISREG(storage.st_mode):
            raise ValueError("This adapter requires a regular file")
        if self.offset + self.capacity > storage.st_size:
            raise ValueError("NDS storage range exceeds file size")
        if not self.config.get("namespace"):
            raise ValueError("An isolated model/runtime namespace is required")
        self.db_path = Path(self.config["metadata_path"])
        self.db_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.rank = parallel_config.rank
        self.device = None
        self.engine = None
        self.segment = None
        self.buffers = {}
        self.lock = threading.RLock()
        self.broken = False
        self.metadata_only = metadata_only
        self.events = (
            Path(self.config["log_dir"])
            / f"tent-nds-rank{self.rank}-pid{os.getpid()}.jsonl"
        )
        self.events.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        identity = json.dumps(
            {
                "path": self.path,
                "offset": self.offset,
                "capacity": self.capacity,
                "allocation_alignment": self.allocation_alignment,
                "namespace": self.config["namespace"],
            },
            sort_keys=True,
        )
        with self._metadata_initializer() as db:
            db.execute(
                "CREATE TABLE IF NOT EXISTS allocator (id INTEGER PRIMARY KEY, identity TEXT NOT NULL, next_offset INTEGER NOT NULL)"
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS objects (key TEXT PRIMARY KEY, offset INTEGER NOT NULL, length INTEGER NOT NULL, layout TEXT NOT NULL, owner TEXT NOT NULL, ready INTEGER NOT NULL)"
            )
            db.execute(
                "INSERT OR IGNORE INTO allocator VALUES (1, ?, ?)",
                (identity, self.offset),
            )
            if (
                db.execute("SELECT identity FROM allocator WHERE id=1").fetchone()[0]
                != identity
            ):
                raise ValueError(
                    "Metadata belongs to a different storage range or model namespace"
                )
        os.chmod(self.db_path, 0o600)
        self._emit("init", metadata_only=metadata_only)

    @contextmanager
    def _metadata_initializer(self):
        # journal_mode changes can fail immediately under concurrent startup,
        # irrespective of sqlite's busy timeout. Serialize schema/WAL setup
        # across scheduler and worker processes, then take the write lock before
        # inspecting schema or allocator identity.
        lock_fd = os.open(
            str(self.db_path) + ".init.lock",
            os.O_CREAT | os.O_RDWR | os.O_CLOEXEC,
            0o600,
        )
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
            with self._db() as db:
                if db.execute("PRAGMA journal_mode").fetchone()[0].lower() != "wal":
                    db.execute("PRAGMA journal_mode=WAL")
                db.execute("BEGIN IMMEDIATE")
                yield db
        finally:
            os.close(lock_fd)

    @contextmanager
    def _db(self):
        db = sqlite3.connect(self.db_path, timeout=60)
        try:
            with db:
                yield db
        finally:
            db.close()

    def _emit(self, operation, **fields):
        event = {
            "operation": operation,
            "rank": self.rank,
            "pid": os.getpid(),
            "time_ns": time.time_ns(),
            **fields,
        }
        fd = os.open(self.events, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            os.write(fd, (json.dumps(event) + "\n").encode())
        finally:
            os.close(fd)

    @classmethod
    def create_scheduler_client(cls, parallel_config):
        return cls(parallel_config, metadata_only=True)

    def set_device(self):
        if self.metadata_only:
            return
        import torch

        if self.device is None:
            self.device = torch.npu.current_device()
        torch.npu.set_device(self.device)

    def _initialize_engine(self):
        if self.metadata_only:
            raise RuntimeError("Scheduler metadata client cannot perform payload IO")
        if self.engine is not None:
            return
        import tent

        self.set_device()
        self.engine = tent.TransferEngine(self.config["tent_config_path"])
        if not self.engine.available():
            raise RuntimeError("TENT initialization failed")
        self.segment = self.engine.open_segment("file://" + self.path)
        info = self.engine.get_segment_info(self.segment)
        if (
            len(info.buffers) != 1
            or self.offset + self.capacity > info.buffers[0].length
        ):
            raise ValueError("Configured range exceeds storage device")

    def register_buffer(self, ptrs, lengths):
        import tent

        if len(ptrs) != len(lengths):
            raise ValueError("Registration vector lengths differ")
        with self.lock:
            self._initialize_engine()
            options = tent.MemoryOptions()
            options.type = tent.TransportType.NDS
            options.location = f"npu:{self.device}"
            for ptr, length in zip(ptrs, lengths):
                if ptr in self.buffers:
                    if self.buffers[ptr] != length:
                        raise ValueError("Buffer is registered with a different size")
                    continue
                self.engine.register_local_memory_ex(ptr, length, options)
                self.buffers[ptr] = length
            self._emit("register", buffers=len(ptrs), bytes=sum(lengths))

    def _lookup(self, keys):
        found = {}
        with self._db() as db:
            for start in range(0, len(keys), 256):
                chunk = keys[start : start + 256]
                placeholders = ",".join("?" for _ in chunk)
                for row in db.execute(
                    f"SELECT key, offset, length, layout FROM objects WHERE ready=1 AND key IN ({placeholders})",
                    chunk,
                ):
                    found[row[0]] = row[1:]
        return found

    def exists(self, keys):
        found = self._lookup(keys)
        return [int(key in found) for key in keys]

    def batch_is_readable(self, keys):
        return [bool(value) for value in self.exists(keys)]

    @staticmethod
    def _validate(keys, addrs, sizes):
        if not (len(keys) == len(addrs) == len(sizes)):
            raise ValueError("Key and buffer vector lengths differ")
        for pointers, lengths in zip(addrs, sizes):
            if not pointers or len(pointers) != len(lengths):
                raise ValueError("Invalid per-key buffer vectors")
            if any(
                ptr <= 0 or size <= 0 or (ptr | size) % 512
                for ptr, size in zip(pointers, lengths)
            ):
                raise ValueError("NDS fragments must be nonempty and sector aligned")

    def _transfer(self, entries, addrs, sizes, write):
        import tent

        if self.broken:
            raise RuntimeError("TENT NDS client is fenced after an IO failure")
        self._initialize_engine()
        requests = []
        for index, offset in entries:
            for address, length in zip(addrs[index], sizes[index]):
                requests.append(
                    tent.Request(
                        tent.OpCode.WRITE if write else tent.OpCode.READ,
                        address,
                        self.segment,
                        offset,
                        length,
                        transport_hint=tent.TransportType.NDS,
                    )
                )
                offset += length
        if not requests:
            return
        batch = self.engine.allocate_transfer_batch(len(requests))
        try:
            self.engine.submit_transfer(batch, requests)
            deadline = time.monotonic() + float(
                self.config.get("io_timeout_seconds", 120)
            )
            while True:
                status = self.engine.get_transfer_status_overall(batch)
                if status.state == tent.TransferStatusEnum.COMPLETED:
                    expected = sum(sum(sizes[index]) for index, _ in entries)
                    if status.bytes != expected:
                        raise RuntimeError(
                            f"Incomplete NDS transfer {status.bytes}/{expected}"
                        )
                    break
                if status.state != tent.TransferStatusEnum.PENDING:
                    raise RuntimeError(f"NDS transfer failed: {status.state}")
                if time.monotonic() >= deadline:
                    raise TimeoutError("NDS transfer deadline")
        except BaseException:
            self.broken = True
            # Fence before vLLM may release or reuse any registered KV buffer.
            for ptr, length in list(self.buffers.items()):
                self.engine.unregister_local_memory(ptr, length)
                del self.buffers[ptr]
            raise
        finally:
            self.engine.free_transfer_batch(batch)

    def put(self, keys, addrs, sizes):
        self._validate(keys, addrs, sizes)
        owner = uuid.uuid4().hex
        entries = []
        start = time.monotonic()
        with self.lock:
            with self._db() as db:
                db.execute("BEGIN IMMEDIATE")
                offset = db.execute(
                    "SELECT next_offset FROM allocator WHERE id=1"
                ).fetchone()[0]
                for index, key in enumerate(keys):
                    if db.execute(
                        "SELECT 1 FROM objects WHERE key=?", (key,)
                    ).fetchone():
                        continue
                    length = sum(sizes[index])
                    reserved = (
                        (length + self.allocation_alignment - 1)
                        // self.allocation_alignment
                    ) * self.allocation_alignment
                    if offset + reserved > self.offset + self.capacity:
                        raise RuntimeError("TENT NDS cache range is full")
                    db.execute(
                        "INSERT INTO objects VALUES (?, ?, ?, ?, ?, 0)",
                        (key, offset, length, json.dumps(sizes[index]), owner),
                    )
                    entries.append((index, offset))
                    offset += reserved
                db.execute("UPDATE allocator SET next_offset=? WHERE id=1", (offset,))
            try:
                self._transfer(entries, addrs, sizes, True)
                with self._db() as db:
                    db.execute("UPDATE objects SET ready=1 WHERE owner=?", (owner,))
            except BaseException:
                with self._db() as db:
                    db.execute(
                        "DELETE FROM objects WHERE owner=? AND ready=0", (owner,)
                    )
                self._emit("put", success=False, keys=len(entries))
                raise
        readable = self._lookup(keys)
        results = [
            0
            if key in readable
            and readable[key][1] == sum(lengths)
            and json.loads(readable[key][2]) == lengths
            else -11
            for key, lengths in zip(keys, sizes)
        ]
        self._emit(
            "put",
            success=all(value == 0 for value in results),
            keys=len(entries),
            bytes=sum(sum(sizes[index]) for index, _ in entries),
            fragments_per_key=len(sizes[0]) if sizes else 0,
            first_key_fragment_bytes=sizes[0] if sizes else [],
            elapsed_seconds=time.monotonic() - start,
        )
        return results

    def get(self, keys, addrs, sizes):
        self._validate(keys, addrs, sizes)
        start = time.monotonic()
        results = [-2] * len(keys)
        with self.lock:
            found = self._lookup(keys)
            entries = []
            for index, key in enumerate(keys):
                row = found.get(key)
                if (
                    row is not None
                    and self.offset <= row[0] <= self.offset + self.capacity
                    and row[0] % 512 == 0
                    and 0 < row[1] <= self.offset + self.capacity - row[0]
                    and row[1] == sum(sizes[index])
                    and json.loads(row[2]) == sizes[index]
                ):
                    entries.append((index, row[0]))
            try:
                self._transfer(entries, addrs, sizes, False)
                for index, _ in entries:
                    results[index] = 0
            except Exception as exc:
                self._emit("get_error", error=str(exc))
                for index, _ in entries:
                    results[index] = -5
        self._emit(
            "get",
            success=all(value == 0 for value in results),
            keys=len(entries),
            bytes=sum(sum(sizes[index]) for index, _ in entries),
            fragments_per_key=len(sizes[0]) if sizes else 0,
            first_key_fragment_bytes=sizes[0] if sizes else [],
            elapsed_seconds=time.monotonic() - start,
        )
        return results
