# HomeDash freeze — 2026-10-25

## Observed failure

The user reported that HomeDash stopped updating its Johan2 LED mirror, displayed
"No master", and no longer responded to touch. It stayed on the Music screen;
no reboot was observed. "No master" is the volume widget's label for owner zero
(`../homedash/main/ui/volume_control.c:59`), not an exception or crash diagnostic.

Read-only HA requests showed `input_select.voice_device = johan2`, Johan2 online
with a fresh heartbeat, and `media_player.johan2_aux = playing`, while the display
was stale. The HomeDash proxy container was still running. Its logs show the
previous device connection timing out at 20:43:53 UTC (21:43:53 CET). No proxy
service restart was performed in this investigation.

## Pre-reset firmware evidence

Passive USB captures, made without sending commands or manipulating DTR/RTS,
show repeated task-watchdog warnings at firmware tick 416230:

```text
task_wdt: - IDLE (CPU 0)
task_wdt: CPU 0: tcpip
MEPC: 0x42100eb2 / 0x42100eb4
RA:   0x42102896
SP:   0x4086dfb0
```

Decoding these addresses with the existing HomeDash ELF and RISC-V addr2line:

- Both PCs resolve to `vListInsert`, FreeRTOS `list.c:183`, the list insertion
  traversal loop. Disassembly confirms the repeated PCs are inside that loop.
- The return address resolves to `vTaskPlaceOnEventList`, `tasks.c:3768`.
- These are ESP-IDF 6.0.1 FreeRTOS sources, not the LED draw callback or a normal
  wait for a proxy response. The loop and watchdog explain CPU starvation and
  the unresponsive UI; they do not identify where the list was damaged or
  misused. Stack overflow, memory corruption and invalid synchronization-object
  use remain hypotheses, not established causes.

The existing HomeDash binary SHA-256 matches its recorded latest deployment:
`8062544f30c5ddd790ef77afe46292ffeedeee45eba6011ec37616b9e46f923e`.
The local configuration enables watchdog logging but disables watchdog panic,
so the observed task-watchdog warning does not itself request a reboot. Frame
pointers are not enabled, and the watchdog reports that a full backtrace is
unavailable.

Filtered pre-reset registers are retained in ignored
`build/homedash-freeze-registers-20261025.log`. No raw credential or voice-payload
capture was retained.

## Debugger side effect and recovery

Attached OpenOCD to HomeDash's specific ESP32-C6 USB serial number
`58:E6:C5:D4:75:B8`, intending to inspect the frozen task without resetting.
OpenOCD initially halted at `PC=0x42100eb6`, also inside the captured list loop.
Its default `gdb-attach` handler then detected memory protection and automatically
ran `reset halt`. This reset was not requested by the diagnostic command and was
reported to the user. Post-reset GDB task backtraces do not describe the original
freeze and must not be used as root-cause evidence.

Subsequently resumed the halted target explicitly and shut down OpenOCD. The
device reconnected to the proxy at 20:49:01 UTC (21:49:01 CET), received its
initial states and authenticated to HA. The user confirmed touch works again.
No firmware was flashed, no HA configuration changed, and no HomeDash source
patch was made.

## Remaining work

The original freeze is confirmed and recovery is confirmed; its source defect
is not fixed. A subsequent diagnostic build should capture complete task stacks
and detect list/heap corruption closer to its origin, while respecting the C6's
limited internal RAM. Repeated watchdog PCs are insufficient grounds to patch
FreeRTOS or the LED renderer. For future live debugger captures, override the
default reset-on-attach handler and disable flash probing before attaching; the
default handler is unsuitable for preserving this protected target's state.
