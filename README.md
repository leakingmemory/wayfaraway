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

- **Headless backend + pixman renderer.** There is one virtual output per
  client monitor, sized and arranged like the client's monitors. Each frame is
  rendered on the CPU. Only the damaged region is copied into a shadow buffer
  covering the whole desktop and encoded with RemoteFX.
  NSCodec is the fallback for clients without RemoteFX, or with a multifragment
  size too small for it; damage is split into tiles that fit the client's PDU
  limit.
- **A classic desktop.** The look is a 16-colour-era desktop: teal background,
  grey 3D frames and navy title bars.
  - **Window frames:** apps that accept server-side decorations (foot, Qt,
    Firefox, Alacritty and others) get the retro frame. Apps that draw their
    own title bars, as most GTK apps do, keep theirs.
  - **Launcher:** the **Programs** window lists every application from the
    XDG `.desktop` files, sorted into groups by category (Accessories,
    Internet, Office and so on). One group is open as an inner window; the
    others are icons along the bottom. It reloads by itself when applications
    are installed or removed.
  - **Minimized windows:** these become captioned icons along the bottom of the
    desktop.
- **Multiple monitors.** With `/multimon`, each client monitor becomes its own
  Wayland output, so apps see real monitors (for example for fullscreen or
  `xdg-output`). The layout can change during a session through the Display
  Control channel when the client window is resized or monitors come and go.
  Outputs are reused across changes. New windows open on the monitor under the
  pointer, maximize fills the window's own monitor, and windows left off-screen
  move to the primary monitor. The lock screen is centred on the primary
  monitor.
- **Built-in lock screen.** The compositor draws the lock screen itself with
  cairo/pango; no client is involved. While the session is locked, the whole desktop
  scene tree is disabled and no client gets keyboard or pointer focus, so nothing
  can leak or race it. The password of the user running the compositor is checked
  through PAM on a worker thread, so the event loop never blocks. Caps Lock and
  Num Lock follow the client's state on connect and whenever its window regains
  focus, and changes on the desktop are mirrored back to the client's keyboard
  LEDs. The lock screen warns when Caps Lock is on.
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
   happens on the lock screen. On first start, the compositor generates a
   self-signed certificate in `~/.config/wayfaraway/` (`tls.crt`, plus `tls.key`
   with mode 0600). It is RSA-3072, valid for 10 years, and covers the hostname
   and localhost. Every start logs its SHA-256 fingerprint:
   ```
   TLS: certificate SHA-256 fingerprint 3A:F1:…
   ```
   Compare it with what your client shows the first time it asks whether to
   trust the server. To use your own certificate, pass `--cert`/`--key` or put
   the files in place. To get a fresh one, delete both files.
2. **PAM service.** `cmake --install` installs `/etc/pam.d/wayfaraway`. It
   includes `system-local-login`, or `system` on FreeBSD; pick another stack
   with `-DWAYFARAWAY_PAM_INCLUDE=...` (for example `system-auth`). Without the file, PAM falls back to the `other`
   service, which usually denies everything. It goes to `/etc/pam.d` whatever
   the install prefix, because that is the only place PAM looks. Override it
   with `-DWAYFARAWAY_PAM_DIR=...` if your distribution uses something else,
   such as `/usr/lib/pam.d`.
