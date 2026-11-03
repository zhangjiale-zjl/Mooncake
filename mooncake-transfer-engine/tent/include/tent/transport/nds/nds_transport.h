// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TENT_NDS_TRANSPORT_H
#define TENT_NDS_TRANSPORT_H

#include "tent/runtime/transport.h"

namespace mooncake::tent {

// The NDS facade owns process-wide driver state. All instances in this library
// share its lifetime and memory registrations. Application-owned NDS sessions
// must be closed before installing this transport.
class NdsTransport : public Transport {
   public:
    NdsTransport();
    ~NdsTransport() override;
    Status install(std::string& local_segment_name,
                   std::shared_ptr<ControlService> metadata,
                   std::shared_ptr<Topology> local_topology,
                   std::shared_ptr<Config> conf = nullptr) override;
    Status uninstall() override;
    Status quiesce() override;
    Status allocateSubBatch(SubBatchRef& batch, size_t max_size) override;
    Status freeSubBatch(SubBatchRef& batch) override;
    Status submitTransferTasks(SubBatchRef batch,
                               const std::vector<Request>& requests) override;
    Status getTransferStatus(SubBatchRef batch, int task_id,
                             TransferStatus& status) override;
    Status addMemoryBuffer(BufferDesc& desc,
                           const MemoryOptions& options) override;
    Status removeMemoryBuffer(BufferDesc& desc) override;
    const char* getName() const override { return "nds"; }

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mooncake::tent
#endif  // TENT_NDS_TRANSPORT_H
