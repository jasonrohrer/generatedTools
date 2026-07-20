# CLAUDE.md — cscreen

Orientation for a fresh session. Read this first, then skim `userNotes.txt`
for the current to-do list. **Keep this file current**: when you change the
architecture, add an invariant, or learn something non-obvious, update the
relevant section here in the same commit.

## What this is

cscreen is a clean-room, dependency-free **screen sharing relay** for two or
more people who are on the phone together and need to see each other's
screens. It replaces Zoom (slow to start), Screego (viewer kept dropping),
and RustDesk (Mac-to-Linux sharing never showed a picture).

Both ends sit behind NAT, so there is **no peer-to-peer and no hole
punching**: everyone connects outward to a relay on a host you control.

The prime directive is **the picture does not stop**. Everything retries
forever with short timeouts. A reconnect must never re-prompt the sharer to
pick their screen again.

## Hard constraints (do not violate)

- `cscreen.c` is **pure C89**, compiles with **zero warnings** under
  `gcc -std=c89 -pedantic -Wall -Wextra`. Declarations at the top of a
  block; `/* */` comments only; no `//`, no C99 mid-block decls, no VLAs.
- **The only linked dependency is `-lpthread`.** It must build with exactly
  `gcc -o cscreen -lpthread cscreen.c`. No libraries, ever. That means no
  OpenSSL (hence no HTTPS), so the SHA-1 and base64 needed for the
  WebSocket handshake are implemented from scratch in this file.
- **TCP only.** No UDP, no WebRTC in the relay.
- The browser client is plain HTML/JS embedded in `cscreen.c` as the
  `gPage[]` array of lines, and must work in **both Firefox and Chrome**.
- **The relay must know nothing about video** — no codecs, no keyframes, no
  parsing. It forwards opaque blocks. Keep it that way.

## Layout

Everything is in `cscreen.c`:

| Section | What |
| --- | --- |
| SHA-1 / base64 | the WebSocket handshake, and the access code |
| access code | `derive_code`, `request_path`, `path_authorized` |
| `Conn` + globals | connection table, guarded by `gLock` |
| `conn_queue` / `ws_queue_frame` | per-connection outbound queue |
| relay logic | `request_restart`, `relay_media`, `handle_text` |
| `writer_thread` / `reader_thread` | two threads per connection |
| `heartbeat_thread` | ping, reap dead conns, expire stuck restarts |
| `gPage[]` | the entire browser client |
| listeners | HTTP accept loop, relay accept loop |

`@@PORT@@` in `gPage[]` is substituted at serve time with the real relay
port, so the client always dials the right port.

## Access code

An optional third command line argument is a passphrase. It is SHA-1'd and
the first five bytes become a ten hex digit code (`gCode`), which must then
be the URL path on **both** ports. The passphrase never leaves the server.

- **Gate both ports.** Gating only HTTP would leave the video stream itself
  open to anyone who guessed the relay port, which defeats the purpose.
- The client carries `location.pathname` straight over into the WebSocket
  URL, so there is nothing to substitute into `gPage[]` and no way for the
  two to disagree.
- Matching is case insensitive and tolerates a trailing slash or query
  string, because a human retypes this off a phone call.
- The code must be **deterministic** across runs -- a cron job restarting
  the relay must not invalidate everyone's bookmark. Never salt it with
  time, pid, or randomness.
- No third argument means no gate at all, which is the old behavior.

## Protocol

Text control messages, plus opaque binary media blocks. See README.md for
the table. The one thing to internalize:

**A viewer cannot splice into the middle of a WebM cluster.** So rather
than trying, whenever someone joins or falls out of sync the relay sends
`RESTART` to the sharer, which tears down its `MediaRecorder` and starts a
new one. The relay then broadcasts `RESET <mime>` to everyone and the fresh
init segment flows to all. Joins cost everyone a sub-second hiccup and in
exchange they are completely reliable.

`gAwaitStart` is the debounce: it is set when `RESTART` goes out and
cleared when `START` comes back. While it is set, further joins do not
request another restart, because the pending one already covers them. The
heartbeat expires it after 5s so a wedged sharer cannot block joins forever.

## Invariants

- **Lock order is always `gLock` then `conn->lock`.** Never the reverse.
  The writer thread takes only `conn->lock`.
