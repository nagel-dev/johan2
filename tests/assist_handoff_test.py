#!/usr/bin/env python3
"""Exercise the actual Assist task's wake/cue/start branches on a host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/assist_client.cpp").read_text()


def block(marker: str, include_condition: bool = False) -> str:
    start = SOURCE.index(marker)
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start if include_condition else opening + 1:end if include_condition else end - 1]


def method(name: str) -> str:
    return block(f"void AssistClient::{name}(", True)


announcer = SOURCE.index("} else if (!protocol_.command_active &&")
announcer = SOURCE[announcer + len("} else if ("):SOURCE.index(") {", announcer)]

HARNESS = r'''
#include "assist_protocol.h"
#include "listening_cue.h"
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <vector>
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define CONFIG_JOHAN2_HA_PIPELINE ""
#define CONFIG_JOHAN2_HA_REPLY_PLAYER ""
constexpr bool kListeningCue = true, kDirectReplies = true;
static int64_t clock_us = 1000;
int64_t esp_timer_get_time() { return clock_us; }
uint32_t esp_random() { return 42; }
void xQueueReset(int) {}
struct Playback {
    int starts = 0;
    void cancel() {}
    bool start_listening_cue(bool ending) { assert(!ending); ++starts; return true; }
};
struct AssistClient {
    AssistProtocol protocol_;
    ListeningCueGate cue_;
    Playback tts_;
    std::atomic<bool> accepting_{true}, command_active_{false}, transport_failed_{false};
    int audio_queue_ = 0;
    bool wake_handoff_ = false, remote_start_pending_ = false, followup_ = false;
    bool followup_used_ = false, followup_speech_seen_ = false, direct_playing_ = false;
    bool speaker_probe_active_ = false;
    unsigned followup_count_ = 0;
    int64_t wake_at_ = 0, conversation_last_ = 0, wake_tail_until_ = 0;
    int64_t retry_at_ = 0, followup_deadline_ = 0, run_deadline_ = 0;
    static constexpr int64_t kWakeTailUs = 600000, kFollowupUs = 3000000;
    static constexpr int64_t kConversationIdleUs = 300000000;
    char conversation_id_[32] = "same-conversation";
    const char* assist_state_ = "idle";
    std::string local_announcement_token_, deferred_reply_;
    std::vector<std::string> sent;
    bool send_text(const std::string& s) { sent.push_back(s); return true; }
    void publish_state(const char* state) { assist_state_ = state; }
    void cancel_local_announcement() {}
    void reset_audio(bool preserve_end_cue = false);
    void request_listening_cue(bool ending = false);
    void cancel_command(bool stop_playback);
    void wake() { @WAKE@ }
    void listening() { @LISTENING@ }
    void schedule_cue() { const auto now = clock_us; @CUE@ }
    void launch() { const auto now = clock_us; const bool voice_allowed = true; @LAUNCH@ }
    bool notification_ready() { const auto now = clock_us; return @ANNOUNCER@; }
};
@METHODS@
void event(AssistClient& c, const char* kind, int handler = 7) {
    const auto s = std::string("{\"id\":") + std::to_string(c.protocol_.run_id) +
        ",\"type\":\"event\",\"event\":{\"type\":\"" + kind +
        "\",\"data\":{\"runner_data\":{\"stt_binary_handler_id\":" +
        std::to_string(handler) + "}}}}";
    c.protocol_.receive(s.c_str());
}
AssistClient* prepare(bool cue_enabled) {
    auto* c = new AssistClient;
    c->protocol_.authenticated = true;
    c->protocol_.listening_cue_enabled = cue_enabled;
    c->protocol_.begin("", true, "same-conversation");
    event(*c, "run-start");
    assert(c->protocol_.receive(R"({"id":1,"type":"event","event":{"type":"wake_word-end","data":{"wake_word_output":{"wake_word_id":"hey_jarvis"}}}})") == AssistProtocol::Event::Wake);
    c->wake();
    assert(c->wake_handoff_ && c->remote_start_pending_ && !c->accepting_);
    assert(!c->protocol_.running && !c->protocol_.streaming);
    JsonDocument d; deserializeJson(d, c->sent.back());
    assert(d["type"] == "unsubscribe_events" && d["subscription"] == 1);
    return c;
}
int main() {
    for (bool cue_enabled : {false, true}) {
        clock_us = 1000;
        auto* c = prepare(cue_enabled);
        // Pending notifications must neither steal nor deadlock command startup.
        c->local_announcement_token_ = "queued-notification";
        c->schedule_cue();
        assert(!c->notification_ready());
        c->launch();
        assert(c->protocol_.running && c->wake_handoff_ && !c->remote_start_pending_);
        assert(!c->notification_ready()); // Especially with cues disabled and STT not ready yet.
        assert(c->local_announcement_token_ == "queued-notification");
        JsonDocument d; deserializeJson(d, c->sent.back());
        assert(d["start_stage"] == "stt" && d["conversation_id"] == "same-conversation");
        assert(c->cue_.pending == cue_enabled); // launch must preserve the cue.
        c->schedule_cue(); // Also valid before the server's run-start/STT-start.
        assert(c->cue_.pending == cue_enabled && c->tts_.starts == 0);
        event(*c, "run-start", 9);
        event(*c, "stt-start");
        c->listening();
        assert(!c->wake_handoff_ && c->protocol_.active_handler() == 9);
        assert(c->wake_at_ == 1000 && c->conversation_last_ == 1000);
        assert(!c->notification_ready());
        clock_us = 601000;
        c->schedule_cue();
        assert(c->tts_.starts == int(cue_enabled));
        c->listening(); // Duplicate STT-start must not add a second beep.
        c->schedule_cue();
        assert(c->tts_.starts == int(cue_enabled) && !c->cue_.pending);
        c->cancel_command(false);
        assert(!c->wake_handoff_ && !c->protocol_.running && !c->remote_start_pending_);
        delete c;
        // Explicit cancellation while still between subscriptions must rearm idle.
        c = prepare(cue_enabled);
        c->cancel_command(false);
        assert(!c->cue_.pending && !c->wake_handoff_ && !c->remote_start_pending_);
        assert(std::string(c->assist_state_) == "idle");
        delete c;
    }
    puts("Assist handoff: fresh STT, one/no cue, early cancellation, queued notification and wake timing passed");
}
'''
HARNESS = HARNESS.replace("@WAKE@", block("} else if (result == AssistProtocol::Event::Wake) {"))
HARNESS = HARNESS.replace("@LISTENING@", block("} else if (result == AssistProtocol::Event::Listening) {"))
HARNESS = HARNESS.replace("@CUE@", block("if (cue_.pending && !cue_.playing) {", True))
HARNESS = HARNESS.replace("@LAUNCH@", block("if (voice_allowed &&", True))
HARNESS = HARNESS.replace("@ANNOUNCER@", announcer)
HARNESS = HARNESS.replace("@METHODS@", "\n".join(method(n) for n in ["reset_audio", "request_listening_cue", "cancel_command"]))
with tempfile.TemporaryDirectory(prefix="johan2-assist-handoff-") as temp:
    source = Path(temp) / "test.cpp"
    binary = Path(temp) / "test"
    source.write_text(HARNESS)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "managed_components/bblanchon__arduinojson/src"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
