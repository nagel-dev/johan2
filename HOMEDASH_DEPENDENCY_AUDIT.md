# HomeDash dependency investigation — 2026-10-25

Johan2's current configuration does not meet a strict requirement that HomeDash
must never gate or delay Johan2. Music has no HomeDash relay, but voice ownership
and shared HA announcement routing introduce real coupling. This investigation
does not change firmware, configuration, or deployed services.

## Scope and evidence

Inspected this working tree, selected non-secret settings in local `sdkconfig`
and `build/config/sdkconfig.h`, and the sibling HomeDash HA packages, announcement
component and proxy. Both local configurations enable `JOHAN2_HOMEDASH_SYNC`,
transcript/response posting and the LED mirror. A build configuration is not proof
of the exact firmware currently running on the device.

Authenticated, read-only HA GET requests at approximately 21:38 CET on October 25
showed `input_select.voice_device = johan2`, Johan2 voice online, and
`input_boolean.homedash_voice_active = off`. Johan2's heartbeat was fresh;
HomeDash's heartbeat was hours old. The ownership automation was enabled. This
demonstrates that a fresh HomeDash heartbeat is not required for Johan2 to own
voice. It does not establish why an earlier interaction was slow. The deployed
automation's full source was not retrieved; timing rules below describe the
sibling repository's current source.

## Confirmed control coupling

- `main/assist_protocol.h:11,47,124,279`: ownership starts false, resets false on
  reconnect and follows HA's `input_select.voice_device`.
- `main/assist_client.cpp:371,375,646`: losing ownership cancels voice work;
  remote starts are ignored without ownership; automatic wake listening cannot
  start until ownership is granted when synchronization is enabled. Missing
  ownership helpers or a failed control subscription can therefore leave voice
  disabled even with HA authentication working.
- `../homedash/ha/packages/voice_failover.yaml:50`: recovery waits for ten seconds
  of stable Johan2 online state, checks on a five-second cycle and adds a
  two-second microphone handover. A HomeDash conversation with a fresh heartbeat
  blocks handback. Its heartbeat expires after twenty seconds, allowing recovery
  from a crashed HomeDash. These are control delays, not per-command HTTP delays.
- `../homedash/ha/packages/johan2_assist.yaml:52`: shared remote start/cancel
  scripts wait up to ten seconds for ownership and then select a device. Johan2's
  branch presses its HA helper directly; it does not relay through HomeDash.

## Announcement coupling

`../homedash/ha/packages/announce.yaml:34` uses a shared queued `script.announce`.
The default target follows voice ownership and can therefore send a notification
to HomeDash instead of Johan2. Both devices share quiet-hours helpers. Because
the script is queued, an earlier HomeDash-targeted invocation can delay a later
Johan2-targeted invocation while the earlier TTS/service work completes. This is
a source-level possibility; no queue-delay measurement was made.

Johan2 automatic announcements require the HA-hosted `homedash_media_player`
component and accept only `/api/homedash_announcement/<token>` URLs
(`main/assist_protocol.h:240`). The name is misleading: the component registers
the redirect endpoint inside HA and each player has its own announcement lock
and helpers. Johan2 audio does not pass through the HomeDash display or its
external proxy. Removing this HA component would nevertheless break Johan2's
automatic announcements. Assist replies use HA TTS directly or the configured
Music Assistant player.

## Background traffic and resource defects

`main/assist_client.cpp:228` creates a new 4096-byte-stack task for each transcript
and response. HTTP runs in that task with a configured 3000 ms timeout; the
Assist loop does not await the result. The HomeDash proxy enqueues display data
without waiting for the physical display (`hd_proxy_server.py:1509`). There is
no display acknowledgment on Johan2's reply path.

At investigation time, `post_homedash_text` called `vTaskDelete(nullptr)` while its local
`std::unique_ptr<HomeDashPost>` and `std::string url` are still in scope. FreeRTOS
self-deletion does not unwind C++ automatic objects: the post allocation, its
heap-backed strings and the URL allocation are not released through their
destructors. ESP-IDF 6.0.1's `components/freertos/FreeRTOS-Kernel/tasks.c`
(`vTaskDelete` and `prvDeleteTCB`) releases scheduler/task storage rather than
performing C++ stack unwinding. This is a concrete leak in the optional HomeDash
posting path, including successful posts. The task stack itself is reclaimed.
Its accumulated effect on this device has not been measured.

Follow-up fix: move the owning HTTP/post scope into `perform_homedash_post`,
which returns normally before the task wrapper calls `vTaskDelete(nullptr)`.
This releases the post and string allocations on successful requests, HTTP
failures, client initialization failures and malformed URL early returns.
An allocation-counting host harness passed 40,000 lifecycles with address and
undefined-behaviour sanitizers; restoring the old lifetime ordering fails its
pre-deletion cleanup assertion. `./scripts/idf.sh build` passed with ESP-IDF
6.0.1. Subsequently flashed at the user's request with bootloader, partition
table and application write hashes verified, followed by the flasher's reset.
Physical heap stability and playback remain unverified.
The per-post task concurrency concern below remains separate from this fix.

There is also no fixed worker/queue or global in-flight limit for these posts.
Slow/unreachable proxy requests can temporarily retain multiple task stacks,
sockets and payloads. Background execution prevents a direct three-second wait
in the Assist loop, but does not eliminate shared heap, CPU or Wi-Fi contention.

`main/audio_output.cpp:209` sends LED colours over UDP from a separate priority-2
task, polling every 20 ms and sending up to roughly 40 times/second plus idle
refreshes. DNS is also in that task. There is no receiver acknowledgment or audio
path wait, but the traffic still consumes shared network resources. Its impact
was not measured.

## Independent paths

`main/main.cpp`, `main/network.cpp`, `CMakeLists.txt` and
`main/idf_component.yml` show Wi-Fi startup, Sendspin discovery/connection and DAC
playback using Johan2's own configuration and managed dependencies. No runtime
HomeDash service is required in that music transport. The Wi-Fi import helper is
an optional build-time convenience; the firmware does not read the sibling
repository at runtime. Shared HA/MA/network load can still affect either device;
this investigation does not measure that contention.

## Changes needed to satisfy strict independence

1. Make Johan2 voice availability independent of the shared HomeDash owner;
   adapt HomeDash fallback rules so Johan2 does not wait for a HomeDash reply.
   Preserve explicit cancellation and intentional microphone handover behavior.
2. Keep Johan2 HA remote controls and reports functional independently of voice
   arbitration. Simply disabling `JOHAN2_HOMEDASH_SYNC` also disables those
   controls/reports, so that switch alone is not a complete fix.
3. Cleanup before worker deletion is now fixed in source. Use one bounded,
   low-priority observer queue with drop/coalesce behavior, or disable optional
   posting and LED mirroring when strict independence is required.
4. Give Johan2 its own announcement routing/queue and independently installable
   HA bridge. Retain compatibility with existing URLs and callers during any
   migration.

The existing Assist and announcement protocol host tests passed with
`-Wall -Wextra -Werror -fsanitize=address,undefined`. The Assist tests confirm
ownership grant/revocation/removal/reset behavior. They do not execute FreeRTOS
posting tasks or measure timing, playback quality, heap depletion or operation
with HomeDash powered off. No new firmware build or physical test was performed
because this task changed documentation only.
