# kilix-land-agent

Private, local-first resident-agent development project. Kilix the firekitten
lives in one studio apartment while the semantic control and safety boundaries
are proven before integration with the larger Kilix Land world.

This repository is private. It grants no public project or generated-asset
license, and it must pass the same publication-hygiene gates as a public Kilix
repository.

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
`pleb-plant-grower` binary without a shell. There is no arbitrary executable,
file-editing, shutdown, or power capability.

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
Use `--no-speech` to mute playback or `--headless` for automated qualification.

The room and resident share a freshly created Unix `SOCK_SEQPACKET` socketpair.
Only its inherited descriptor crosses into the room process; there is no named
socket for another process to discover. The room verifies same-user peer
credentials and marks the descriptor close-on-exec. Requests carry session and
action IDs, an expected world revision, and a bounded timeout. Duplicate IDs,
stale revisions, extra fields, unknown targets, unavailable actions, oversized
messages, and expired work fail closed. Manual input cancels agent navigation.
See [docs/LIVE-PROTOCOL.md](docs/LIVE-PROTOCOL.md).

The current `kilix-land-agentd` implements one user-initiated turn and then
leaves the graphical room under manual control until it is closed. Persistent
chat, memory, sandboxed application artifacts, and remote GPT/MiniMax/Kimi
adapters remain later milestones.

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
rejection, bounded speech, and fixed `kilix-tts` invocation without a shell.

## Dependencies and assets

`third_party/kilix-game-sdk` is a recursively pinned submodule. Its existing
Kilix modules provide the terminal session, input, framebuffer, software
renderer, top-down runtime, assets, state primitive, and PCM support used by the
room. Build products remain under this repository's ignored `build/` directory.

`assets/graphics/casts/kilix-player.png` remains CC BY-NC-SA 4.0. The generated
studio plate and the checked demo video remain private development assets with
their hashes and provenance recorded beside them. Read
[assets/LICENSE.md](assets/LICENSE.md) before redistributing any asset.

Research, threat models, prompts, qualification evidence, and implementation
records live outside this repository under
`~/research/gpu_terminal/kilix-apps/kilix-land-agent`. Repository operations
follow `~/research/github/README.md`.
