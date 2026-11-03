// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <thread>
#include <sstream>
#include "tent/metrics/tent_metrics.h"
#include "tent/common/config_lifecycle.h"

#include "nds_test_backend.h"
#include "tent/common/config.h"
#include "tent/transfer_engine.h"
#include "tent/runtime/transfer_engine_impl.h"
#include "tent/runtime/transport.h"

namespace mooncake::tent {
namespace {
using nds_test::state;
constexpr size_t kSize = 48 * 1024 * 1024;

class NdsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        state = {};
        ASSERT_EQ(posix_memalign(&buffer, 4096, kSize), 0);
        state.npu_base = reinterpret_cast<uint64_t>(buffer);
        state.npu_size = kSize;
        memset(buffer, 0x5a, kSize);
        auto name =
            (std::filesystem::temp_directory_path() / "tent-nds-test-XXXXXX")
                .string();
        int fd = mkstemp(name.data());
        ASSERT_GE(fd, 0);
        path = name;
        ASSERT_EQ(ftruncate(fd, kSize), 0);
        close(fd);
        cfg = std::make_shared<Config>();
        cfg->set("metadata_type", "p2p");
        cfg->set("rpc_server_hostname", "127.0.0.1");
        cfg->set("rpc_server_port", "0");
        cfg->set("log_level", "error");
        for (const auto* name : {"tcp", "rdma", "shm", "io_uring", "gds",
                                 "nvlink", "mnnvl", "ascend_direct"})
            cfg->set(std::string("transports/") + name + "/enable", false);
        cfg->set("transports/nds/enable", true);
        cfg->set("transports/nds/io_batch_depth", 2);
        cfg->set("transports/nds/register_memory", true);
        engine = std::make_unique<TransferEngine>(cfg);
        ASSERT_TRUE(engine->available());
        MemoryOptions options;
        options.location =
            "npu:0";  // CPU storage emulates registered HBM in this test.
        options.type = NDS;
        ASSERT_TRUE(engine->registerLocalMemory(buffer, kSize, options).ok());
        ASSERT_TRUE(engine->openSegment(segment, "file://" + path).ok());
    }

    void TearDown() override {
        state.destroy_error = state.unpin_error = false;
        engine.reset();
        EXPECT_EQ(state.live_contexts, 0);
        EXPECT_EQ(state.live_files, 0);
        EXPECT_EQ(state.live_topologies, 0);
        EXPECT_EQ(state.pins, state.unpins);
        EXPECT_EQ(state.opens, state.closes);
        free(buffer);
        unlink(path.c_str());
    }

    Request request(Request::OpCode opcode, size_t length = 4096,
                    size_t offset = 0) {
        return {opcode, static_cast<char*>(buffer) + offset, segment, offset,
                length};
    }

    TransferStatus poll(BatchID batch, size_t task = 0) {
        TransferStatus status{};
        for (int i = 0; i < 16; ++i) {
            auto result = engine->getTransferStatus(batch, task, status);
            EXPECT_TRUE(result.ok()) << result.ToString();
            if (!result.ok() || status.s != PENDING) break;
        }
        return status;
    }

    std::shared_ptr<Config> cfg;
    std::unique_ptr<TransferEngine> engine;
    void* buffer = nullptr;
    std::string path;
    SegmentID segment = 0;
};

TEST_F(NdsTest, WindowedRoundTripAndIndependentTasks) {
    auto batch = engine->allocateBatch(3);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::WRITE, kSize)}).ok());
    auto status = poll(batch);
    EXPECT_EQ(status.s, COMPLETED);
    EXPECT_EQ(status.transferred_bytes, kSize);
    EXPECT_EQ(state.submits, 2);
    memset(buffer, 0, kSize);
    // Reuse the batch and verify stable task IDs for appended requests.
    ASSERT_TRUE(
        engine
            ->submitTransfer(batch, {request(Request::READ, 4096),
                                     request(Request::READ, 8192, 4096)})
            .ok());
    status = poll(batch, 2);
    EXPECT_EQ(status.s, COMPLETED);
    EXPECT_EQ(status.transferred_bytes, 8192);
    status = poll(batch, 1);
    EXPECT_EQ(status.transferred_bytes, 4096);
    for (size_t i = 0; i < 12288; ++i)
        ASSERT_EQ(static_cast<unsigned char*>(buffer)[i], 0x5a);
    ASSERT_TRUE(engine->freeBatch(batch).ok());
}

