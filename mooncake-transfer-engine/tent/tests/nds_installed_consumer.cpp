// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <iostream>
#include "tent/transfer_engine.h"

static_assert(mooncake::tent::NDS == TRANSPORT_NDS);
static_assert(TRANSPORT_MEMFABRIC == 18 && TRANSPORT_NDS == 19);

int main(int argc, char**) {
    if (argc != 2) return 1;
    tent_set_config("metadata_type", "p2p");
    tent_set_config("rpc_server_hostname", "127.0.0.1");
    tent_set_config("rpc_server_port", "0");
    tent_set_config("transports/nds/enable", "false");
    auto engine = tent_create_engine();
    if (!engine) return 2;
    char name[1024]{};
    int rc = tent_segment_name(engine, name, sizeof(name));
    tent_destroy_engine(engine);
    if (rc || !std::strlen(name)) return 3;
    std::cout << "installed C++/C API consumer: PASS\n";
    return 0;
}
