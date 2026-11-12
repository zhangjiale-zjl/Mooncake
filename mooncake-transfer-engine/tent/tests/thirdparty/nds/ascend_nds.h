/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * ubs-io is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *      http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef ASCEND_NDS_H
#define ASCEND_NDS_H

/*
 * Ascend NPU Direct Storage — public C facade (GDS-shaped).
 *
 * This header wraps the vendor NDS async primitives (libnds.so) behind a
 * surface modelled on NVIDIA GPUDirect Storage (cuFile) so that GDS users can
 * migrate with minimal effort. All names are Ascend-flavoured (ndsFile* /
 * NDSfile*_t) to avoid symbol collisions when both libraries coexist.
 *
 * Lifecycle (mirrors cuFile):
 *   ndsFileDriverOpen()          // init driver (parameter-less)
 *   ndsFileRegisterFs(&fs)      // register filesystem topology (user-managed)
 *   ndsFileHandleRegister(&fh, &descr)   // bind an open fd to a handle
 *   ndsFileBufRegister(buf, len, 0)      // pin HBM/CMB VA for P2P
 *   ndsFileBatchIOSetUp(&batch, nr)      // create a batch context
 *   ndsFileBatchIOSubmit(batch, nr, params, 0)
 *   ndsFileBatchIOGetStatus(batch, min, &got, events, timeout)
 *   ndsFileRead/Write(fh, buf, size, foff, boff)  // sync convenience
 *   ndsFileBatchIODestroy / ndsFileHandleDeregister / ndsFileBufDeregister
 *   ndsFileDeregisterFs(&fs) / ndsFileDriverClose()
 *
 * Conventions (deviate from cuFile only where noted):
 *   - Functions return int: 0 on success, -errno on failure.
 *   - ndsFileRead/Write and NDSfileIOEvents_t.ret also return int (0/-errno);
 *     unlike cuFileRead/Write (which return ssize_t bytes transferred), NDS
 *     completions report only success/failure with no byte count, so the number
 *     of bytes transferred is not returned (a 0 implies the full request
 *     completed — NDS per-iov DMA is all-or-nothing).
 *   - I/O is fully async via the batch API; sync read/write are convenience
 *     wrappers (submit + blocking reap). There are NO stream/async APIs.
 *   - Offsets / lengths / buffer addresses must be 512-byte aligned.
 *
 * Error codes (all functions return int: 0 on success, negative = -errno;
 * Linux errno values shown in parentheses):
 *     0              success. For ndsFileRead/Write a 0 means the full
 *                    request completed (no byte count is returned).
 *     -EINVAL  (22)  invalid argument — null pointer, size 0 or > 4 GiB,
 *                    wrong handle type, zero batch capacity, etc.
 *     -ENODEV  (19)  driver not open — call ndsFileDriverOpen() first.
 *     -EBUSY   (16)  driver already open (duplicate ndsFileDriverOpen).
 *     -ENOMEM  (12)  out of memory (handle allocation).
 *     -EIO      (5)  partial submit — fewer entries accepted than requested;
 *                    already-queued entries are NOT rolled back, reap them
 *                    with ndsFileBatchIOGetStatus(). Also passed through
 *                    from NDS native I/O failures.
 *     -ETIMEDOUT (110) synchronous reap timeout — getevents did not return
 *                    the expected count within 30 s (ndsFileRead/Write).
 *     -ENOENT   (2)  libnds.so not found (dlopen failed on all paths).
 *     -ENOSYS  (38)  NDS symbol missing (dlsym failed / not loaded).
 *     other -errno   passed through from libnds.so (e.g. -EACCES, -EFAULT,
 *                    -EBADF for permission / bad address / bad fd).
 *
 * Per-completion event: NDSfileIOEvents_t.ret uses the same 0/-errno
 * convention (0 = the request completed, -errno = it failed).
 *
 * Unsupported cuFile APIs (intentionally not declared):
 *   cuFileReadAsync / cuFileWriteAsync, cuFileStreamRegister / Deregister,
 *   cuFileBatchIOCancel, cuFileDriverGetProperties / SetPollMode / SetMax*
 *   (no driver tuning knobs). Define NDS_FILE_NO_ASYNC at build time to detect.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h> /* off_t */
#include <time.h>      /* struct timespec */

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time marker: async-stream / tuning APIs are unavailable. */
#define NDS_FILE_NO_ASYNC 1

/* Opaque handle types (mirror cuFile). */
typedef void *NDSfileHandle_t;       /* registered file handle (wraps an fd) */
typedef void *NDSfileBatchHandle_t;  /* batch I/O context */

/* File handle descriptor (mirror CUfileDescr_t). */
enum NDSfileHandleType {
    NDS_FILE_HANDLE_TYPE_OPAQUE_FD = 1, /* Linux fd (the only supported type) */
};

typedef struct NDSfileDescr {
    enum NDSfileHandleType type;
    union {
        int fd; /* Linux open file / block-device fd */
    } handle;
} NDSfileDescr_t;

/*
 * Filesystem topology descriptor for ndsFileRegisterFs(). NDS requires the
 * underlying block-device fds to be registered explicitly (cuFileDriverOpen is
 * parameter-less and auto-detects; NDS exposes topology registration to the
 * user). Each fd must be opened with O_DIRECT by the caller and kept open
 * until after ndsFileDeregisterFs(). Registered topologies remain active until
 * ndsFileDeregisterFs() or ndsFileDriverClose() (nds_exit tears them down).
 */
typedef struct NDSfileFsDesc {
    const int *fs_fd;     /* topology fd array (caller-owned, kept open) */
    unsigned int fs_fd_cnt; /* >= 1 */
    unsigned int reserved; /* must be 0 */
} NDSfileFsDesc_t;

