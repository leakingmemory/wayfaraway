# wayfaraway

A headless [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) Wayland
compositor that you use over RDP. It runs as a long-lived per-user daemon, so the
desktop and its applications stay alive between RDP sessions. Every session starts
on a lock screen.

## How it works

```
 RDP client ──TLS──▶ FreeRDP listener ──input──▶ virtual wlr_keyboard / cursor ──▶ seat ──▶ clients
                          ▲
                          └── RemoteFX / NSCodec ◀── shadow framebuffer ◀── pixman render of the scene
```

- **Headless backend + pixman renderer.** There is one virtual output. Its size
  follows the RDP client's desktop size. Each frame is rendered on the CPU. Only
  the damaged region is copied into a shadow buffer and encoded with RemoteFX.
  NSCodec is the fallback for clients without RemoteFX, or with a multifragment
  size too small for it; damage is split into tiles that fit the client's PDU
  limit.
- **Built-in lock screen.** The compositor draws the lock screen itself with
  cairo/pango; no client is involved. While the session is locked, the whole desktop
  scene tree is disabled and no client gets keyboard or pointer focus, so nothing
  can leak or race it. The password of the user running the compositor is checked
  through PAM on a worker thread, so the event loop never blocks. Caps Lock and
  Num Lock follow the client's state on connect and whenever its window regains
  focus. The lock screen warns when Caps Lock is on.
- **Persistence.** The compositor doesn't exit when an RDP client disconnects.
  It locks, and applications keep running. The next connection, from any client,
  lands on the lock screen. A new connection while one is active takes over the
  session and locks it again.
- **Clipboard.** Plain text is shared both ways over the cliprdr channel, with
  line endings converted. Text copied on the desktop is cached, so it can still
  be pasted on the client after the app that copied it exits. Nothing crosses
  while the session is locked. A copy made on the client at the lock screen
  takes effect on unlock.
- **Audio.** The compositor creates a PipeWire sink, "Remote desktop (RDP)",
  with a high session priority, so it usually becomes the default output. What
  apps play to it is sent to the client over rdpsnd as 44.1 kHz stereo PCM, and
  silence is not sent. The sink outlives RDP connections, so audio routing
  survives reconnects. Nothing is played to a client at the lock screen.
- **Microphone.** A matching PipeWire source, "Remote desktop microphone", lets
  apps record from the client's microphone over audin, using 44.1 kHz PCM, with
  mono upmixed to stereo. The client's microphone is opened only while an app
  is actually recording *and* the session is unlocked, and closed otherwise, so
  nobody at the lock screen is ever recorded.

## Dependencies

wlroots 0.20, wayland-server, xkbcommon, pixman, cairo, pango, FreeRDP 3
(`freerdp3`, `freerdp-server3`, `winpr3`) and PAM. PipeWire (`libpipewire-0.3`) is optional and enables audio and the
microphone. Turn it
off with `-DWAYFARAWAY_AUDIO=OFF`. You need a C++23 compiler
(GCC 14+ or Clang 18+) and CMake 3.28 or newer.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

## Setup

1. **TLS certificate.** RDP is TLS-only. NLA is off because authentication
   happens on the lock screen.
   ```sh
   mkdir -p ~/.config/wayfaraway
   openssl req -x509 -newkey rsa:3072 -nodes -days 3650 -subj /CN=wayfaraway \
       -keyout ~/.config/wayfaraway/tls.key -out ~/.config/wayfaraway/tls.crt
   ```
2. **PAM service.** Install `data/wayfaraway.pam` as `/etc/pam.d/wayfaraway`
   (`cmake --install` does this). It includes `system-local-login`; adjust that
   for your distribution. Without the file, PAM falls back to the `other`
   service, which usually denies everything.
3. **Run it.** Start it as the user who owns the desktop:
   ```sh
   ./build/wayfaraway --startup 'foot'
   ```
   You can also use the systemd user unit in `data/wayfaraway.service`.
   By default it only listens on `127.0.0.1:3389`. From another machine, tunnel
   it over SSH:
   ```sh
   ssh -L 3389:localhost:3389 user@host
   ```
   Then connect any client that supports RemoteFX or NSCodec to `localhost`,
   for example `xfreerdp /v:localhost /rfx /sound /microphone /cert:ignore`,
   Remmina or mstsc. Pass `--address 0.0.0.0` to accept connections from the
   network directly. Audio needs a
   running PipeWire user session and a client with sound redirection enabled.

## Keys

| Keys                  | Action                     |
|-----------------------|----------------------------|
| Alt+Return            | Launch terminal (`-t`)     |
| Alt+Tab               | Cycle windows              |
| Alt+F4                | Close window               |
| Ctrl+Alt+L            | Lock                       |
| Alt+drag / right-drag | Move / resize window       |

## Status / next steps

- Clients that support neither RemoteFX nor NSCodec are refused. Planar/raw
  bitmap fallback and GFX/H.264 are still to come.
- Backpressure relies on RDP frame acknowledgement (at most 4 unacknowledged
  frames; damage is coalesced meanwhile). Clients without it are not throttled
  yet; socket write-blocking could cover them.
- The clipboard is text-only: images, HTML, files and the primary selection
  aren't bridged.
- Audio and microphone use plain PCM at a fixed 44.1 kHz. Clients without that
  format get no sound or microphone. Compressed formats such as AAC/Opus, and
  throttling for slow links, are still to come.
- There's no rate limiting of unlock attempts beyond PAM's own fail delay.
- Window state doesn't survive a compositor restart. Adding
  `xdg-session-management-v1` would let clients restore their windows.