- A `Conn` slot's mutex/cond are destroyed **while `gLock` is held, before
  `used` goes to 0** — otherwise the accept loop can claim the slot and
  re-init them underneath the destroy.
- Media is only forwarded to connections with `inSync` set, which happens
  on `RESET`. A newly joined viewer has `inSync == 0` and receives nothing
  until the next generation starts.
- Control frames are queued with `droppable == 0` so they survive
  backpressure; media is droppable.

## Non-obvious things learned the hard way

- **A paused `<video>` keeps buffering.** The stream looks perfectly
  healthy — data arriving, buffer growing — while the picture is frozen.
  A watchdog that only checks buffer growth will never fire. Check that
  `currentTime` actually advances. This was a real bug found in testing.
- **Background tabs throttle `setInterval` to about once a minute.** Any
  recovery that depends on a timer is dead in a background tab. Recovery is
  therefore driven from `pump()`, i.e. from arriving data, with the timer
  only as a backstop.
- Never assume `autoplay` took; call `play()` explicitly and swallow the
  promise rejection (`try/catch` does **not** catch it).
- Dropping queued media blocks to catch up splices corruption into the
  stream. Always prefer a clean resync over skipping blocks.
- Chunks are sent through a single `sendQ` promise chain, because
  `Blob.arrayBuffer()` resolution order is not otherwise guaranteed and
  out-of-order media blocks corrupt the stream.
- Seeking flushes the decoder, so `chase()` rate-limits its seeks to one
  per second. Seeking on every arriving block can by itself prevent
  playback from ever making progress.

## Tried and rejected (do not re-attempt without new evidence)

**Caching the generation's first block in the relay to re-prime one viewer
alone**, so that a join or a hiccup would not restart the stream for
everybody. It looks obviously right — the relay stays codec-blind, it just
replays "block #1" — and it does not work. The primed viewer receives the
opening block followed by *mid-cluster* live blocks, MSE raises a
`sourcebuffer error`, the viewer resyncs, gets primed again, and flaps in a
tight reconnect loop (measured: five reconnects in three seconds). A
`NEEDKEY` escalation does not save it either, because the sourcebuffer
error fires long before any escalation timer.

This is the whole reason the global-restart design exists. One viewer
joining costs everyone a sub-second hiccup, and that is the correct trade:
joins are rare, and the restart is the only thing that reliably gets a
decoder started. If you want to revisit this, you need a way to know where
a keyframe boundary is — which means parsing the container, which breaks
the "relay knows nothing about video" constraint.

## Testing

There is no automated suite; test against real browsers. Xvfb works:

```
setsid nohup Xvfb :99 -screen 0 1280x800x24 &        # plain & gets reaped
DISPLAY=:99 chromium --no-sandbox --disable-gpu \
  --user-data-dir=/tmp/p1 --remote-debugging-port=9222 \
  --auto-select-desktop-capture-source='Entire screen' \
  http://localhost:5050/
DISPLAY=:99 scrot -o shot.png
```

`--auto-select-desktop-capture-source` bypasses the picker dialog, which
otherwise blocks the renderer and hangs any DevTools eval. Drive the pages
over the DevTools Protocol and read real numbers back (`vid.videoWidth`,
`vid.currentTime`, `vid.paused`, `getVideoPlaybackQuality()`) rather than
judging by screenshot alone. If the sharer captures the whole screen while
a viewer is on the same display you get an infinite hall-of-mirrors, which
is a nice quick confirmation that live video is really flowing.

**Two browser windows on one Xvfb display stack on top of each other, and
Chromium reports a fully occluded window as `document.hidden`.** A hidden
tab never opens its `MediaSource` -- `ms.readyState` stays `closed` and the
`<video>` sits at `readyState 0` -- while blocks pile up in `queue`. That
looks exactly like a broken viewer and is not one; `Page.bringToFront` on
the viewer makes it open and drain within a second. Check `document.hidden`
before chasing a playback bug in a headless test.

**Shell gotcha:** `pkill -f "cscreen 5050 5051"` matches the command line of
your own shell and kills it (you will see exit code 144). Use `pkill -x
cscreen`, or a bracket pattern like `pkill -f "soa[k].py"`.

## Style

Match the surrounding code: banner-comment section headers, the
`( spaced parens )` call style, generous comments explaining the *why*.
Keep the zero-warning C89 bar. Prefer editing in place over rewrites.
