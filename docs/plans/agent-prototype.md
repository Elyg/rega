# rega agent mode: prototype first

## Context
Over ssh+tmux, frames currently go through the pty as base64 PNG kitty escapes. tmux
buffers them with no backpressure, and that is the reason for LinkMeter (DA1 probes /
`ss` bytes_acked), autoTransfer, resend-on-attach and the Ghostty-side frame cache. The
idea: when rega runs on the remote, send the pixels over a separate ssh-forwarded
socket to a Mac-side `rega --agent`, which holds them in Mac RAM as shm. Ghostty
then reads them locally, and only tiny `t=s` escapes cross tmux. Running rega on the
Mac stays exactly as it is.

The user wants a **prototype first** to find out how cumbersome this gets, before
integrating it into the viewer. This plan is the prototype, plus the agreed design it
should validate.

## Agreed design (what the prototype validates)
1. UI stays remote, in tmux. Local runs are unchanged. With no agent, the path is
   v0.0.18's inline path, kept frozen (nothing removed).
2. Verified 2026-10-22: ldn allows unix-socket RemoteForward through the ControlMaster
   mux. sshd_config is unreadable (StreamLocalBindUnlink unknown), so socket paths are
   always unique.
3. The ssh session runs under the agent: `rega --agent -- ssh <args>` picks the Mac
   socket `$TMPDIR/rega-agent-<pid>.sock`, adds
   `-R /tmp/rega-$USER/<unique>.sock:<mac sock>` and `LC_REGA_AGENT=<remote path>`,
   runs ssh as its child, and exits when ssh does. Final form: `rega --shell-init bash`
   prints the `ssh()`/`ssht` helper (`ssht` runs `ghostty -e ssh`, which bypasses
   functions, so it needs the helper too).
4. The remote finds the socket through the **attached tmux client's** environment
   (/proc/<client pid>/environ), the mechanism `tmuxSshPeers()` uses in
   src/term/Caps.cpp:175. It prunes dead sockets (the ControlMaster keeps old forwards).
5. Wire data: display-ready RGBA8 rendered on the remote, as today. JPEG (stb) by
   default; `L` cycles q60/q85/q95/lossless, and `--quality` sets the start. Agent mode
   only.
6. Mac cache: **RAM only, never on disk** (the frames are sensitive). Each frame is a
   POSIX shm object, mmap'd and mlock'd by the agent. Ghostty unlinks a `t=s` name after
   reading, but the agent's mapping stays valid, so it re-arms
   `re<token>-<frame>-<gen+1>` (≤31 chars, macOS limit) and sends READY to the remote.
   The remote only emits names the agent has confirmed, so there is no race between the
   socket and pty channels, and a replay costs 0 RTT. The agent unlinks everything on
   exit or SIGTERM.
7. Agent mode needs every attached client to have an agent, and exactly one agent in
   v1. Otherwise: inline, with a status note.
8. Pacing reuses LinkMeter ACK mode, with agent READY replies standing in for DA1
   answers. Sends run on their own thread, and the newest playback frame wins.
9. v1 scope (after the prototype): core, `--shell-init bash|zsh`, `--doctor` agent
   checks, docs/setup.md. Not in v1: nested hops, several agents.

## Prototype: what to build
Branch `agent-prototype`, with hidden flags only. No viewer integration and no release.

- `src/agent/Protocol.{h,cpp}`: length-prefixed messages over a stream socket:
  HELLO{version, token} → FRAME{key, w, h, codec(jpeg|raw), bytes} →
  READY{key, shmName, size} / DROP{key} / BYE. Blocking read/write helpers.
- `src/agent/Agent.{h,cpp}` (Mac side, `rega --agent -- ssh …`):
  - creates the socket, forks/execs ssh with the extra `-R` and `-o SetEnv`/env args,
    and polls the socket and child;
  - per FRAME: decodes JPEG with stb_image (already compiled in
    src/image/StbLoader.cpp), then `shm_open` + `ftruncate` + `mmap` + `mlock`
    (adapted from `kitty::writeSharedMemory`, src/term/Kitty.cpp:405, but keeping the
    mapping), then READY;
  - a re-arm loop checks armed names with `shm_open(O_RDONLY)`; on ENOENT it creates
    gen+1 from the kept mapping and sends READY;
  - a RAM budget flag with LRU DROP; unlinks everything on SIGTERM, SIGINT and child exit.
