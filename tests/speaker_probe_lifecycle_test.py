"""Exercise actual probe scheduling, cancellation and playback branches."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
client = (root / "main/assist_client.cpp").read_text()
tts = (root / "main/tts_player.cpp").read_text()
diagnostics = (root / "main/mic_diagnostics.cpp").read_text()


def block(source, marker, offset=0, whole=True):
    first = source.index(marker, offset)
    opening = source.index("{", first)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[first if whole else opening + 1:end if whole else end - 1]


fixture = r'''
#include "assist_protocol.h"
#include "listening_cue.h"
#include "speaker_probe.h"
#include "mic_diagnostics.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define CONFIG_JOHAN2_MIC_DIAGNOSTICS 1
#define CONFIG_JOHAN2_MIC_AEC 1
using S = mic_diagnostics::SpeakerProbeState;
std::atomic<S> probe_state{S::Idle};
S mic_diagnostics::speaker_probe_state() { return probe_state.load(); }
void mic_diagnostics::set_speaker_probe_state(S s) { probe_state.store(s); }
@CLAIM@
struct Output {
    int writes = 0, ends = 0;
    int peak = 0;
    bool fail = false, drain_ok = true;
    std::atomic<bool>* cancel_after_write = nullptr;
    bool tts_begin() { return true; }
    bool tts_write(const int16_t* samples, size_t frames, const std::atomic<bool>&, int gain) {
        assert(gain == 100);
        for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, std::abs(int(samples[i])));
        ++writes;
        if (cancel_after_write) cancel_after_write->store(true);
        return !fail;
    }
    bool tts_drain(int) { return drain_ok; }
    void tts_end() { ++ends; }
    int volume = 6; bool muted = false;
    int player_volume() { return volume; }
    bool player_muted() { return muted; }
};
struct TtsPlayer {
    enum Result { None, Ok, Unsupported, Failed, CueDone, ProbeDone, ProbeFailed };
    Output output; Output* audio_ = &output;
    std::atomic<bool> cancel_{false};
    int16_t out_[1024]{};
    bool starts = true; int requests = 0; Result result = None;
    Result play_speaker_probe();
    bool start_speaker_probe() { ++requests; return starts; }
    Result poll() { auto r = result; result = None; return r; }
    void cancel() { cancel_.store(true); result = ProbeFailed; }
};
@PLAY@
struct AssistClient {
    AssistProtocol protocol_;
    Output output; Output* audio_ = &output;
    TtsPlayer tts_;
    ListeningCueGate cue_;
    bool speaker_probe_active_ = false, remote_start_pending_ = false, direct_playing_ = false;
    bool wake_handoff_ = false, followup_ = false;
    int64_t speaker_probe_until_ = 0, retry_at_ = 0, followup_deadline_ = 0;
    std::string local_announcement_token_, deferred_reply_;
    std::atomic<bool> transport_failed_{false}, command_active_{false};
    bool send_text(const std::string&) { return true; }
    void reset_audio() { command_active_.store(false); }
    void cancel_local_announcement() {}
    void publish_state(const char*) {}
    void cancel_command(bool stop_playback);
    void tick(int64_t now) { @REQUEST@ @POLL@ }
};
@CANCEL@
int main() {
    for (int mode = 0; mode < 4; ++mode) {
        TtsPlayer p;
        if (mode == 1) p.output.fail = true;
        if (mode == 2) p.output.drain_ok = false;
        if (mode == 3) p.output.cancel_after_write = &p.cancel_;
        assert(p.play_speaker_probe() == (mode ? TtsPlayer::ProbeFailed : TtsPlayer::ProbeDone));
        assert(p.output.ends == 1 && !p.cancel_);
        if (!mode) assert(p.output.peak > 0 && p.output.peak <= SpeakerProbePcm::kAmplitude + 1);
    }
    for (int mode = 0; mode < 7; ++mode) {
        AssistClient c; c.protocol_.authenticated = true;
        c.protocol_.begin("", true);
        if (mode == 1) c.protocol_.command_active = true;
        if (mode == 2) c.output.muted = true;
        if (mode == 3) c.output.volume = 0;
        if (mode == 4) c.tts_.starts = false;
        if (mode == 5) { c.output.muted = true; c.output.volume = 0; }
        if (mode == 6) c.output.volume = 100;
        probe_state.store(S::Requested); c.tick(1000);
        if (mode == 1 || mode == 4) { assert(!c.speaker_probe_active_); assert(probe_state == (mode == 4 ? S::Failed : S::Rejected)); continue; }
        assert(c.speaker_probe_active_ && c.command_active_ && probe_state == S::Playing);
        assert(!c.protocol_.running); // Wake subscription must be cancelled for the tones.
        c.tts_.result = TtsPlayer::ProbeDone; c.tick(9000000);
        assert(!c.speaker_probe_active_ && !c.command_active_ && probe_state == S::Done);
        assert(c.retry_at_ == 9300000);
    }
    AssistClient cancelled; cancelled.protocol_.authenticated = true;
    probe_state.store(S::Requested); cancelled.tick(1000);
    cancelled.cancel_command(true);
    cancelled.tick(2000);
    assert(!cancelled.speaker_probe_active_ && probe_state == S::Failed);
    probe_state.store(S::Idle); cancelled.tick(3000);
    assert(cancelled.tts_.requests == 1); // Expired HTTP request cannot start late.
    AssistClient timeout; timeout.protocol_.authenticated = true;
    probe_state.store(S::Requested); timeout.tick(1000); timeout.tick(13000000); timeout.tick(13000001);
    assert(!timeout.speaker_probe_active_ && probe_state == S::Failed);
    puts("Speaker probe lifecycle: fixed level, muted/zero/maximum player volume, busy rejection, cancellation and failures passed");
}
'''
request_marker = "if (mic_diagnostics::speaker_probe_state() == mic_diagnostics::SpeakerProbeState::Requested)"
request_at = client.index(request_marker)
fixture = fixture.replace("@REQUEST@", block(client, request_marker))
fixture = fixture.replace("@POLL@", block(client, "if (speaker_probe_active_) {", request_at))
fixture = fixture.replace("@PLAY@", block(tts, "TtsPlayer::Result TtsPlayer::play_speaker_probe()"))
fixture = fixture.replace("@CANCEL@", block(client, "void AssistClient::cancel_command("))
fixture = fixture.replace("@CLAIM@", block(diagnostics, "bool mic_diagnostics::claim_speaker_probe()"))
with tempfile.TemporaryDirectory(prefix="johan2-probe-lifecycle-") as temp:
    source = Path(temp) / "test.cpp"
    binary = Path(temp) / "test"
    source.write_text(fixture)
    subprocess.run(["c++", "-O2", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root / "main"), "-I", str(root / "managed_components/bblanchon__arduinojson/src"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