TEST_F(NdsTest, PartialCompletionSurvivesZeroEventPolls) {
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::READ, kSize)}).ok());
    TransferStatus status{};
    ASSERT_TRUE(engine->getTransferStatus(batch, 0, status).ok());
    EXPECT_EQ(status.s, PENDING);
    EXPECT_EQ(status.transferred_bytes, 16 * 1024 * 1024);
    EXPECT_EQ(state.submits, 2);  // Refill before the first window drains.
    EXPECT_EQ(state.max_inflight, 2);
    state.pause = true;
    ASSERT_TRUE(engine->getTransferStatus(batch, 0, status).ok());
    EXPECT_EQ(status.s, PENDING);
    EXPECT_EQ(status.transferred_bytes, 16 * 1024 * 1024);
    state.pause = false;
    EXPECT_EQ(poll(batch).s, COMPLETED);
}

TEST_F(NdsTest, PartialWriteSubmitDrainsBeforeFailureWithoutFallback) {
    state.partial_submit = true;
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::WRITE, kSize)}).ok());
    EXPECT_EQ(state.drains, 1);
    EXPECT_EQ(poll(batch).s, FAILED);
    EXPECT_EQ(poll(batch).s, FAILED);
    EXPECT_EQ(state.submits, 1);
    EXPECT_EQ(state.live_contexts, 0);
}

TEST_F(NdsTest, DestroyErrorConsumesContextWithoutRetry) {
    state.partial_submit = state.destroy_error = true;
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::READ, kSize)}).ok());
    TransferStatus status{};
    EXPECT_TRUE(engine->getTransferStatus(batch, 0, status).ok());
    EXPECT_EQ(status.s, FAILED);
    EXPECT_EQ(state.live_contexts, 0);
    EXPECT_EQ(state.unpins, 0);
    EXPECT_TRUE(engine->unregisterLocalMemory(buffer, kSize).ok());
    EXPECT_EQ(state.live_contexts, 0);
    EXPECT_EQ(state.unpins, 1);
    EXPECT_EQ(poll(batch).s, FAILED);
}

TEST_F(NdsTest, PollErrorAndMalformedCookieFenceOutstandingIo) {
    for (bool corrupt_cookie : {false, true}) {
        state.poll_error = !corrupt_cookie;
        state.bad_cookie = corrupt_cookie;
        auto batch = engine->allocateBatch(1);
        ASSERT_TRUE(
            engine->submitTransfer(batch, {request(Request::READ, kSize)})
                .ok());
        EXPECT_EQ(poll(batch).s, FAILED);
        EXPECT_EQ(state.live_contexts, 0);
        EXPECT_TRUE(engine->freeBatch(batch).ok());
    }
}

TEST_F(NdsTest, UnpinFailureCanBeRetried) {
    state.unpin_error = true;
    EXPECT_FALSE(engine->unregisterLocalMemory(buffer, kSize).ok());
    state.unpin_error = false;
    EXPECT_TRUE(engine->unregisterLocalMemory(buffer, kSize).ok());
    EXPECT_EQ(state.unpins, 1);
}

TEST_F(NdsTest, SeparateRequestsValidateEachBufferRange) {
    auto first = request(Request::WRITE);
    auto second = request(Request::WRITE);
    second.source = static_cast<char*>(buffer) + kSize / 2;
    second.target_offset = 4096;
    auto batch = engine->allocateBatch(4);
    ASSERT_TRUE(engine->submitTransfer(batch, {first, second}).ok());
    EXPECT_EQ(poll(batch, 0).transferred_bytes, 4096);
    EXPECT_EQ(poll(batch, 1).s, COMPLETED);
    EXPECT_EQ(state.submitted_ios, 2);
    first.length = kSize + 4096;
    second.source = static_cast<char*>(buffer) + kSize;
    ASSERT_TRUE(engine->submitTransfer(batch, {first, second}).ok());
    EXPECT_EQ(poll(batch, 2).s, FAILED);
    EXPECT_EQ(poll(batch, 3).s, FAILED);
    EXPECT_EQ(state.submitted_ios, 2);
}

