// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0
#include "tent/transport/nds/nds_transport.h"

#include <ascend_nds.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace mooncake::tent {
namespace {
constexpr size_t kAlignment = 512;
constexpr size_t kSliceSize = 16 * 1024 * 1024;
constexpr size_t kMaxQueueDepth = 1024;

Status ndsError(const char* operation, int rc) {
    return Status::InternalError(std::string(operation) + " returned " +
                                 std::to_string(rc));
}
Status storageSize(int fd, uint64_t& length) {
    struct stat st{};
    if (fstat(fd, &st)) return ndsError("fstat", -errno);
    if (S_ISREG(st.st_mode) && st.st_size >= 0) {
        length = st.st_size;
        return Status::OK();
    }
    return Status::InvalidArgument("NDS requires a regular file");
}
struct Registration {
    size_t length;
    size_t users;
    size_t pinned_users;
};
// Native initialization is process-wide and not reentrant. Serialize lifecycle,
// registrations and queue access; polling is always nonblocking under this
// lock.
struct Driver {
    std::mutex mutex;
    size_t users = 0;
    std::map<uint64_t, Registration> buffers;
};
Driver& driver() {
    static Driver state;
    return state;
}
bool contains(uint64_t base, size_t size, uint64_t addr, size_t length) {
    return addr >= base && addr - base <= size &&
           length <= size - (addr - base);
}
struct File {
    int fd = -1;
    NDSfileHandle_t handle = nullptr;
    bool registered = false;
    struct stat identity{};
    ~File() {
        if (handle) ndsFileHandleDeregister(handle);
        if (registered) {
            NDSfileFsDesc_t fs{&fd, 1, 0};
            int rc = ndsFileDeregisterFs(&fs);
            if (rc) LOG(ERROR) << "ndsFileDeregisterFs returned " << rc;
        }
        if (fd >= 0) close(fd);
    }
};
struct Task {
    Request request{};
    uint64_t buffer_base = 0;
    TransferStatus status{PENDING, 0};
};
struct Part {
    size_t task;
    size_t bytes;
};
struct Io {
    NDSfileIOParams_t params{};
    std::vector<Part> parts;
    std::shared_ptr<File> file;
    uint64_t buffer_base = 0;
    size_t bytes = 0;
    bool done = false;
};
struct NdsBatch : Transport::SubBatch {
    explicit NdsBatch(size_t capacity) : capacity(capacity) {}
    size_t size() const override { return tasks.size(); }
    bool needsProgressPolling() const override { return true; }
    size_t capacity;
    std::vector<Task> tasks;
    std::vector<std::unique_ptr<Io>> ios;
    NDSfileBatchHandle_t context = nullptr;
    size_t submitted = 0;
    size_t inflight = 0;
    std::vector<NDSfileIOParams_t> submission;
    std::vector<NDSfileIOEvents_t> events;

    // Facade destroy drains and consumes the batch even when it returns an I/O
    // error. Mark failure only after this fence; never retry the freed pointer.
    void fail() {
        if (context) {
            int rc = ndsFileBatchIODestroy(context);
            context = nullptr;
            if (rc) LOG(ERROR) << "ndsFileBatchIODestroy returned " << rc;
        }
        inflight = 0;
        submitted = ios.size();
        for (auto& task : tasks)
            if (task.status.s == PENDING) task.status.s = FAILED;
    }
    void refill(size_t depth) {
        if (!context || submitted == ios.size()) return;
        size_t count = std::min(depth - inflight, ios.size() - submitted);
        if (!count) return;
        submission.clear();
        for (size_t i = submitted; i < submitted + count; ++i) {
            auto& io = *ios[i];
            io.params.cookie = reinterpret_cast<void*>(i + 1);
            submission.push_back(io.params);
        }
        int rc = ndsFileBatchIOSubmit(context, count, submission.data(), 0);
        // A partial prefix may already DMA. Fence before reporting failure;
        // the engine owns any failover according to its runtime policy.
        if (rc) {
            LOG(ERROR) << "ndsFileBatchIOSubmit returned " << rc;
            fail();
            return;
        }
        submitted += count;
        inflight += count;
    }
    void poll(size_t depth) {
        if (!context || !inflight) return;
        timespec timeout{0, 0};
        events.resize(depth);
        unsigned count = inflight;
        int rc = ndsFileBatchIOGetStatus(context, 0, &count, events.data(),
                                         &timeout);
        if (rc || count > inflight) {
            LOG(ERROR) << "ndsFileBatchIOGetStatus returned " << rc
                       << ", count=" << count;
            fail();
            return;
        }
        for (unsigned i = 0; i < count; ++i) {
            size_t cookie = reinterpret_cast<uintptr_t>(events[i].cookie);
            size_t index = cookie - 1;
            if (!cookie || index >= submitted || ios[index]->done ||
                events[i].ret || events[i].status != NDS_FILE_COMPLETE) {
                LOG(ERROR) << "Invalid NDS completion: cookie=" << index
                           << ", result=" << events[i].ret;
                fail();
                return;
            }
            auto& io = *ios[index];
            io.done = true;
            for (const auto& part : io.parts) {
                auto& task = tasks[part.task];
                task.status.transferred_bytes += part.bytes;
                if (task.status.transferred_bytes == task.request.length)
                    task.status.s = COMPLETED;
            }
        }
        inflight -= count;
        // Refill freed slots even while other requests remain in flight.
        refill(depth);
    }
};
}  // namespace

