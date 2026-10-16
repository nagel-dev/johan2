#pragma once
#include <atomic>
#include "sendspin/client.h"

class Network : public sendspin::SendspinNetworkProvider {
public:
    bool start();
    void poll();
    bool is_network_ready() override { return connected_.load(); }
private:
    static void event(void*, const char*, int32_t, void*);
    std::atomic<bool> connected_{false};
    std::atomic<bool> retry_{false};
    int64_t retry_at_{0};
    uint32_t retry_delay_ms_{1000};
};