TEST_F(NdsTest, SeparateRequestsCrossRegistrationsAndReuseContext) {
    engine.reset();
    cfg->set("transports/nds/register_memory", false);
    cfg->set("merge_requests", false);
    engine = std::make_unique<TransferEngine>(cfg);
    MemoryOptions options;
    options.type = NDS;
    options.location = "npu:0";
    auto* second = static_cast<char*>(buffer) + kSize / 2;
    ASSERT_TRUE(engine->registerLocalMemory(buffer, kSize / 2, options).ok());
    ASSERT_TRUE(engine->registerLocalMemory(second, kSize / 2, options).ok());
    ASSERT_TRUE(engine->openSegment(segment, "file://" + path).ok());
    int pins = state.pins;
    for (auto opcode : {Request::WRITE, Request::READ}) {
        auto batch = engine->allocateBatch(2);
        std::vector<Request> requests = {{opcode, buffer, segment, 0, 4096},
                                         {opcode, second, segment, 4096, 4096}};
        if (opcode == Request::READ) {
            memset(buffer, 0, 4096);
            memset(second, 0, 4096);
        }
        ASSERT_TRUE(engine->submitTransfer(batch, requests).ok());
        EXPECT_EQ(poll(batch, 1).s, COMPLETED);
        EXPECT_EQ(poll(batch, 0).transferred_bytes, 4096);
        EXPECT_TRUE(engine->freeBatch(batch).ok());
    }
    EXPECT_EQ(state.submitted_ios, 4);
    EXPECT_EQ(state.context_setups, 1);
    EXPECT_EQ(state.pins, pins);
    for (size_t i = 0; i < 4096; ++i) {
        EXPECT_EQ(static_cast<unsigned char*>(buffer)[i], 0x5a);
        EXPECT_EQ(static_cast<unsigned char>(second[i]), 0x5a);
    }
    auto pending = engine->allocateBatch(2);
    std::vector<Request> reads = {{Request::READ, buffer, segment, 0, 4096},
                                  {Request::READ, second, segment, 4096, 4096}};
    state.pause = true;
    ASSERT_TRUE(engine->submitTransfer(pending, reads).ok());
    ASSERT_TRUE(engine->unregisterLocalMemory(second, kSize / 2).ok());
    EXPECT_EQ(state.drains,
              2);  // Removing either buffer fences the active batch.
    state.pause = false;
    EXPECT_EQ(poll(pending).s, FAILED);
}

TEST_F(NdsTest, RejectsUnalignedOutOfRangeAndCpuMemory) {
    auto invalid = request(Request::READ);
    invalid.target_offset = 1;
    auto out_of_range = request(Request::READ);
    out_of_range.length = kSize + 4096;
    auto unregistered = request(Request::READ);
    unregistered.source = static_cast<char*>(buffer) + kSize;
    auto batch = engine->allocateBatch(3);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {invalid, out_of_range, unregistered})
            .ok());
    for (size_t i = 0; i < 3; ++i) EXPECT_EQ(poll(batch, i).s, FAILED);
    EXPECT_EQ(state.submits, 0);
    ASSERT_TRUE(engine->unregisterLocalMemory(buffer, kSize).ok());
    MemoryOptions cpu;
    cpu.type = NDS;
    cpu.location = "cpu:0";
    state.npu_size = 0;
    ASSERT_TRUE(engine->registerLocalMemory(buffer, kSize, cpu).ok());
    batch = engine->allocateBatch(1);
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::READ)}).ok());
    EXPECT_EQ(poll(batch).s, FAILED);
    EXPECT_EQ(state.pins, 1);
}

TEST_F(NdsTest, SharedDriverAndRegistrationSurviveOtherEngineShutdown) {
    auto other = std::make_unique<TransferEngine>(cfg);
    ASSERT_TRUE(other->available());
    MemoryOptions options;
    options.type = NDS;
    options.location = "npu:0";
    ASSERT_TRUE(other->registerLocalMemory(buffer, kSize, options).ok());
    EXPECT_EQ(state.opens, 1);
    EXPECT_EQ(state.pins, 1);
    other.reset();
    EXPECT_EQ(state.closes, 0);
    EXPECT_EQ(state.unpins, 0);
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::READ)}).ok());
    EXPECT_EQ(poll(batch).s, COMPLETED);
}

TEST_F(NdsTest, RuntimeGateAndDriverFailureLeaveTransportUnavailable) {
    engine.reset();
    cfg->set("transports/nds/enable", false);
    engine = std::make_unique<TransferEngine>(cfg);
    ASSERT_TRUE(engine->available());
    MemoryOptions options;
    options.type = NDS;
    options.location = "npu:0";
    EXPECT_FALSE(engine->registerLocalMemory(buffer, kSize, options).ok());
    engine.reset();
    cfg->set("transports/nds/enable", true);
    state.open_error = true;
    engine = std::make_unique<TransferEngine>(cfg);
    ASSERT_TRUE(engine->available());
    EXPECT_FALSE(engine->registerLocalMemory(buffer, kSize, options).ok());
}