- `src/agent/AgentDemo.cpp` (remote side, `rega --agent-demo <sequence>`):
  - detects caps (`detectCaps` for cell size and `tmuxLayers()`), finds the socket via
    a new `tmuxClientEnv(name)` helper in Caps.cpp (generalised from the /proc read in
    `tmuxSshPeers`; falls back to `$LC_REGA_AGENT` outside tmux);
  - per frame: `probeImage`/`loadLayer`/`renderLayer` at the terminal pixel size, then
    JPEG via stb_image_write (new implementation TU; the stb package already ships the
    header) or raw for lossless, then FRAME;
  - on READY: write `kitty::transmitShared(img, name, opt)` (src/term/Kitty.cpp:359)
    under one image id, with placeholder cells (`kitty::placeholderCell`) printed once;
  - loops the sequence; loop 2+ uses armed names only. Prints per-loop fps, bytes per
    frame, READY latency and re-arm latency; `--quality 60|85|95|lossless`.
- `src/main.cpp`: hidden `--agent` and `--agent-demo` flags, then dispatch.
- `CMakeLists.txt`: add the new sources.
- `tests/test_agent.cpp`: protocol round trip; re-arm (create, unlink externally, see
  gen+1 appear with the same bytes); cleanup on stop. Runs on macOS and Linux.

## Experiments (the answer to "how cumbersome")
| # | Question | Pass |
|---|---|---|
| E1 | Does Ghostty on the Mac show a `t=s` escape that came through remote tmux passthrough, naming shm created by *another* process (the agent)? | the picture shows |
| E2 | Does Ghostty unlink after reading, does the kept mapping survive, how fast is re-arm? | loop 2 sends 0 frame bytes, no blank frames |
| E3 | Does `rega --agent -- ssh -t ldn tmux new -A -s proto` forward through the mux, and is LC_REGA_AGENT readable from the client's /proc env, including after detach and reattach? | demo finds the socket both times |
| E4 | Throughput on the real link: first-loop fps at q60/q85/q95/raw vs v0.0.18 inline on the same sequence | clearly beats inline |
| E5 | Does mlock of a few GB succeed for a normal user on macOS? | succeeds, or a documented fallback |
| E6 | After ssh exits or SIGTERM: is all shm gone? (SIGKILL is expected to leak) | yes |
| E7 | What it cost: LoC, moving parts, every surprise hit | written up for the go/no-go call |

## Verification
- Mac: `just build && just test`.
- Linux binary: the docker nix static build (`nix build --impure .#static`, recipe in
  the ssh-playback memory), copied to ldn by the user.
- End to end, run by the user (never mtl): on the Mac,
  `rega --agent -- ssh -t ldn tmux new -A -s proto`. In the pane,
  `./rega --agent-demo <seq>.####.exr --quality 85`. Compare with
  `rega <seq>` inline + `S` stats. Detach, reattach and rerun for E3; quit ssh and
  check that no `re*` shm is left.
- Outcome: go/no-go on integrating (Transfer::AGENT in kitty::Transmitter, agent-backed
  kept frames in ImageSlot, discovery in App, the `L` key, doctor, shell-init).

## Built (2026-10-25)
Hidden flags only, on `agent-prototype`: `src/agent/{Protocol,Agent,AgentDemo}`,
`src/image/StbWriter` (stb_image_write JPEG), `tmuxClientEnv()` in Caps
(`tmuxSshPeers` now uses it), `tests/test_agent.cpp`.

Where it differs from the plan above:
- The remote socket is `/tmp/rega-agent-<user>-<random>.sock`, not in a
  `/tmp/rega-$USER/` directory: sshd won't create the directory for `-R`.
