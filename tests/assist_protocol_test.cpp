#include "assist_protocol.h"
#include <cassert>
#include <iostream>

using Event = AssistProtocol::Event;
int main() {
    AssistProtocol p;
    assert(p.receive(R"({"type":"auth_required"})") == Event::Authenticate);
    assert(!p.authenticated && !p.streaming);
    assert(p.receive(R"({"type":"auth_ok"})") == Event::Ready);
    assert(p.authenticated);
    JsonDocument command;
    assert(!deserializeJson(command, p.begin("my-pipeline")));
    assert(command["id"] == 1 && command["pipeline"] == "my-pipeline");
    assert(command["start_stage"] == "wake_word" && command["end_stage"] == "intent");
    assert(command["input"]["sample_rate"] == 16000);
    assert(p.running && !p.streaming);
    p.receive(R"({"id":99,"type":"event","event":{"type":"run-start","data":{"runner_data":{"stt_binary_handler_id":8}}}})");
    assert(!p.streaming); // Stale/unrelated runs must not change the audio routing.
    p.receive(R"({"id":1,"type":"event","event":{"type":"run-start","data":{"runner_data":{"stt_binary_handler_id":7}}}})");
    assert(p.streaming && p.handler == 7);
    assert(p.receive(R"({"id":1,"type":"result","success":true})") == Event::None);
    assert(p.streaming);
    p.receive(R"({"id":1,"type":"event","event":{"type":"wake_word-end","data":{}}})");
    assert(!p.command_active); // Silence timeout is not a wake word.
    assert(p.receive(R"({"id":1,"type":"event","event":{"type":"wake_word-end","data":{"wake_word_output":{"wake_word_id":"hey_jarvis"}}}})") == Event::Wake);
    assert(p.command_active);
    assert(p.receive(R"({"id":1,"type":"event","event":{"type":"stt-end","data":{"stt_output":{"text":"turn on the light"}}}})") == Event::Transcript);
    assert(p.transcript() == "turn on the light");
    p.receive(R"({"id":1,"type":"event","event":{"type":"stt-end"}})");
    assert(!p.streaming && p.command_active); // Keep music ducked until intent completes.
    assert(p.receive(R"({"id":1,"type":"event","event":{"type":"run-end"}})") == Event::Finished);
    assert(!p.running && !p.command_active);
    deserializeJson(command, p.begin(""));
    assert(command["id"] == 2 && command["pipeline"].isNull());
    p.receive(R"({"id":1,"type":"event","event":{"type":"error"}})");
    assert(p.running); // Late previous-run errors cannot cancel a new run.
    assert(p.receive(R"({"id":2,"type":"result","success":false})") == Event::Error);
    assert(!p.running);
    p.begin("");
    assert(p.receive(R"({"id":3,"type":"event","event":{"type":"run-start","data":{"runner_data":{"stt_binary_handler_id":256}}}})") == Event::Error);
    assert(!p.running && !p.streaming);
    p.begin("");
    assert(p.receive("not json") == Event::Error && !p.running);
    p.begin("");
    assert(p.receive(R"({"type":"auth_invalid"})") == Event::AuthInvalid);
    assert(!p.authenticated && !p.running && !p.streaming && !p.command_active);
    p.receive(R"({"type":"auth_ok"})");
    p.begin("");
    assert(p.receive(R"({"id":6,"type":"event","event":{"type":"error","data":{"code":"wake-word-timeout"}}})") == Event::Finished);
    assert(!p.running && !p.streaming);
    // The TTS answer must become one announcement, using a distinct, monotonic ID.
    AssistProtocol reply;
    reply.receive(R"({"type":"auth_ok"})");
    deserializeJson(command, reply.begin("", true));
    assert(command["end_stage"] == "tts");
    assert(reply.receive(R"({"id":99,"type":"event","event":{"type":"tts-end","data":{"tts_output":{"url":"/wrong.wav"}}}})") == Event::None);
    assert(reply.receive(R"({"id":1,"type":"event","event":{"type":"tts-end","data":{"tts_output":{"media_id":"media-source://tts/answer","url":"/api/tts_proxy/answer.wav","mime_type":"audio/wav"}}}})") == Event::SpeechReady);
    assert(!reply.command_active && !reply.streaming);
    assert(reply.announcement("light.wrong", "ws://haos.home:8123/api/websocket").empty());
    deserializeJson(command, reply.announcement("media_player.test", "ws://haos.home:8123/api/websocket"));
    assert(command["id"] == 2 && command["type"] == "call_service");
    assert(command["target"]["entity_id"] == "media_player.test");
    assert(command["service_data"]["media_content_id"] == "media-source://tts/answer");
    assert(command["service_data"]["announce"] == true);
    assert(command["service_data"]["extra"]["use_pre_announce"] == false);
    assert(reply.announcement_pending);
    assert(reply.announcement("media_player.test", "ws://haos.home:8123/api/websocket").empty());
    assert(reply.receive(R"({"id":1,"type":"event","event":{"type":"tts-end"}})") == Event::None);
    reply.receive(R"({"id":1,"type":"event","event":{"type":"run-end"}})");
    assert(!reply.running && reply.announcement_pending);
    assert(reply.receive(R"({"id":2,"type":"result","success":true})") == Event::PlaybackAccepted);
    assert(!reply.announcement_pending);
    deserializeJson(command, reply.begin("", true));
    assert(command["id"] == 3);
    reply.receive(R"({"id":3,"type":"event","event":{"type":"tts-end","data":{"tts_output":{"url":"/api/tts_proxy/a.wav"}}}})");
    deserializeJson(command, reply.announcement("media_player.test", "wss://haos.home:8123/api/websocket"));
    assert(command["id"] == 4);
    assert(command["service_data"]["media_content_id"] == "https://haos.home:8123/api/tts_proxy/a.wav");
    assert(reply.receive(R"({"id":4,"type":"result","success":false})") == Event::PlaybackFailed);
    assert(!reply.announcement_pending);
    reply.reset();
    assert(!reply.command_active && !reply.announcement_pending && !reply.streaming);
    reply.begin("", true);
    reply.receive(R"({"id":5,"type":"event","event":{"type":"tts-end","data":{"tts_output":{"url":"file:///tmp/a.wav"}}}})");
    assert(reply.announcement("media_player.test", "ws://haos.home:8123/api/websocket").empty());
    assert(!reply.announcement_pending);
    std::cout << "Assist protocol tests passed\n";
}