TEST_F(NdsTest, InvalidDepthIsRejectedAtInstall) {
    engine.reset();
    cfg->set("transports/nds/io_batch_depth", 1025);
    engine = std::make_unique<TransferEngine>(cfg);
    ASSERT_TRUE(engine->available());
    MemoryOptions options;
    options.type = NDS;
    options.location = "npu:0";
    EXPECT_FALSE(engine->registerLocalMemory(buffer, kSize, options).ok());
}

TEST_F(NdsTest, UnregisterAndShutdownDrainPendingIo) {
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::WRITE, kSize)}).ok());
    ASSERT_TRUE(engine->unregisterLocalMemory(buffer, kSize).ok());
    EXPECT_EQ(state.drains, 2);
    EXPECT_EQ(state.unpins, 1);
    EXPECT_EQ(poll(batch).s, FAILED);
}

TEST_F(NdsTest, ReopensFileAfterPathReplacement) {
    auto batch = engine->allocateBatch(2);
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::WRITE)}).ok());
    EXPECT_EQ(poll(batch).s, COMPLETED);
    ASSERT_EQ(unlink(path.c_str()), 0);
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, kSize), 0);
    close(fd);
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::READ)}).ok());
    EXPECT_EQ(poll(batch, 1).s, COMPLETED);
    EXPECT_EQ(static_cast<unsigned char*>(buffer)[0], 0);
}

TEST_F(NdsTest, HandleRegistrationFailureReleasesTopologyAndCanRetry) {
    state.handle_error = true;
    auto batch = engine->allocateBatch(2);
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::READ)}).ok());
    EXPECT_EQ(poll(batch).s, FAILED);
    EXPECT_EQ(state.live_topologies, 0);
    EXPECT_EQ(state.live_files, 0);
    state.handle_error = false;
    ASSERT_TRUE(engine->submitTransfer(batch, {request(Request::READ)}).ok());
    EXPECT_EQ(poll(batch, 1).s, COMPLETED);
    EXPECT_EQ(state.live_files, 1);
}

TEST_F(NdsTest, ConcurrentBatchesUseIndependentTaskState) {
    std::vector<std::thread> workers;
    for (size_t i = 0; i < 4; ++i) {
        workers.emplace_back([&, i] {
            auto batch = engine->allocateBatch(1);
            ASSERT_TRUE(engine
                            ->submitTransfer(
                                batch, {request(Request::READ, 4096, i * 4096)})
                            .ok());
            auto status = poll(batch);
            EXPECT_EQ(status.s, COMPLETED);
            EXPECT_EQ(status.transferred_bytes, 4096);
            EXPECT_TRUE(engine->freeBatch(batch).ok());
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(state.submits, 4);
}

TEST_F(NdsTest, ShutdownFencesLiveBatchBeforeDriverClose) {
    auto batch = engine->allocateBatch(1);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::READ, kSize)}).ok());
    engine.reset();
    EXPECT_EQ(state.drains, 2);
    EXPECT_EQ(state.live_contexts, 0);
    EXPECT_EQ(state.unpins, 1);
    EXPECT_EQ(state.closes, 1);
}

class FileFallback : public Transport {
   public:
    struct Batch : SubBatch {
        std::vector<Request> requests;
        size_t size() const override { return requests.size(); }
    };
    FileFallback() {
        caps.dram_to_file = true;
        caps.gpu_to_file =
            true;  // CPU test storage also runs under AscendPlatform.
    }
    Status allocateSubBatch(SubBatchRef& batch, size_t) override {
        batch = new Batch;
        return Status::OK();
    }
    Status freeSubBatch(SubBatchRef& batch) override {
        delete batch;
        batch = nullptr;
        return Status::OK();
    }
    Status submitTransferTasks(SubBatchRef batch,
                               const std::vector<Request>& requests) override {
        if (require_fence) {
            EXPECT_EQ(state.live_contexts, 0);
        }
        ++submits;
        auto& stored = static_cast<Batch*>(batch)->requests;
        stored.insert(stored.end(), requests.begin(), requests.end());
        return Status::OK();
    }
    Status getTransferStatus(SubBatchRef batch, int id,
                             TransferStatus& status) override {
        status = {COMPLETED, static_cast<Batch*>(batch)->requests[id].length};
        return Status::OK();
    }
    int submits = 0;
    bool require_fence = true;
};

