#include "assist_protocol.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    assert(metric_age_ms(10000, 0) == -1);
    assert(metric_age_ms(10000, 11000) == -1);
    assert(metric_age_ms(10000, 9000) == 1);
    assert(metric_age_ms(INT64_MAX, 1) == INT32_MAX);
    assert(!is_pcm_delivery_gap(0, 1000000)); // First PCM after start isn't a gap.
    assert(!is_pcm_delivery_gap(1000000, 1499999));
    assert(is_pcm_delivery_gap(1000000, 1500000));
    assert(!is_pcm_delivery_gap(1000000, 900000));
    RuntimeSnapshot snapshot;
    assert(!strcmp(snapshot.status(), "disconnected"));
    snapshot.sendspin_connected = true;
    assert(!strcmp(snapshot.status(), "idle"));
    snapshot.stream_active = true;
    assert(!strcmp(snapshot.status(), "awaiting_pcm"));
    snapshot.pcm_age_ms = 999;
    assert(!strcmp(snapshot.status(), "streaming"));
    snapshot.pcm_age_ms = 1000;
    assert(!strcmp(snapshot.status(), "pcm_stalled"));
    snapshot.main_loop_age_ms = 2000;
    assert(!strcmp(snapshot.status(), "main_loop_stalled"));
    snapshot.stream_active = snapshot.sendspin_connected = false;
    assert(!strcmp(snapshot.status(), "main_loop_stalled")); // Cached connectivity can be stale.
    snapshot.boot_id = UINT32_MAX;
    snapshot.uptime_s = 42;
    snapshot.pcm_gaps = 7;
    snapshot.i2s_errors = 3;
    snapshot.wifi_disconnects = 2;
    AssistProtocol protocol;
    protocol.receive(R"({"type":"auth_ok"})");
    protocol.begin("", true);
    const auto run = protocol.run_id;
    JsonDocument doc;
    assert(!deserializeJson(doc, protocol.diagnostics_report(snapshot)));
    assert(doc["type"] == "call_service" && doc["domain"] == "script");
    assert(doc["service"] == "johan2_diagnostics_report");
    assert(doc["service_data"]["metrics"]["status"] == "main_loop_stalled");
    assert(doc["service_data"]["metrics"]["boot_id"].as<uint32_t>() == UINT32_MAX);
    assert(doc["service_data"]["metrics"]["uptime_s"] == 42);
    assert(doc["service_data"]["metrics"]["pcm_gaps"] == 7);
    assert(doc["service_data"]["metrics"]["i2s_errors"] == 3);
    assert(doc["service_data"]["metrics"]["wifi_disconnects"] == 2);
    assert(doc["service_data"]["metrics"]["pcm_age_ms"] == 1000);
    assert(doc["service_data"]["metrics"]["mic_queue_drops"] == 0);
    assert(doc["service_data"]["metrics"]["sendspin_connected"].is<bool>());
    assert(protocol.receive(R"({"id":2,"type":"result","success":false})") == AssistProtocol::Event::DiagnosticsError);
    assert(protocol.running && protocol.run_id == run && protocol.authenticated);
    protocol.reset();
    assert(protocol.receive(R"({"id":2,"type":"result","success":false})") == AssistProtocol::Event::None);
    std::cout << "Runtime metric classification and protocol tests passed\n";
}
