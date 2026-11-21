# wayfaraway

A headless [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) Wayland
compositor that you use over RDP. It runs as a long-lived per-user daemon, so the
desktop and its applications stay alive between RDP sessions. Every session starts
on a lock screen.

## How it works

```
 RDP client ──TLS──▶ FreeRDP listener ──input──▶ virtual wlr_keyboard / cursor ──▶ seat ──▶ clients
                          ▲
                          └── RemoteFX ◀── shadow framebuffer ◀── pixman render of the scene
```

- **Headless backend + pixman renderer.** There is one virtual output. Its size
  follows the RDP client's desktop size. Each frame is rendered on the CPU. Only
  the damaged region is copied into a shadow buffer and encoded with RemoteFX.
- **Built-in lock screen.** The compositor draws the lock screen itself with
  cairo/pango; no client is involved. While the session is locked, the whole desktop
  scene tree is disabled and no client gets keyboard or pointer focus, so nothing
  can leak or race it. The password of the user running the compositor is checked
  through PAM on a worker thread, so the event loop never blocks.
- **Persistence.** The compositor doesn't exit when an RDP client disconnects.
  It locks, and applications keep running. The next connection, from any client,
  lands on the lock screen. A new connection while one is active takes over the
  session and locks it again.

## Dependencies

wlroots 0.20, wayland-server, xkbcommon, pixman, cairo, pango, FreeRDP 3
(`freerdp3`, `freerdp-server3`, `winpr3`) and PAM. You need a C++23 compiler
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
   Connect with any RemoteFX-capable client, for example
   `xfreerdp /v:host /rfx /cert:ignore`, Remmina or mstsc.

## Keys

| Keys                  | Action                     |
|-----------------------|----------------------------|
| Alt+Return            | Launch terminal (`-t`)     |
| Alt+Tab               | Cycle windows              |
| Alt+F4                | Close window               |
| Ctrl+Alt+L            | Lock                       |
| Alt+drag / right-drag | Move / resize window       |

## Status / next steps

- Only RemoteFX is supported. Clients without it are refused. NSCodec/planar
  fallback and GFX/H.264 are still to come.
- There's no output backpressure. Frames are sent as fast as they are rendered.
  Frame acknowledgement should throttle this.
- Clipboard (cliprdr), audio (rdpsnd) and lock-key synchronisation aren't
  implemented yet.
- There's no rate limiting of unlock attempts beyond PAM's own fail delay.
- Window state doesn't survive a compositor restart. Adding
  `xdg-session-management-v1` would let clients restore their windows.