struct NdsTransport::Impl {
    bool installed = false;
    bool register_memory = false;
    size_t depth = 128;
    size_t io_bytes = kSliceSize;
    std::shared_ptr<ControlService> metadata;
    std::map<uint64_t, size_t> buffers;
    std::set<NdsBatch*> batches;
    std::map<std::pair<std::string, int>, std::shared_ptr<File>> files;
    std::vector<NDSfileBatchHandle_t> idle_contexts;

    Status validate(const Request& request, uint64_t& base, std::string& path) {
        auto addr = reinterpret_cast<uint64_t>(request.source);
        if (!installed || !request.length || addr % kAlignment ||
            request.length % kAlignment || request.target_offset % kAlignment ||
            (request.opcode != Request::READ &&
             request.opcode != Request::WRITE) ||
            request.target_offset >
                uint64_t(std::numeric_limits<off_t>::max()) ||
            request.length > uint64_t(std::numeric_limits<off_t>::max()) -
                                 request.target_offset)
            return Status::InvalidArgument("Invalid NDS request or alignment");
        auto check = [&](uint64_t address, size_t length) {
            auto it = buffers.upper_bound(address);
            if (!length || address % kAlignment || length % kAlignment ||
                it == buffers.begin())
                return false;
            --it;
            if (!contains(it->first, it->second, address, length)) return false;
            base = it->first;
            return true;
        };
        if (!check(addr, request.length)) {
            return Status::AddressNotRegistered(
                "NDS source exceeds registration");
        }
        return metadata->segmentManager().withCachedSegment(
            request.target_id, [&](SegmentDesc* segment) {
                auto local = metadata->segmentManager().getLocal();
                if (segment->type != SegmentType::File || !local ||
                    local->machine_id.empty() ||
                    segment->machine_id != local->machine_id ||
                    !segment->name.starts_with(kLocalFileSegmentPrefix))
                    return Status::InvalidArgument(
                        "NDS requires a local File Segment");
                const auto& entries =
                    std::get<FileSegmentDesc>(segment->detail).buffers;
                if (entries.size() != 1 || entries[0].offset != 0 ||
                    !contains(0, entries[0].length, request.target_offset,
                              request.length))
                    return Status::InvalidArgument(
                        "NDS request exceeds File Segment");
                path = entries[0].path;
                return Status::OK();
            });
    }
    Status openFile(const std::string& path, int access,
                    std::shared_ptr<File>& file) {
        struct stat current{};
        if (stat(path.c_str(), &current)) return ndsError("stat", -errno);
        auto key = std::make_pair(path, access);
        auto it = files.find(key);
        if (it != files.end() &&
            current.st_dev == it->second->identity.st_dev &&
            current.st_ino == it->second->identity.st_ino &&
            current.st_rdev == it->second->identity.st_rdev) {
            file = it->second;
            return Status::OK();
        }
        auto opened = std::make_shared<File>();
        opened->fd = open(path.c_str(), access | O_DIRECT | O_CLOEXEC);
        if (opened->fd < 0) return ndsError("open", -errno);
        if (fstat(opened->fd, &opened->identity))
            return ndsError("fstat", -errno);
        NDSfileFsDesc_t fs{&opened->fd, 1, 0};
        int rc = ndsFileRegisterFs(&fs);
        if (rc) return ndsError("ndsFileRegisterFs", rc);
        opened->registered = true;
        NDSfileDescr_t desc{};
        desc.type = NDS_FILE_HANDLE_TYPE_OPAQUE_FD;
        desc.handle.fd = opened->fd;
        rc = ndsFileHandleRegister(&opened->handle, &desc);
        if (rc) return ndsError("ndsFileHandleRegister", rc);
        files[key] = opened;
        file = std::move(opened);
        return Status::OK();
    }
    Status acquire(NdsBatch& batch) {
        if (batch.context) return Status::OK();
        if (!idle_contexts.empty()) {
            batch.context = idle_contexts.back();
            idle_contexts.pop_back();
            return Status::OK();
        }
        int rc = ndsFileBatchIOSetUp(&batch.context, depth);
        if (rc || !batch.context) return ndsError("ndsFileBatchIOSetUp", rc);
        return Status::OK();
    }
    void append(NdsBatch& batch, size_t task_id,
                const std::shared_ptr<File>& file, size_t first_io) {
        auto& task = batch.tasks[task_id];
        const auto& request = task.request;
        auto opcode =
            request.opcode == Request::READ ? NDS_FILE_READ : NDS_FILE_WRITE;
        const auto address = reinterpret_cast<uint64_t>(request.source);
        const auto base = task.buffer_base;
        size_t consumed = 0;
        while (consumed < request.length) {
            Io* io =
                batch.ios.size() > first_io ? batch.ios.back().get() : nullptr;
            if (!io || io->file != file || io->params.opcode != opcode ||
                uint64_t(io->params.file_offset) + io->bytes !=
                    request.target_offset + consumed ||
                io->bytes == io_bytes ||
                reinterpret_cast<uint64_t>(io->params.bufPtr_base) +
                        io->params.bufPtr_offset + io->bytes !=
                    address + consumed ||
                io->buffer_base != base) {
                auto added = std::make_unique<Io>();
                added->file = file;
                added->buffer_base = base;
                added->params.opcode = opcode;
                added->params.fh = file->handle;
                added->params.bufPtr_base = reinterpret_cast<void*>(base);
                added->params.bufPtr_offset = address + consumed - base;
                added->params.file_offset = request.target_offset + consumed;
                batch.ios.push_back(std::move(added));
                io = batch.ios.back().get();
            }
            size_t length =
                std::min(io_bytes - io->bytes, request.length - consumed);
            io->params.size += length;
            io->parts.push_back({task_id, length});
            io->bytes += length;
            consumed += length;
        }
    }
    void drain(uint64_t base, bool all) {
        for (auto* batch : batches) {
            if (all || std::any_of(batch->tasks.begin(), batch->tasks.end(),
                                   [&](const Task& task) {
                                       return task.status.s == PENDING &&
                                              task.buffer_base == base;
                                   }))
                batch->fail();
        }
    }
    Status unregister(uint64_t addr) {
        auto it = buffers.find(addr);
        if (it == buffers.end()) return Status::OK();
        drain(addr, false);
        auto shared = driver().buffers.find(addr);
        if (register_memory) {
            if (shared->second.pinned_users == 1) {
                int rc = ndsFileBufDeregister(reinterpret_cast<void*>(addr));
                if (rc) return ndsError("ndsFileBufDeregister", rc);
            }
            --shared->second.pinned_users;
        }
        if (--shared->second.users == 0) driver().buffers.erase(shared);
        buffers.erase(it);
        return Status::OK();
    }
};

