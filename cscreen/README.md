# cscreen

A tiny, near-dependency-free screen sharing relay.  One C file, one HTML
page, no accounts, no NAT punch-through.

Everyone connects *outward* to a relay you control, so it works fine when
both ends sit behind NAT firewalls.

## Build

```
gcc -o cscreen -lpthread cscreen.c
```

That is the whole build for local use.  The only dependency beyond the C
standard library is pthreads.  The code is C89 and compiles clean under
`-std=c89 -pedantic -Wall -Wextra`.

**For a public relay you also need HTTPS** (see *HTTPS* below), which adds
OpenSSL and one compile flag:

```
gcc -DUSE_TLS -o cscreen -lpthread -lssl -lcrypto cscreen.c
```

The TLS code is compiled in only with `-DUSE_TLS`; the plain build above
stays exactly as it was, no OpenSSL required.

## Run

```
cscreen HTTP_PORT RELAY_PORT [security string] [fullchain.pem privkey.pem]
```

* `5050` -- HTTP(S) port.  Serves the browser client.
* `5051` -- relay port.  The browser connects back here over WebSocket to
  carry the video.
* the optional security string hides the relay behind a secret URL; see
  *Keeping strangers out*.
* the optional cert/key pair turns on HTTPS; see *HTTPS*.

The extra arguments are positional and unambiguous by count, because the
security string is exactly one argument and a cert/key is exactly two:

```
cscreen 5050 5051                                  no gate, no TLS
cscreen 5050 5051 secretword                       access code
cscreen 5050 5051 fullchain.pem privkey.pem        HTTPS
cscreen 5050 5051 secretword fullchain.pem privkey.pem
```

Then open `http(s)://<server>:5050` in Firefox or Chrome.  On a remote
box, open the two ports in the firewall and use the server's hostname.

Testing locally over plain HTTP works because browsers make an exception
for `localhost`:

```
./cscreen 5050 5051
firefox http://localhost:5050
```

## HTTPS

Browsers only allow screen capture (`getDisplayMedia`) in a *secure
context*: an `https://` page, or plain `http://` **to localhost only**.
So the moment the relay lives on a real machine instead of your own,
plain HTTP loads the page fine but *Share My Screen* fails with "This
browser cannot capture the screen."  The fix is to serve over HTTPS.

Build with `-DUSE_TLS` and pass a certificate chain and private key, in
PEM form -- exactly the files `certbot` writes:

```
cscreen 5050 5051 \
  /etc/letsencrypt/live/YOURHOST/fullchain.pem \
  /etc/letsencrypt/live/YOURHOST/privkey.pem
```

Both ports get TLS: the page is served over HTTPS, and the WebSocket is
`wss://`.  (An HTTPS page is forbidden from opening a plain `ws://`
socket, so both have to be secure.)  The client picks `wss` vs `ws`
automatically from the page's own scheme, so there is nothing to
configure on the browser side.

Reach the relay by the hostname the certificate was issued for, not by an
IP or `localhost`, or the browser will reject the certificate.  A
self-signed certificate works for testing if you accept it in the browser
(or pass `--ignore-certificate-errors` to Chromium).

TLS is the *only* thing OpenSSL is used for; everything else, including
the SHA-1 and base64 for the WebSocket handshake, is still from scratch.

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

On plain HTTP this is obscurity, not encryption: anyone sitting on the
wire can read the URL and the video, so it keeps out passers-by, not a
determined eavesdropper.  Run it over HTTPS (above) and the URL and the
stream are both encrypted in transit, and the secret code stays secret on
the wire.  The two combine well: HTTPS to encrypt, the code to gate.

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
* Without `-DUSE_TLS` and a certificate it is plain HTTP and unencrypted
  WebSocket, which the browser only allows to capture the screen on
  `localhost`.  For any remote use, build with TLS.
* cscreen does not obtain or renew certificates; point it at the PEM files
  `certbot` (or your CA) already manages, and restart it when they renew.
* Without a security string anyone who can reach the relay port can watch;
  with one, anyone who learns the URL can.  Combine HTTPS with a security
  string, put it behind a VPN or firewall rule, or run it on a host only
  you and your collaborators can reach.