3. **Run it.** Start it as the user who owns the desktop:
   ```sh
   ./build/wayfaraway --startup 'foot'
   ```
   Or run it as a systemd user service. On Linux, `cmake --install` installs
   `wayfaraway.service` into the systemd user unit directory (turn this off
   with `-DWAYFARAWAY_SYSTEMD=OFF`). The directory comes from `systemd.pc`
   under the install prefix, or you can set it with
   `-DWAYFARAWAY_SYSTEMD_USER_UNIT_DIR=...`. Then, as the user:
   ```sh
   systemctl --user daemon-reload
   systemctl --user enable --now wayfaraway
   systemctl --user edit wayfaraway   # to add --startup, --address, ...
   loginctl enable-linger "$USER"     # keep it running without a login
   ```
   Without lingering, the service only runs while you have a local login or
   SSH session. To pass options, override `ExecStart` in the drop-in: an
   empty `ExecStart=` line, then the new one.

   On FreeBSD, `cmake --install` installs an rc.d script instead, as
   `${PREFIX}/etc/rc.d/wayfaraway` (`-DWAYFARAWAY_RC=OFF` turns it off,
   `-DWAYFARAWAY_RC_DIR=...` moves it). It runs the compositor as the given
   user under daemon(8), which restarts it if it crashes, and creates
   `/var/run/user/<uid>` for the Wayland socket:
   ```sh
   sysrc wayfaraway_enable=YES
   sysrc wayfaraway_user=alice
   sysrc wayfaraway_flags="--startup 'foot'"   # optional
   sysrc wayfaraway_env="LANG=en_US.UTF-8"     # optional
   service wayfaraway start
   ```
   Output goes to `/var/log/wayfaraway.log` (`wayfaraway_log`).

   On Gentoo and other OpenRC systems, `cmake --install` installs
   `/etc/init.d/wayfaraway` and, unless it already exists,
   `/etc/conf.d/wayfaraway`. This happens by default when `openrc-run` is
   found; `-DWAYFARAWAY_OPENRC=ON/OFF` overrides that. It runs under
   supervise-daemon, which restarts it if it crashes. Make one instance per
   user, named after them:
   ```sh
   ln -s wayfaraway /etc/init.d/wayfaraway.alice
   rc-update add wayfaraway.alice default
   rc-service wayfaraway.alice start
   ```
   Options go in `/etc/conf.d/wayfaraway.alice`, for example
   `WAYFARAWAY_OPTS="--startup 'foot'"`. Output goes to
   `/var/log/wayfaraway.alice.log`. The Wayland socket lives in
   `/run/wayfaraway/alice`. That directory is outside `/run/user/<uid>`,
   which elogind mounts over at login and removes at logout. To share the
   user's PipeWire there, set `WAYFARAWAY_RUNTIME_DIR=/run/user/<uid>` and
   run `loginctl enable-linger alice`.

   On Void Linux and other runit systems with `/etc/sv`, `cmake --install`
   installs the service `/etc/sv/wayfaraway`. This happens by default when
   `runsvdir` is found; `-DWAYFARAWAY_RUNIT=ON/OFF` overrides that. Its `conf`
   is only installed if it doesn't exist yet. Copy the service under a name
   ending in the user's name, then enable it:
   ```sh
   cp -a /etc/sv/wayfaraway /etc/sv/wayfaraway-alice
   ln -s /etc/sv/wayfaraway-alice /var/service/
   ```
   runit restarts it if it exits. Options go in
   `/etc/sv/wayfaraway-alice/conf` (`WAYFARAWAY_OPTS`), and svlogd keeps the
   log in `/var/log/wayfaraway-alice/`. As with OpenRC, the socket lives in
   `/run/wayfaraway/alice` unless `WAYFARAWAY_RUNTIME_DIR` says otherwise.

   On s6-rc systems such as Artix, `cmake --install` installs the source
   definitions `wayfaraway-srv` and `wayfaraway-log` into `/etc/s6/sv`. They
   form the pipeline bundle `wayfaraway`. This happens by default when
   `s6-rc-compile` is found; `-DWAYFARAWAY_S6=ON/OFF` overrides that.
   Settings go in `/etc/s6/config/wayfaraway.conf`, which is only installed
   if it doesn't exist yet and is read on every start:
   ```sh
   # set WAYFARAWAY_USER (and WAYFARAWAY_OPTS) in /etc/s6/config/wayfaraway.conf
   s6-service add default wayfaraway
   s6-db-reload
   s6-rc -u change wayfaraway
   ```
   Without Artix's helpers, add the bundle to your source tree, then run
   `s6-rc-compile` and `s6-rc-update`. s6 restarts the desktop if it exits,
   and s6-log keeps the log in `/var/log/wayfaraway/`. The socket lives in
   `/run/wayfaraway/<user>` unless `WAYFARAWAY_RUNTIME_DIR` says otherwise.

   On dinit systems such as Chimera Linux or Artix, `cmake --install`
   installs the service templates `wayfaraway@` and `wayfaraway-setup@` into
   `/etc/dinit.d`, plus their helper scripts in `${PREFIX}/libexec/wayfaraway`.
   This happens by default when `dinit` is found; `-DWAYFARAWAY_DINIT=ON/OFF`
   overrides that. Each user gets an instance named after them:
   ```sh
   dinitctl enable wayfaraway@alice
   ```
   `wayfaraway-setup@alice` creates the runtime directory as root, then
   `wayfaraway@alice` runs the desktop as alice and restarts it if it exits.
   Settings go in `/etc/dinit.d/config/wayfaraway.conf`, which is only
   installed if it doesn't exist yet. Settings for one user go in
   `wayfaraway-alice.conf` beside it. The log is
   `/var/log/wayfaraway-alice.log`, and the socket lives in
   `/run/wayfaraway/alice` unless `WAYFARAWAY_RUNTIME_DIR` says otherwise.

   By default it only listens on `127.0.0.1:3389`. From another machine, tunnel
   it over SSH:
   ```sh
   ssh -L 3389:localhost:3389 user@host
   ```
   Then connect any client that supports RemoteFX or NSCodec to `localhost`,
   for example `xfreerdp /v:localhost /rfx /multimon /dynamic-resolution /sound /microphone /cert:tofu`,
   Remmina or mstsc. Pass `--address 0.0.0.0` to accept connections from the
   network directly. Audio needs a
   running PipeWire user session and a client with sound redirection enabled.

## Keys

| Keys                  | Action                     |
|-----------------------|----------------------------|
| Alt+Return            | Launch terminal (`-t`)     |
| Super (Windows key)   | Open Programs, or go back to the window you came from |
| Ctrl+Esc              | Same as Super              |
| Alt+Tab               | Switch tasks: hold Alt, press Tab to move, release to switch |
| Alt+Shift+Tab         | Switch tasks, backwards    |
| Alt+F4                | Close window               |
| Ctrl+Alt+L            | Lock                       |
| Alt+drag / right-drag | Move / resize window       |

In the task switcher, Escape cancels and a click picks a task directly. It
lists every window in most-recently-used order, including minimized windows
(restored when chosen) and Programs. A quick Alt+Tab tap flips between the two
most recent windows.

The Windows key and Alt+Tab must get past your local desktop to reach the
session. With mstsc, set *Apply Windows key combinations* to *On the remote
computer*, or use full screen. xfreerdp passes them on while it has the
keyboard grabbed (in full screen, or with `+grab-keyboard`).

With the mouse:

| Action | Where |
|--------|-------|
| Move | Drag the title bar |
| Resize | Drag the border; the notched corners resize both ways |
| Maximize / restore | Double-click the title bar, or the ▲ button |
| Minimize | The ▼ button. Double-click the desktop icon to restore |
| Close | Double-click the control-menu box (top left) |
| Launch | Double-click a program in Programs, or select it and press Enter |
| Switch group | Double-click a group icon, or press Tab |

## Status / next steps

- The shell is deliberately minimal. There are no menus (so the control-menu
  box has no menu), no taskbar beyond Alt+Tab, and nothing to edit groups. Programs always
  sorts apps into category groups.

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
