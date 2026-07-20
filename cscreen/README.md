# cscreen

A tiny, dependency-free screen sharing relay.  One C file, one HTML page,
no libraries, no certificates, no accounts, no NAT punch-through.

Everyone connects *outward* to a relay you control, so it works fine when
both ends sit behind NAT firewalls.

## Build

```
gcc -o cscreen -lpthread cscreen.c
```

That is the whole build.  The only dependency beyond the C standard
library is pthreads.  The code is C89 and compiles clean under
`-std=c89 -pedantic -Wall -Wextra`.

## Run

```
cscreen 5050 5051 [security string]
```

* `5050` -- HTTP port.  Serves the browser client.  Plain HTTP, no TLS.
* `5051` -- relay port.  The browser connects back here over WebSocket to
  carry the video.
* the third argument is optional; see *Keeping strangers out* below.

Then open `http://<server>:5050` in Firefox or Chrome.  On a remote box,
open the two ports in the firewall and use the server's hostname.

Testing locally:

```
./cscreen 5050 5051
firefox http://localhost:5050
```

## Using it

* **Share My Screen** -- pick a window, a tab, or the whole screen from the
  browser's own picker.  The **fps** dropdown next to it sets the capture
  rate (60/30/15/10/5).
* **Stop Sharing** -- stop.
* The header shows how many people are connected and whether you are
  currently sharing.
* **Fullscreen** blows the video up.

Only one person shares at a time.  If someone else hits *Share My Screen*,
whoever was sharing is stopped automatically to make room.  Viewers can
come and go mid-stream and will pick the picture up within a couple of
seconds.

Video only, no audio.

## Keeping strangers out

Give a third argument and the relay hides behind a secret URL:

```
cscreen 5050 5051 "some passphrase"
```

```
cscreen ready
  open   http://localhost:5050/589AF9BCD9   in a browser
  (clients without that code are refused)
```

The passphrase itself is never sent anywhere and never appears in a URL.
It is hashed down to a ten hex digit access code, and that code must be
the URL path on *both* ports -- the page and the video stream.  Anything
else gets a bare "Security code incorrect." page, so a bot scanning port
5050 finds nothing to look at.  The code is matched case insensitively,
and a trailing slash or query string is fine.

The code is a pure function of the passphrase, so it is the same every
time.  A cron job can keep the relay alive with the same passphrase and
your collaborators' bookmark keeps working across restarts.

This is obscurity, not encryption: it is plain HTTP, so anyone sitting on
the wire can read the URL and the video.  It keeps out passers-by, not a
determined eavesdropper.

## How it works

The relay understands nothing about video.  It shuttles opaque binary
blocks from the one designated sharer to every viewer, plus a handful of
tiny text control messages:

| Direction | Message | Meaning |
| --- | --- | --- |
| client to relay | `HELLO` | I just connected |
| client to relay | `SHARE <mime>` | I want the floor, encoding as `<mime>` |
| client to relay | `START` | my next binary block begins a new stream |
| client to relay | `STOP` | I am done sharing |
| relay to client | `COUNT <n>` | `n` people connected |
| relay to client | `RESTART` | sharer: cut a fresh stream, please |
| relay to client | `RESET <mime>` | viewer: rebuild your decoder, new stream incoming |
| relay to client | `STOPPED` | you lost the floor to someone else |
| relay to client | `IDLE` | nobody is sharing |

The browser captures with `getDisplayMedia`, encodes with `MediaRecorder`
(WebM/VP8 by default, the most universally decodable option), and viewers
decode through Media Source Extensions.

**Late joiners.**  A viewer cannot splice into the middle of a WebM
cluster, so instead of trying, the relay asks the sharer to cut a brand
new stream whenever someone joins or falls out of sync.  Everyone gets a
fresh init segment and a clean keyframe.  That costs a sub-second hiccup
on a join, and in exchange joining is completely reliable.

## Staying up

The whole point of this program is that the picture does not stop.

* Every connection retries forever, roughly twice a second, whether it is
  the sharer or a viewer.
* The sharer keeps its capture handle across reconnects, so a dropped
  connection never re-prompts you to pick your screen again.
* Killing and restarting the relay itself recovers with no user
  interaction at all.
* A viewer whose picture freezes while data keeps arriving is detected and
  nudged back into playing; if it stays stuck it tears down and rejoins.
  This matters more than it sounds: a paused `<video>` keeps buffering, so
  the stream looks perfectly healthy while the picture is frozen.
* Recovery is driven by *arriving data*, not by timers, because browsers
  throttle `setInterval` to about once a minute in background tabs.
* Viewers actively chase the live edge rather than drifting behind.
* If a viewer's link cannot keep up, the relay drops that viewer's backlog
  and resyncs it instead of stalling everyone else.
* Dead connections are reaped by a WebSocket ping heartbeat.

## Limits

* 64 simultaneous connections (`MAX_CONNS`).
* Plain HTTP and unencrypted WebSocket.  Without a security string, anyone
  who can reach the relay port can watch; with one, anyone who learns the
  URL can.  Put it behind a VPN or a firewall rule if that matters, or run
  it on a host only you and your collaborators can reach.
