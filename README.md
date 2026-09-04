# kilix-land-agent

Local-first resident-agent development project. Kilix the firekitten
lives in one studio apartment while the semantic control and safety boundaries
are proven before integration with the larger Kilix Land world.

The source of this repository is licensed under the MIT License; see
[`LICENSE`](LICENSE). Everything under `assets/` is licensed separately under
CC BY-NC-SA 4.0; see [`assets/LICENSE.md`](assets/LICENSE.md), which also
records each asset's provenance.

## Build

Clone recursively so the exact Kilix Game SDK revision and its modules are
present, then build and test:

```sh
git clone --recursive https://github.com/itsmygithubacct/kilix-land-agent.git
cd kilix-land-agent
make
make test
```

## Manual Studio mode

```sh
./kilix-land-agent --test-room
```

| Key | Action |
|---|---|
| Arrow keys or WASD | Walk |
| Enter or Space | Use the nearby object |
| Q, Escape, or Ctrl-C | Leave the room |

The room exposes stable IDs for a computer, desk, kitchenette, TV, radio,
bookshelf, bed, and plant. Seven targets are inspection-only placeholders. The
plant alone has an executable capability: it launches the fixed, allowlisted
`pleb-plant-grower` binary in a separate Kilix tab without a shell. The room
and chat remain alive in their original tab. Use Ctrl-C to exit the plant app
cleanly; closing or detaching its brokered tab alone can leave the game process
running. There is no arbitrary executable, file-editing, shutdown, or power
capability.

The handoff derives the matching `kitten` client from the owner of the current
Kilix socket rather than searching `PATH`. Before launch it verifies the
terminal process and pane identity, remote-control credential, executable
ownership and modes, link counts, parent directories, and the fixed plant
binary. It uses Kilix's same-user socket policy first and retries with the
credential only when the live engine requires password authorization.
Symlinks, group/world-writable paths, malformed sockets, and untrusted
credentials fail closed.

## Live resident mode

The deterministic provider proves the complete live path without an LLM:

```sh
./kilix-land-agentd --provider deterministic
```

For the qualified local test model, name an explicitly approved SSH host whose
Ollama service listens only on its own loopback interface:

```sh
./kilix-land-agentd \
  --provider qwen-ssh \
  --ssh-host <approved-host> \
  --model qwen3.5:9b \
  --request "Walk to the bookshelf, inspect it, face me, and tell me what you found."
```

For a persistent graphical session with Qwen controlling Kilix and a dialogue
composer at the bottom of the room, use:

```sh
./kilix-land-agentd \
  --provider qwen-ssh \
  --ssh-host <approved-host> \
  --model qwen3.5:9b \
  --chat
```

The chat composer is focused initially. Type printable text and press Enter to
send it; Backspace edits it. Tab switches between chat and manual mode. Manual
mode restores WASD/arrows and Enter/Space interaction, and Tab returns to chat.
Escape clears a non-empty draft or leaves when the draft is empty; Ctrl-C
always leaves. The panel shows each validated Qwen action while Kilix moves and
keeps the most recent user message and Kilix reply visible. Reply text wraps to
the available viewport. A layered transcript surface prevents room notices or
the moving character from overprinting long replies, while the composer stays
visibly translucent. Rendering scales through a bounded 1920x1080 for a
full-screen Kilix tab.

Both modes execute a real closed loop:

1. obtain the current revision and semantic target catalog;
2. accept exactly one typed provider proposal;
3. validate it against the current catalog and action sequence;
4. send it over the private room session;
5. wait for actual movement, collision, proximity, and interaction results;
6. return that result and new revision before asking for another proposal.

The final `say` action is displayed in the room and spoken through `kilix-tts`.
The default is the registered `piper-en-us-kristin-medium` model. The trusted
resident starts `kilix-voiced` on demand, passes speech over standard input,
and never lets a provider choose an executable, model path, URL, or shell text.
If graphical playback fails, the real reply remains visible and the status line
reports that audio is unavailable. Closing the room also terminates an in-flight
SSH model request instead of leaving a hidden resident behind. Use `--no-speech`
to mute playback or `--headless` for automated qualification.

The room and resident share a freshly created Unix `SOCK_SEQPACKET` socketpair.
Only its inherited descriptor crosses into the room process; there is no named
socket for another process to discover. The room verifies same-user peer
credentials and marks the descriptor close-on-exec. Requests carry session and
action IDs, an expected world revision, and a bounded timeout. Duplicate IDs,
stale revisions, extra fields, unknown targets, unavailable actions, oversized
messages, and expired work fail closed. Manual input cancels agent navigation.
See [docs/LIVE-PROTOCOL.md](docs/LIVE-PROTOCOL.md).

Qwen also receives two read-only tools for searching and reading bounded help
excerpts beneath the Kilix tree. The resident selects `~/gpu_terminal` in a
development checkout or `~/.local/gpu_terminal` in an installed session. Only
Markdown, text, reStructuredText, README, and HELP documents are indexed;
absolute paths, traversal, symlink escapes, binary data, oversized files, and
writes are rejected. Content is returned as untrusted reference data, never as
instructions.

The one-shot mode leaves the graphical room under manual control after its
reply. Chat mode serves repeated in-room turns and keeps only a short volatile
conversation window. Durable memory, the general application permission
broker, and remote GPT/MiniMax/Kimi adapters remain later milestones.

## Semantic observation

The initial state can be inspected without opening a terminal framebuffer:

```sh
./kilix-land-agent --observe
```

It emits `kilix.land.observe/v1` JSON containing the room revision, Kilix's
position, stable entity IDs, approach points, capability IDs, and the bounded
action catalog.

## Earlier replay and video fixtures

`tools/qwen_room_demo.py` retains the original preflight-validated visual replay
for regression comparison. It validates a complete three-tool model sequence
before opening the room, so it is not the live resident path above.

`tools/qwen_video_demo.py` retains the deterministic 30-second H.264/AAC media
fixture. Its model actions are validated before rendering and therefore carry
the same replay qualification.

## Tests

```sh
make test
```

The gates cover movement and collision, unique target IDs, fixed plant
capability selection, graphics hashes and atlas layout, deterministic rendering,
strict request parsing, private live transport, revision progression, duplicate
IDs, stale actions, cancellation, manual override, malformed and oversized
messages, unknown targets, exact provider tool shapes, unobserved-target
rejection, bounded speech, fixed `kilix-tts` invocation without a shell,
confined help ranges and paths, and fixed-tab launcher rejection of unsafe
links and file modes.

## Dependencies and assets

`third_party/kilix-game-sdk` is a recursively pinned submodule. Its existing
Kilix modules provide the terminal session, input, framebuffer, software
renderer, top-down runtime, assets, state primitive, and PCM support used by the
room. Build products remain under this repository's ignored `build/` directory.

Every asset under `assets/` is licensed CC BY-NC-SA 4.0, including
`graphics/casts/kilix-player.png`, the generated studio plate, and the checked
demo video. Their hashes and provenance are recorded beside them. Read
[assets/LICENSE.md](assets/LICENSE.md) before redistributing any asset — the
asset license is NonCommercial and ShareAlike, and does not follow the MIT
license that covers the source.

Research, threat models, prompts, qualification evidence, and implementation
records live outside this repository under
`~/research/gpu_terminal/kilix-apps/kilix-land-agent`. Repository operations
follow `~/research/github/README.md`.