TEST_F(NdsTest, ReadAndWriteFailoverWaitForFence) {
    TransferEngineImpl other(cfg);
    ASSERT_TRUE(other.available());
    auto fallback = std::make_shared<FileFallback>();
    other.swapTransportForTest(IOURING, fallback);
    MemoryOptions options;
    options.location = "npu:0";
    options.type = NDS;
    ASSERT_TRUE(other.registerLocalMemory({buffer}, {kSize}, options).ok());
    SegmentID target;
    ASSERT_TRUE(other.openSegment(target, "file://" + path).ok());
    state.partial_submit = true;
    for (auto opcode : {Request::WRITE, Request::READ}) {
        const int previous_submits = fallback->submits;
        auto batch = other.allocateBatch(1);
        auto req = request(opcode, kSize);
        req.target_id = target;
        ASSERT_TRUE(other.submitTransfer(batch, {req}).ok());
        TransferStatus status{};
        ASSERT_TRUE(other.getTransferStatus(batch, 0, status).ok());
        EXPECT_EQ(status.s, PENDING);
        EXPECT_EQ(state.live_contexts, 0);
        ASSERT_TRUE(other.getTransferStatus(batch, 0, status).ok());
        EXPECT_EQ(status.s, COMPLETED);
        EXPECT_EQ(status.transferred_bytes, kSize);
        EXPECT_EQ(fallback->submits, previous_submits + 1);
        ASSERT_TRUE(other.freeBatch(batch).ok());
    }
}

TEST_F(NdsTest, PolicyAndMixedHintsRespectNdsEligibility) {
    cfg->set("policy", nlohmann::json::parse(R"([
        {"name":"nds_storage","segment_type":"file",
         "transports":["nds","io_uring"]},
        {"name":"cpu_only","segment_type":"file","transports":["io_uring"]}
    ])"));
    TransferEngineImpl other(cfg);
    auto fallback = std::make_shared<FileFallback>();
    fallback->require_fence = false;
    other.swapTransportForTest(IOURING, fallback);
    MemoryOptions opts;
    opts.location = "npu:0";
    opts.type = NDS;
    ASSERT_TRUE(other.registerLocalMemory({buffer}, {kSize}, opts).ok());
    SegmentID target;
    ASSERT_TRUE(other.openSegment(target, "file://" + path).ok());
    auto req = request(Request::READ);
    req.target_id = target;
    req.policy_name = "nds_storage";
    req.transport_hint = NDS;
    auto batch = other.allocateBatch(2);
    auto second = req;
    second.transport_hint = IOURING;
    ASSERT_TRUE(other.submitTransfer(batch, {req, second}).ok());
    TransferStatus status{};
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(other.getTransferStatus(batch, i, status).ok());
        EXPECT_EQ(status.s, COMPLETED);
    }
    EXPECT_EQ(state.submits, 1);
    EXPECT_EQ(fallback->submits, 1);
    ASSERT_TRUE(other.freeBatch(batch).ok());
    // An explicit NDS hint cannot override a policy that only permits io_uring.
    req.policy_name = "cpu_only";
    batch = other.allocateBatch(1);
    auto submitted = other.submitTransfer(batch, {req});
    if (submitted.ok()) {
        ASSERT_TRUE(other.getTransferStatus(batch, 0, status).ok());
        EXPECT_EQ(status.s, FAILED);
    }
    EXPECT_EQ(state.submits, 1);
    EXPECT_EQ(fallback->submits, 1);
}

TEST_F(NdsTest, NdsOnlyPolicyPreventsReadAndWriteFailover) {
    cfg->set("policy", nlohmann::json::parse(R"([
        {"name":"nds_storage","segment_type":"file","transports":["nds"]}
    ])"));
    TransferEngineImpl other(cfg);
    auto fallback = std::make_shared<FileFallback>();
    other.swapTransportForTest(IOURING, fallback);
    MemoryOptions opts;
    opts.type = NDS;
    opts.location = "npu:0";
    ASSERT_TRUE(other.registerLocalMemory({buffer}, {kSize}, opts).ok());
    SegmentID target;
    ASSERT_TRUE(other.openSegment(target, "file://" + path).ok());
    state.partial_submit = true;
    for (auto opcode : {Request::WRITE, Request::READ}) {
        auto req = request(opcode, kSize);
        req.target_id = target;
        req.transport_hint = NDS;
        auto batch = other.allocateBatch(1);
        ASSERT_TRUE(other.submitTransfer(batch, {req}).ok());
        TransferStatus status{};
        ASSERT_TRUE(other.getTransferStatus(batch, 0, status).ok());
        EXPECT_EQ(status.s, FAILED);
        EXPECT_EQ(fallback->submits, 0);
        EXPECT_EQ(state.live_contexts, 0);
        ASSERT_TRUE(other.freeBatch(batch).ok());
    }
}

