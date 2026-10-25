#include "assist_protocol.h"
#include <cassert>
#include <iostream>

using Event = AssistProtocol::Event;

static std::string update(int id, const std::string& url, const char* section = "c") {
    JsonDocument doc;
    doc["id"] = id;
    doc["type"] = "event";
    if (std::string(section) == "a")
        doc["event"]["a"]["input_text.johan2_announcement_url"]["s"] = url;
    else
        doc["event"]["c"]["input_text.johan2_announcement_url"]["+"]["s"] = url;
    std::string message;
    serializeJson(doc, message);
    return message;
}

int main() {
    AssistProtocol p;
    p.receive(R"({"type":"auth_ok"})");
    JsonDocument command;
    assert(!deserializeJson(command, p.subscribe_announcements()));
    const int subscription = command["id"];
    assert(command["type"] == "subscribe_entities");
    assert(command["entity_ids"].size() == 1);
    assert(command["entity_ids"][0] == "input_text.johan2_announcement_url");
    const std::string url = "http://haos.home:8123/api/homedash_announcement/abc_123-DEF";
    assert(p.receive(update(subscription + 1, url).c_str()) == Event::None);
    assert(p.receive(update(subscription, url, "a").c_str()) == Event::None);
    assert(p.receive(update(subscription, url).c_str()) == Event::None);
    assert(p.local_announcement_url().empty()); // Reconnects must not replay a notification.
    assert(p.receive(update(subscription, "").c_str()) == Event::LocalAnnouncementStop);
    assert(p.receive(update(subscription, url).c_str()) == Event::LocalAnnouncement);
    assert(p.local_announcement_url() == url);
    assert(p.local_announcement_token() == "abc_123-DEF");
    assert(!p.owns_voice && !p.running && !p.music_muted); // Explicit targets do not need voice ownership.
    assert(p.receive(update(subscription, url).c_str()) == Event::None);

    p.begin("", true);
    const int run = p.run_id;
    const std::string listening = "{\"id\":" + std::to_string(run) +
        ",\"type\":\"event\",\"event\":{\"type\":\"stt-start\"}}";
    assert(p.receive(listening.c_str()) == Event::Listening);
    assert(p.command_active && p.music_muted);
    for (const char* state : {"queued", "playing", "done", "error"}) {
        assert(!deserializeJson(command, p.local_announcement_status("abc_123-DEF", state)));
        assert(command["type"] == "call_service");
        assert(command["domain"] == "input_text" && command["service"] == "set_value");
        assert(command["target"]["entity_id"] == "input_text.johan2_announcement_status");
        assert(command["service_data"]["value"] == std::string(state) + ":abc_123-DEF");
        assert(p.running && p.run_id == run && p.music_muted);
    }
    const int status_id = command["id"];
    const std::string failed = "{\"id\":" + std::to_string(status_id) +
        ",\"type\":\"result\",\"success\":false}";
    assert(p.receive(failed.c_str()) == Event::LocalAnnouncementError);
    assert(p.running && p.command_active && p.music_muted); // An acknowledgment failure is not an Assist failure.
    const std::string rejected = "{\"id\":" + std::to_string(subscription) +
        ",\"type\":\"result\",\"success\":false}";
    assert(p.receive(rejected.c_str()) == Event::LocalAnnouncementError);

    for (const std::string bad : {
        "file:///api/homedash_announcement/abc", "http://ha/api/tts_proxy/abc.mp3",
        "http://ha/api/homedash_announcement/", "http://ha/api/homedash_announcement/a/b",
        "http://ha/api/homedash_announcement/a?b", "http://ha/api/homedash_announcement/a#b",
        "http://ha/api/homedash_announcement/a b", "http://ha/api/homedash_announcement/a.b",
        "http://api/homedash_announcement/abc"
    }) {
        assert(p.receive(update(subscription, bad).c_str()) == Event::LocalAnnouncementError);
        assert(p.local_announcement_url().empty() && p.local_announcement_token().empty());
        assert(p.running && p.music_muted);
    }
    assert(p.receive(update(subscription, "http://ha/api/homedash_announcement/" + std::string(65, 'a')).c_str()) ==
           Event::LocalAnnouncementError);
    assert(p.receive(update(subscription, "http://" + std::string(200, 'a') +
                            "/api/homedash_announcement/" + std::string(64, 'b')).c_str()) ==
           Event::LocalAnnouncementError);
    const std::string https = "https://ha/api/homedash_announcement/" + std::string(64, 'z');
    assert(p.receive(update(subscription, https).c_str()) == Event::LocalAnnouncement);
    assert(p.local_announcement_token() == std::string(64, 'z'));
    for (const char* stopped : {"", "unknown", "unavailable"})
        assert(p.receive(update(subscription, stopped).c_str()) == Event::LocalAnnouncementStop);
    p.receive(update(subscription, url).c_str());
    const std::string removed = "{\"id\":" + std::to_string(subscription) +
        ",\"type\":\"event\",\"event\":{\"r\":[\"input_text.johan2_announcement_url\"]}}";
    assert(p.receive(removed.c_str()) == Event::LocalAnnouncementStop);
    assert(p.receive(update(subscription, url, "a").c_str()) == Event::None);
    p.reset();
    assert(p.local_announcement_url().empty() && p.local_announcement_token().empty());
    assert(p.receive(update(subscription, url).c_str()) == Event::None);
    p.receive(R"({"type":"auth_ok"})");
    deserializeJson(command, p.subscribe_announcements());
    const int reconnected = command["id"];
    assert(reconnected > status_id);
    assert(p.receive(update(subscription, url).c_str()) == Event::None);
    assert(p.receive(update(reconnected, url, "a").c_str()) == Event::None);
    assert(p.receive(update(reconnected, "http://ha/api/homedash_announcement/fresh").c_str()) == Event::LocalAnnouncement);
    std::cout << "Announcement protocol tests passed\n";
}
