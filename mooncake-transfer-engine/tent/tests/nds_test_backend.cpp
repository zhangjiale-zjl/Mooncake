// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0
// Test-only UBSIO facade implementation, using its public header and contracts.
#include "nds_test_backend.h"
#include <ascend_nds.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <deque>
#include <map>

namespace nds_test {
State state;
}
namespace {
using nds_test::state;
struct FileHandle {
    int fd;
};
struct Batch {
    std::deque<NDSfileIOParams_t> pending;
};
std::map<uint64_t, size_t> registrations;
int perform(const NDSfileIOParams_t& io) {
    auto* addr = static_cast<char*>(io.bufPtr_base) + io.bufPtr_offset;
    int fd = static_cast<FileHandle*>(io.fh)->fd;
    ssize_t rc = io.opcode == NDS_FILE_READ
                     ? pread(fd, addr, io.size, io.file_offset)
                     : pwrite(fd, addr, io.size, io.file_offset);
    return rc == ssize_t(io.size) ? 0 : -EIO;
}
}  // namespace
extern "C" {
int ndsFileDriverOpen() {
    if (state.open_error) return -ENODEV;
    ++state.opens;
    return 0;
}
int ndsFileDriverClose() {
    ++state.closes;
    return 0;
}
int ndsFileGetVersion(int* version) {
    *version = 1;
    return 0;
}
int ndsFileBufRegister(void* addr, size_t length, int) {
    auto base = reinterpret_cast<uint64_t>(addr);
    if (registrations.count(base)) return -EBUSY;
    registrations.emplace(base, length);
    ++state.pins;
    return 0;
}
int ndsFileBufDeregister(void* addr) {
    if (state.unpin_error) return -EIO;
    auto it = registrations.find(reinterpret_cast<uint64_t>(addr));
    if (it == registrations.end()) return -EINVAL;
    registrations.erase(it);
    ++state.unpins;
    return 0;
}
int ndsFileRegisterFs(const NDSfileFsDesc_t*) {
    ++state.live_topologies;
    return 0;
}
int ndsFileDeregisterFs(const NDSfileFsDesc_t*) {
    --state.live_topologies;
    return 0;
}
int ndsFileHandleRegister(NDSfileHandle_t* handle, const NDSfileDescr_t* desc) {
    if (state.handle_error) return -EIO;
    if (desc->type != NDS_FILE_HANDLE_TYPE_OPAQUE_FD) return -EINVAL;
    *handle = new FileHandle{desc->handle.fd};
    ++state.live_files;
    return 0;
}
void ndsFileHandleDeregister(NDSfileHandle_t handle) {
    delete static_cast<FileHandle*>(handle);
    --state.live_files;
}
int ndsFileBatchIOSetUp(NDSfileBatchHandle_t* context, unsigned depth) {
    if (!depth || depth > 1024) return -EINVAL;
    *context = new Batch;
    ++state.live_contexts;
    ++state.context_setups;
    return 0;
}
int ndsFileBatchIODestroy(NDSfileBatchHandle_t context) {
    auto* batch = static_cast<Batch*>(context);
    for (const auto& io : batch->pending) {
        perform(io);
        ++state.drains;
    }
    delete batch;
    --state.live_contexts;
    return state.destroy_error ? -EIO : 0;
}
int ndsFileBatchIOSubmit(NDSfileBatchHandle_t context, unsigned nr,
                         NDSfileIOParams_t* params, unsigned) {
    auto* batch = static_cast<Batch*>(context);
    ++state.submits;
    unsigned accepted = state.partial_submit ? std::min(1u, nr) : nr;
    for (unsigned i = 0; i < accepted; ++i) {
        batch->pending.push_back(params[i]);
        ++state.submitted_ios;
    }
    state.max_inflight =
        std::max(state.max_inflight, int(batch->pending.size()));
    // The public facade returns 0, not an accepted-entry count.
    return accepted == nr ? 0 : -EIO;
}
int ndsFileBatchIOGetStatus(NDSfileBatchHandle_t context, unsigned min_nr,
                            unsigned* nr, NDSfileIOEvents_t* events,
                            timespec* timeout) {
    if (min_nr || !nr || !*nr || !timeout || timeout->tv_sec ||
        timeout->tv_nsec)
        return -EINVAL;
    if (state.poll_error) return -EIO;
    auto* batch = static_cast<Batch*>(context);
    *nr = 0;
    if (state.pause || batch->pending.empty()) return 0;
    // Reverse completions and one event per poll exercise cookie/refill logic.
    auto io = batch->pending.back();
    batch->pending.pop_back();
    events[0].cookie =
        state.bad_cookie ? reinterpret_cast<void*>(UINTPTR_MAX) : io.cookie;
    events[0].ret = perform(io);
    events[0].status = events[0].ret ? NDS_FILE_FAILED : NDS_FILE_COMPLETE;
    *nr = 1;
    return 0;
}
}