TEST_F(NdsTest, RuntimeQueueProgressCompletesSeparateRequests) {
    engine.reset();
    cfg->set("enable_runtime_queue", true);
    engine = std::make_unique<TransferEngine>(cfg);
    ASSERT_TRUE(engine->available());
    MemoryOptions opts;
    opts.location = "npu:0";
    opts.type = NDS;
    ASSERT_TRUE(engine->registerLocalMemory(buffer, kSize, opts).ok());
    ASSERT_TRUE(engine->openSegment(segment, "file://" + path).ok());
    auto first = request(Request::READ);
    auto second = request(Request::READ);
    second.source = static_cast<char*>(buffer) + 8192;
    second.target_offset = 4096;
    auto batch = engine->allocateBatch(2);
    ASSERT_TRUE(engine->submitTransfer(batch, {first, second}).ok());
    TransferStatus status{PENDING, 0};
    for (int i = 0; i < 500 && status.s == PENDING; ++i) {
        ASSERT_TRUE(engine->progressBatch(batch, status).ok());
        if (status.s == PENDING)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(status.s, COMPLETED);
    EXPECT_EQ(status.transferred_bytes, 8192);
    ASSERT_TRUE(engine->freeBatch(batch).ok());
}

TEST_F(NdsTest, NdsConfigurationIsBootstrapOnly) {
    for (const auto* key :
         {"enable", "io_bytes", "io_batch_depth", "register_memory"})
        EXPECT_EQ(classifyConfigPath(std::string("transports/nds/") + key),
                  ConfigLifecycle::kBootstrapOnly);
    EXPECT_EQ(parseTransportType("nds"), NDS);
    EXPECT_STREQ(transportTypeName(NDS), "nds");
    EXPECT_EQ(c_to_transport_hint(TRANSPORT_NDS), NDS);
}

#if TENT_METRICS_ENABLED
TEST_F(NdsTest, MetricsCountLogicalBytesAndFailureOnce) {
    auto& metrics = TentMetrics::instance();
    MetricsConfig config;
    config.enabled = true;
    config.http_host = "127.0.0.1";
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(socket_fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(
        bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
        0);
    socklen_t address_size = sizeof(address);
    ASSERT_EQ(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address),
                          &address_size),
              0);
    config.http_port = ntohs(address.sin_port);
    close(socket_fd);
    config.report_interval_seconds = 0;
    ASSERT_TRUE(metrics.initialize(config).ok());
    TentMetrics::setEnabled(true);
    auto value = [&](const std::string& series) {
        std::istringstream lines(metrics.getPrometheusMetrics());
        std::string line;
        while (std::getline(lines, line))
            if (line.starts_with(series + " "))
                return std::stod(line.substr(series.size() + 1));
        return 0.0;
    };
    const std::string bytes = "tent_read_bytes_total{transport=\"nds\"}";
    const std::string requests = "tent_read_requests_total{transport=\"nds\"}";
    const std::string errors = "tent_write_failures_total{transport=\"nds\"}";
    const double before_bytes = value(bytes), before_requests = value(requests),
                 before_errors = value(errors);
    auto batch = engine->allocateBatch(2);
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::READ, kSize)}).ok());
    EXPECT_EQ(poll(batch).s, COMPLETED);
    EXPECT_EQ(poll(batch).s, COMPLETED);
    EXPECT_EQ(value(bytes) - before_bytes, kSize);
    EXPECT_EQ(value(requests) - before_requests, 1);
    state.partial_submit = true;
    ASSERT_TRUE(
        engine->submitTransfer(batch, {request(Request::WRITE, kSize)}).ok());
    EXPECT_EQ(poll(batch, 1).s, FAILED);
    EXPECT_EQ(poll(batch, 1).s, FAILED);
    EXPECT_EQ(value(errors) - before_errors, 1);
    ASSERT_TRUE(engine->freeBatch(batch).ok());
}
#endif
}  // namespace
}  // namespace mooncake::tent