/* I/O opcode (mirror CUfileOpcode_t). */
typedef enum {
    NDS_FILE_READ = 0,
    NDS_FILE_WRITE = 1,
} NDSfileOpcode_t;

/*
 * Single batch I/O parameter (mirror CUfileIOParams_t.u.batch + fh + opcode).
 * Single buffer per entry; the facade converts to a one-element scatter-gather
 * vector for NDS.
 */
typedef struct NDSfileIOParams {
    void *bufPtr_base;    /* device/host memory base */
    off_t file_offset;    /* offset in the target file/device */
    off_t bufPtr_offset;  /* offset into bufPtr_base */
    size_t size;          /* transfer length (<= 4 GiB per entry) */
    NDSfileHandle_t fh;   /* registered file handle */
    NDSfileOpcode_t opcode;
    void *cookie;        /* echoed in completion events */
} NDSfileIOParams_t;

/* Completion status (mirror CUfileStatus_t). */
typedef enum {
    NDS_FILE_WAITING = 0x000001,
    NDS_FILE_PENDING = 0x000002,
    NDS_FILE_INVALID = 0x000004,
    NDS_FILE_CANCELED = 0x000008,
    NDS_FILE_COMPLETE = 0x000010,
    NDS_FILE_TIMEOUT = 0x000020,
    NDS_FILE_FAILED = 0x000040,
} NDSfileStatus_t;

typedef struct NDSfileIOEvents {
    void *cookie;
    NDSfileStatus_t status; /* NDS_FILE_COMPLETE on success, NDS_FILE_FAILED otherwise */
    int ret;               /* 0 on success (byte count unavailable); -errno on failure */
} NDSfileIOEvents_t;

/* Buffer-register flags (GDS-compatible values; NDS ignores them, masks to 0). */
#define NDS_FILE_RDMA_REGISTER 1
#define NDS_FILE_RDMA_RELAXED_ORDERING (1 << 1)

/*
 * Initialise the NDS driver. Parameter-less (mirrors cuFileDriverOpen): the
 * filesystem topology is registered separately via ndsFileRegisterFs() after
 * the driver is open. Call once, single-threaded. Returns 0/-errno.
 */
int ndsFileDriverOpen(void);

/*
 * Register / deregister a filesystem topology (the block-device fds the NDS
 * P2P path targets). Register after ndsFileDriverOpen(); a topology stays
 * registered until ndsFileDeregisterFs() or ndsFileDriverClose() (nds_exit
 * tears everything down). The facade stores a copy of the fd array, so the
 * caller's @desc need only outlive the ndsFileRegisterFs() call; pass the same
 * fs_fd pointer to ndsFileDeregisterFs() to look it up. Returns 0/-errno.
 */
int ndsFileRegisterFs(const NDSfileFsDesc_t *desc);
int ndsFileDeregisterFs(const NDSfileFsDesc_t *desc);

/* Tear down the driver; auto-deregisters all registered memory. Returns 0. */
int ndsFileDriverClose(void);

/* Register an open fd as a file handle. Caller owns the fd (close it yourself
 * after deregister). Returns 0/-errno; *fh set on success. */
int ndsFileHandleRegister(NDSfileHandle_t *fh, const NDSfileDescr_t *descr);

/* Deregister a file handle (does not close the fd). */
void ndsFileHandleDeregister(NDSfileHandle_t fh);

/* Pin / unpin a VA range for P2P. flags currently ignored (masked to 0).
 * Registered ranges are auto-detected at submit time and routed via the
 * registered-mem path. Returns 0/-errno. */
int ndsFileBufRegister(void *buf, size_t length, int flags);
int ndsFileBufDeregister(void *buf);

/* Create / destroy a batch I/O context. nr is a queue-capacity hint.
 * Destroy drains outstanding I/O before releasing. Returns 0/-errno. */
int ndsFileBatchIOSetUp(NDSfileBatchHandle_t *batch, unsigned nr);
int ndsFileBatchIODestroy(NDSfileBatchHandle_t batch);

/*
 * Submit nr I/O parameters. Each entry is converted to one NDS control block.
 * NDS uses fail-stop partial submit: if fewer than nr are accepted the facade
 * returns -EIO (already-queued entries are NOT rolled back — reap them with
 * ndsFileBatchIOGetStatus). flags reserved (pass 0). Returns 0/-errno.
 */
int ndsFileBatchIOSubmit(NDSfileBatchHandle_t batch, unsigned nr,
                         NDSfileIOParams_t *params, unsigned flags);

/*
 * Reap completions: wait for at least min_nr, up to *nr (capacity). On return
 * *nr holds the actual event count. timeout NULL = wait forever, {0,0} =
 * non-blocking. Returns 0/-errno (see NDSfileIOEvents_t.ret for per-event).
 */
int ndsFileBatchIOGetStatus(NDSfileBatchHandle_t batch, unsigned min_nr,
                            unsigned *nr, NDSfileIOEvents_t *events,
                            struct timespec *timeout);

/* Synchronous read/write convenience (NDS is async-only internally). Uses an
 * ephemeral context per call. Returns 0 on success, -errno on failure.
 * NDS completion reports only success/failure, not a byte count, so the number
 * of bytes transferred is not returned; a 0 return implies the full request
 * completed (NDS per-iov DMA is all-or-nothing). */
int ndsFileRead(NDSfileHandle_t fh, void *buf, size_t size,
                 off_t file_offset, off_t bufPtr_offset);
int ndsFileWrite(NDSfileHandle_t fh, const void *buf, size_t size,
                 off_t file_offset, off_t bufPtr_offset);

/* Query the NDS API version (NDS_API_VERSION). Usable before driver open. */
int ndsFileGetVersion(int *version);

#ifdef __cplusplus
}
#endif
#endif /* ASCEND_NDS_H */
