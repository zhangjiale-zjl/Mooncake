# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0
"""UBSIO ascend_nds.h baseline for the same fragmented NPU workload."""

import ctypes as c
import os
import time


class Fs(c.Structure):
    _fields_ = [
        ("fds", c.POINTER(c.c_int)),
        ("count", c.c_uint),
        ("reserved", c.c_uint),
    ]


class FileDesc(c.Structure):
    _fields_ = [("type", c.c_int), ("fd", c.c_int)]


class Params(c.Structure):
    _fields_ = [
        ("base", c.c_void_p),
        ("file_offset", c.c_long),
        ("buffer_offset", c.c_long),
        ("size", c.c_size_t),
        ("handle", c.c_void_p),
        ("opcode", c.c_int),
        ("cookie", c.c_void_p),
    ]


class Event(c.Structure):
    _fields_ = [("cookie", c.c_void_p), ("status", c.c_int), ("result", c.c_int)]


class Timespec(c.Structure):
    _fields_ = [("seconds", c.c_long), ("nanoseconds", c.c_long)]


class FacadeNds:
    """Use only ndsFile*; the UBSIO library owns loading the XDS runtime."""

    def __init__(self, args):
        self.args = args
        self.lib = c.CDLL("libascend_nds.so")
        self.ctx = c.c_void_p()
        self.handle = c.c_void_p()
        self.fd = c.c_int(-1)
        self.fs = Fs(c.pointer(self.fd), 1, 0)
        self.topology = False
        self.initialized = False
        signatures = {
            "ndsFileDriverOpen": [],
            "ndsFileDriverClose": [],
            "ndsFileRegisterFs": [c.POINTER(Fs)],
            "ndsFileDeregisterFs": [c.POINTER(Fs)],
            "ndsFileHandleRegister": [c.POINTER(c.c_void_p), c.POINTER(FileDesc)],
            "ndsFileBatchIOSetUp": [c.POINTER(c.c_void_p), c.c_uint],
            "ndsFileBatchIODestroy": [c.c_void_p],
            "ndsFileBatchIOSubmit": [c.c_void_p, c.c_uint, c.POINTER(Params), c.c_uint],
            "ndsFileBatchIOGetStatus": [
                c.c_void_p,
                c.c_uint,
                c.POINTER(c.c_uint),
                c.POINTER(Event),
                c.POINTER(Timespec),
            ],
        }
        for name, types in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes = types
            function.restype = c.c_int
        self.lib.ndsFileHandleDeregister.argtypes = [c.c_void_p]
        self.lib.ndsFileHandleDeregister.restype = None
        self.check(self.lib.ndsFileDriverOpen(), "DriverOpen")
        self.initialized = True

    @staticmethod
    def check(rc, operation):
        if rc:
            raise RuntimeError(f"NDS facade {operation}: {rc}")

    def available(self):
        return self.initialized

    def register_local_memory_ex(self, *_):
        pass  # Use the facade's one-shot mapping path.

    def unregister_local_memory(self, *_):
        pass  # transfer() drains before returning, including on failure.

    def open_segment(self, name):
        self.fd.value = os.open(
            name.removeprefix("file://"), os.O_RDWR | os.O_DIRECT | os.O_CLOEXEC
        )
        self.check(self.lib.ndsFileRegisterFs(c.byref(self.fs)), "RegisterFs")
        self.topology = True
        desc = FileDesc(1, self.fd.value)
        self.check(
            self.lib.ndsFileHandleRegister(c.byref(self.handle), c.byref(desc)),
            "HandleRegister",
        )
        self.check(
            self.lib.ndsFileBatchIOSetUp(c.byref(self.ctx), self.args.io_depth),
            "BatchIOSetUp",
        )
        return self.fd.value

    def close_segment(self, _):
        self.close()

    def close(self):
        if self.ctx.value:
            rc = self.lib.ndsFileBatchIODestroy(self.ctx)
            self.ctx = c.c_void_p()
            if rc:
                print(f"NDS facade destroy drained with error {rc}", flush=True)
        if self.handle.value:
            self.lib.ndsFileHandleDeregister(self.handle)
            self.handle = c.c_void_p()
        if self.topology:
            self.check(self.lib.ndsFileDeregisterFs(c.byref(self.fs)), "DeregisterFs")
            self.topology = False
        if self.fd.value >= 0:
            os.close(self.fd.value)
            self.fd.value = -1
        if self.initialized:
            self.check(self.lib.ndsFileDriverClose(), "DriverClose")
            self.initialized = False

    def transfer(self, buffers, opcode):
        args = self.args
        entries = []
        for key in range(args.batch):
            for layer, buffer in enumerate(buffers):
                for consumed in range(0, args.bytes, args.io_bytes):
                    length = min(args.bytes - consumed, args.io_bytes)
                    entries.append(
                        Params(
                            buffer.data_ptr(),
                            args.offset
                            + key * args.key_stride
                            + layer * args.bytes
                            + consumed,
                            key * args.bytes + consumed,
                            length,
                            self.handle,
                            opcode,
                            len(entries) + 1,
                        )
                    )
        params = (Params * len(entries))(*entries)
        events = (Event * args.io_depth)()
        completed = [False] * len(params)
        submitted = inflight = reaped = 0
        submit_seconds = 0
        start = time.perf_counter()
        try:
            while reaped < len(params):
                count = min(len(params) - submitted, args.io_depth - inflight)
                if count:
                    pointer = c.cast(
                        c.byref(params, submitted * c.sizeof(Params)), c.POINTER(Params)
                    )
                    before = time.perf_counter()
                    rc = self.lib.ndsFileBatchIOSubmit(self.ctx, count, pointer, 0)
                    submit_seconds += time.perf_counter() - before
                    self.check(rc, "BatchIOSubmit")
                    submitted += count
                    inflight += count
                timeout = Timespec(1, 0)
                count = c.c_uint(inflight)
                self.check(
                    self.lib.ndsFileBatchIOGetStatus(
                        self.ctx, 1, c.byref(count), events, c.byref(timeout)
                    ),
                    "BatchIOGetStatus",
                )
                if count.value > inflight:
                    raise RuntimeError("Facade completion count exceeds inflight")
                for event in events[: count.value]:
                    cookie = event.cookie or 0
                    if (
                        cookie < 1
                        or cookie > submitted
                        or completed[cookie - 1]
                        or event.result
                        or event.status != 0x10
                    ):
                        raise RuntimeError(
                            f"Invalid completion {cookie}: {event.result}"
                        )
                    completed[cookie - 1] = True
                inflight -= count.value
                reaped += count.value
                if time.perf_counter() - start > args.timeout:
                    raise TimeoutError("NDS facade transfer deadline")
            seconds = time.perf_counter() - start
            return {
                "seconds": seconds,
                "submit_seconds": submit_seconds,
                "poll_seconds": seconds - submit_seconds,
                "io_entries": len(params),
            }
        except BaseException:
            if self.ctx.value:
                self.lib.ndsFileBatchIODestroy(self.ctx)
                self.ctx = c.c_void_p()
            raise