- `lossless` sends PNG (`kitty::encodePng`; the agent decodes it with stb like
  JPEG). `--quality raw` sends plain RGBA.
- There is one extra message, SHOWN{key}: the remote says it emitted a name, and
  the agent then checks that name every 2 ms until the terminal unlinks it. It
  checks only those names, not every frame it holds.
- ssh gets `-o SendEnv=LC_REGA_AGENT` as well as SetEnv, with the variable
  also set in its environment, in case a mux client doesn't pass SetEnv on.
  Both need `AcceptEnv LC_*` on the server.
- JPEG has no alpha, so letterbox and outside-data-window pixels show black.
- Frames live as long as the remote's connection, and a disconnect unlinks them.
  The agent decodes on its one poll thread.
- If a name isn't re-armed within 1 s, the demo emits it again and counts it as
  "stale" (that's E2's failure case: the terminal doesn't unlink).

Smoke-tested on the Mac with no ssh: a fake ssh hands the socket straight over,
and a Python pty plays the terminal, reading and unlinking every `t=s` name the
way Ghostty does. 48 frames 1600x900 over 3 loops: loop 1 ran at ~46-89 fps,
loop 2 at 24 fps with 0 late, re-arm took ~5 ms, and 0 shm were left after BYE
or SIGTERM.

Run (E1-E6), Mac side, then in the ldn pane:

    ./build/macos-arm64-Release/rega --agent -- ssh -t ldn tmux new -A -s proto
    ./rega --agent-demo 'shot.####.exr' --quality 85 [--loops 3] [--fps 24]

The demo prints one line per loop (fps, KB/frame, FRAME→READY, late/stale,
re-arm latency). The agent prints its summary when ssh exits (frames, decode
ms, peak MB held / not mlock'd = E5, shm left = E6) and logs to
`~/.cache/rega/rega.log` (`--agent-log` to change it). `--agent-budget 2G`
turns on LRU DROP.

## In the viewer (2026-10-25, for a trial)
Agent mode is wired into the viewer as a second home for kept frames. The
first home is the terminal cache: frames under kitty ids in Ghostty.
- **Remote (`rega` as usual):** while the transfer is direct (a client over
  ssh), `App` looks for an agent every 3 s: the clients' LC_REGA_AGENT, its
  own, then `/tmp/rega-agent-*.sock` of ours, newest first. ldn's sshd drops
  the variable. When one answers, `Transmitter::setAgent` turns
  `terminalCache()` into the agent's budget (`--agent-cache`, default 4G of Mac
  RAM). ImageSlot then sends each picture it would keep to the agent as JPEG
  (`agent::AgentClient::send`, paced by the socket), not under its own kitty id.
  It shows a kept picture by writing `t=s` with the name the agent armed, under
  the slot's one id. A picture not armed yet keeps the previous one on screen,
  and the agent's READY wakes a redraw. A name not read within 1 s is sent
  again (stale). An agent gone drops its frames, and they are sent again.
- **Fill:** the existing store-ahead (`Viewer::fillTerminal`) feeds the agent.
  There are no 255-id or 4 GB limits. `cappedBudget` doesn't fit frames to
  the inline link while an agent is in use.
- **Unchanged:** stills, paused frames, tiles, the playbar, and everything
  without an agent stay inline as before.
- **Keys:** `C` cycles q60 / q85 / q95 / lossless and sends every picture
  again. `S` shows the agent: socket, frames sent, last KiB, names shown,
  stale count, READY and re-arm times.
- **Flags (hidden):** `--no-agent`, `--agent-cache SIZE`, `--quality` for the
  start quality, and `--shell-init bash|zsh`, which prints an `ssh()` that runs
  an interactive ssh under `--agent` (`REGA_NO_AGENT=1` turns it off).
- **Known gaps:** a client attached without an agent (from the desk, say) next
  to one with an agent gets blank frames. Two live agents: the newest wins.
  No `--doctor` checks yet.