NdsTransport::NdsTransport() : impl_(std::make_unique<Impl>()) {}
NdsTransport::~NdsTransport() {
    auto status = uninstall();
    CHECK(status.ok()) << status.ToString();
    for (auto* batch : impl_->batches) delete batch;
}
Status NdsTransport::install(std::string&,
                             std::shared_ptr<ControlService> metadata,
                             std::shared_ptr<Topology>,
                             std::shared_ptr<Config> conf) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    if (impl_->installed) return Status::OK();
    if (!metadata) return Status::InvalidArgument("NDS requires metadata");
    int depth = conf ? conf->get("transports/nds/io_batch_depth", 128) : 128;
    int bytes = conf ? conf->get("transports/nds/io_bytes", int(kSliceSize))
                     : kSliceSize;
    if (depth <= 0 || depth > int(kMaxQueueDepth))
        return Status::InvalidArgument(
            "NDS io_batch_depth must be in [1, 1024]");
    if (bytes <= 0 || bytes % kAlignment || bytes > 64 * 1024 * 1024)
        return Status::InvalidArgument(
            "NDS io_bytes must be aligned and in [512, 64 MiB]");
    if (driver().users == 0) {
        int rc = ndsFileDriverOpen();
        if (rc) return ndsError("ndsFileDriverOpen", rc);
        int version = 0;
        rc = ndsFileGetVersion(&version);
        if (rc || version != 1) {
            ndsFileDriverClose();
            return Status::InvalidArgument("Unsupported NDS API version");
        }
    }
    ++driver().users;
    impl_->depth = depth;
    impl_->io_bytes = bytes;
    impl_->register_memory =
        conf && conf->get("transports/nds/register_memory", false);
    impl_->metadata = std::move(metadata);
    caps.gpu_to_file = Platform::getLoader().type() == "ascend";
    impl_->installed = true;
    return Status::OK();
}
Status NdsTransport::quiesce() {
    std::lock_guard<std::mutex> lock(driver().mutex);
    impl_->drain(0, true);
    return Status::OK();
}
Status NdsTransport::uninstall() {
    std::lock_guard<std::mutex> lock(driver().mutex);
    if (!impl_->installed) return Status::OK();
    impl_->drain(0, true);
    while (!impl_->buffers.empty())
        CHECK_STATUS(impl_->unregister(impl_->buffers.begin()->first));
    for (auto* context : impl_->idle_contexts) {
        int rc = ndsFileBatchIODestroy(context);
        if (rc) LOG(ERROR) << "ndsFileBatchIODestroy returned " << rc;
    }
    impl_->idle_contexts.clear();
    for (auto* batch : impl_->batches) batch->ios.clear();
    impl_->files.clear();
    if (driver().users == 1) {
        int rc = ndsFileDriverClose();
        if (rc) return ndsError("ndsFileDriverClose", rc);
    }
    --driver().users;
    impl_->installed = false;
    impl_->metadata.reset();
    return Status::OK();
}
Status NdsTransport::allocateSubBatch(SubBatchRef& batch, size_t max_size) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    if (!impl_->installed || !max_size)
        return Status::InvalidArgument(
            "NDS is unavailable or batch size is zero");
    auto* allocated = new NdsBatch(max_size);
    impl_->batches.insert(allocated);
    batch = allocated;
    return Status::OK();
}
Status NdsTransport::freeSubBatch(SubBatchRef& batch) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    auto* entry = dynamic_cast<NdsBatch*>(batch);
    if (!entry || !impl_->batches.count(entry))
        return Status::InvalidArgument("Invalid NDS batch");
    if (entry->context && !entry->inflight &&
        entry->submitted == entry->ios.size() &&
        impl_->idle_contexts.size() < 32) {
        impl_->idle_contexts.push_back(entry->context);
        entry->context = nullptr;
    } else
        entry->fail();
    impl_->batches.erase(entry);
    delete entry;
    batch = nullptr;
    return Status::OK();
}
Status NdsTransport::submitTransferTasks(SubBatchRef batch,
                                         const std::vector<Request>& requests) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    auto* entry = dynamic_cast<NdsBatch*>(batch);
    if (!impl_->installed || !entry || !impl_->batches.count(entry) ||
        requests.size() > entry->capacity - entry->size())
        return Status::InvalidArgument("Invalid NDS batch or capacity");
    entry->poll(impl_->depth);
    // Revalidate each distinct file once per submission. Accepted I/O retains
    // descriptor ownership if the pathname is replaced later.
    std::map<std::pair<std::string, int>,
             std::pair<std::shared_ptr<File>, uint64_t>>
        opened;
    size_t first_io = entry->ios.size();
    entry->tasks.reserve(entry->tasks.size() + requests.size());
    for (const auto& request : requests) {
        Task task;
        task.request = request;
        std::string path;
        auto status = impl_->validate(request, task.buffer_base, path);
        std::shared_ptr<File> file;
        if (status.ok()) {
            int access = request.opcode == Request::READ ? O_RDONLY : O_RDWR;
            auto key = std::make_pair(path, access);
            auto it = opened.find(key);
            if (it == opened.end()) {
                uint64_t length = 0;
                status = impl_->openFile(path, access, file);
                if (status.ok()) status = storageSize(file->fd, length);
                if (status.ok())
                    it =
                        opened.emplace(key, std::make_pair(file, length)).first;
            }
            if (status.ok()) {
                file = it->second.first;
                if (!contains(0, it->second.second, request.target_offset,
                              request.length))
                    status = Status::InvalidArgument(
                        "NDS request exceeds storage size");
            }
        }
        if (!status.ok()) {
            LOG(ERROR) << status.ToString();
            task.status.s = FAILED;
        }
        entry->tasks.push_back(task);
        if (status.ok())
            impl_->append(*entry, entry->tasks.size() - 1, file, first_io);
    }
    if (entry->ios.size() > first_io) {
        auto status = impl_->acquire(*entry);
        if (status.ok())
            entry->refill(impl_->depth);
        else {
            LOG(ERROR) << status.ToString();
            entry->fail();
        }
    }
    return Status::OK();
}
Status NdsTransport::getTransferStatus(SubBatchRef batch, int task_id,
                                       TransferStatus& status) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    auto* entry = dynamic_cast<NdsBatch*>(batch);
    if (!entry || !impl_->batches.count(entry) || task_id < 0 ||
        size_t(task_id) >= entry->size())
        return Status::InvalidArgument("Invalid NDS task ID");
    if (entry->tasks[task_id].status.s == PENDING) entry->poll(impl_->depth);
    status = entry->tasks[task_id].status;
    return Status::OK();
}
Status NdsTransport::addMemoryBuffer(BufferDesc& desc, const MemoryOptions&) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    if (!impl_->installed) return Status::InvalidArgument("NDS is unavailable");
    if (LocationParser(desc.location).type() != "npu") return Status::OK();
    if (!desc.addr || !desc.length || desc.addr % kAlignment ||
        desc.length % kAlignment ||
        desc.length > std::numeric_limits<uint64_t>::max() - desc.addr)
        return Status::InvalidArgument("Invalid NDS memory registration");
    if (impl_->buffers.count(desc.addr))
        return Status::InvalidArgument("NDS buffer is already registered");
    auto& buffers = driver().buffers;
    auto next = buffers.lower_bound(desc.addr);
    bool shared = next != buffers.end() && next->first == desc.addr &&
                  next->second.length == desc.length;
    if (!shared &&
        ((next != buffers.end() && next->first < desc.addr + desc.length) ||
         (next != buffers.begin() &&
          std::prev(next)->first + std::prev(next)->second.length > desc.addr)))
        return Status::InvalidArgument("Overlapping NDS registrations");
    if (impl_->register_memory && (!shared || !next->second.pinned_users)) {
        int rc = ndsFileBufRegister(reinterpret_cast<void*>(desc.addr),
                                    desc.length, 0);
        if (rc) return ndsError("ndsFileBufRegister", rc);
    }
    if (!shared)
        next =
            buffers.emplace(desc.addr, Registration{desc.length, 0, 0}).first;
    ++next->second.users;
    if (impl_->register_memory) ++next->second.pinned_users;
    impl_->buffers.emplace(desc.addr, desc.length);
    desc.transports.push_back(NDS);
    return Status::OK();
}
Status NdsTransport::removeMemoryBuffer(BufferDesc& desc) {
    std::lock_guard<std::mutex> lock(driver().mutex);
    return impl_->unregister(desc.addr);
}
}  // namespace mooncake::tent
