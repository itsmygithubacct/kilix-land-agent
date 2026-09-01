# Live Studio protocol

`kilix.land.action/v1` is the private control contract between the trusted
resident and one authoritative Studio runtime. Model output never reaches this
contract directly: the resident validates one proposal and constructs the
request envelope itself.

## Transport

The resident creates an unnamed Unix `SOCK_SEQPACKET` socketpair and passes one
descriptor to the room with `--agent-session-fd FD`. The room accepts only a
sequenced-packet socket owned by the current user, applies `FD_CLOEXEC`, limits
each packet to 4095 bytes, and binds the first valid `session_id` for the life of
the connection. Closing the resident end closes the room session.

Each packet is one printable-ASCII JSON object. Duplicate or unknown fields,
unsupported escapes, trailing data, control characters, and values outside
their fixed bounds are rejected.

## Action request

Every request contains:

```json
{
  "protocol": "kilix.land.action/v1",
  "session_id": "session-token",
  "action_id": "unique-action-token",
  "action": "go_to",
  "expected_revision": 7,
  "timeout_ms": 15000,
  "target_id": "bookshelf"
}
```

`session_id` and `action_id` are 1–32 character ASCII tokens. `timeout_ms` is
1–30000. The room remembers at most 4096 unique action IDs and never re-executes
a duplicate. `expected_revision` is checked immediately before every stateful
action; `observe` and `cancel` are revision-independent so they can recover from
concurrent manual input.

| Action | Additional field | Trusted effect |
|---|---|---|
| `observe` | none | Return current state and projected capabilities. |
| `go_to` | `target_id` | Navigate toward one catalogued approach point. |
| `interact` | `target_id` | Interact only when that same target is nearby. The plant returns `launch_requested`, not an unverified claim that its app opened. |
| `face_user` | none | Face down toward the room viewer. |
| `say` | `text` | Accept up to 512 printable-ASCII characters for a bounded room notice and trusted speech handoff. |
| `cancel` | `cancel_action_id` | Cancel only the named in-flight action. |

No request schema contains a command, executable, path, URL, environment,
desktop-input, file, session-administration, shutdown, or power field.

The trusted resident may additionally send `status` with bounded text to update
the chat panel while it executes resident-side help tools. `status` is absent
from the provider tool schema and the room's advertised action catalog; it has
no world effect and does not advance the revision.

## Results and revisions

`observe` returns `kilix.land.observe/v1`. It contains the matching session and
action IDs, the authoritative revision, player state, stable entity catalog,
projected capability IDs, and currently available actions.

Other actions return `kilix.land.action-result/v1`:

```json
{
  "protocol": "kilix.land.action-result/v1",
  "session_id": "session-token",
  "action_id": "unique-action-token",
  "action": "go_to",
  "status": "ok",
  "revision": 42,
  "result": "arrived",
  "target_id": "bookshelf"
}
```

Rejected, canceled, and timed-out work carries a bounded machine-readable
`code`. Movement and interactions advance the revision in the room, not in the
provider adapter. The resident returns the complete authoritative result to the
provider before accepting another proposal.

Only one navigation action may be pending. A user movement key or interaction
cancels it with `manual_override`. A matching `cancel` produces a terminal
result for the original action and an acknowledgement for the cancel request.
Timeouts use the room's monotonic clock.

The plant request is resolved by the trusted graphical host after interaction.
It opens the one fixed `pleb-plant-grower` executable in a separate tab, so the
resident transport and room remain live. The launcher uses no shell and no
model-supplied path or argument. It derives the matching `kitten` binary from
the PID encoded in the current Kilix socket, validates that process, the
inherited pane identity, and the private remote-control credential. It first
uses the engine's same-user socket authorization and retries with the credential
on password-only engines. Both requests have the same fixed app argv. Symlinks,
hard links, unsafe ownership or modes, writable parent directories, and
malformed socket names are rejected. The UI reports the actual launcher
outcome; a model may claim only that the request was accepted.

Ctrl-C is the reliable plant-app exit. Kilix's persistent PTY broker can detach
a closed tab without immediately terminating its foreground game, so the UI
and user documentation do not present close-tab alone as process cleanup.

## In-room chat input

Graphical chat mode uses the same private sequenced-packet transport but a
separate message type flowing from the trusted room UI to the resident:

```json
{
  "protocol": "kilix.land.chat-input/v1",
  "session_id": "session-token",
  "message_id": "chat-00000001",
  "text": "Please inspect the radio."
}
```

The room cannot submit chat until the action session is bound. It accepts one
1–256 character printable-ASCII message, marks the composer busy, and does not
send another until a validated `say` completes. Exact fields and matching
session IDs are required. Chat input is user intent for the provider; it is not
parsed as an action request and never bypasses the resident validator.

Tab changes between composer and manual-control modes. Manual movement retains
priority and cancels pending navigation. Conversation context is bounded and
memory-only for the life of the resident process.

While Qwen is generating, the resident peeks at the room channel for closure
without consuming packets. Closing the room terminates the SSH transport within
one polling interval. A TTS failure after a successful `say` does not discard or
replace the displayed reply; chat reports local audio unavailable and remains
ready for the next message.

## Provider boundary

Providers see only the bounded conversation, fixed tool schemas, and minimum
live results required for the turn. They do not receive the host environment,
credentials, arbitrary desktop input, or an execution primitive. Qwen has two
additional resident-side tools: `search_help` and `read_help`. Those tools can
return bounded excerpts only from indexed help documents below the detected
development or installed Kilix root. They reject absolute and non-normalized
paths, traversal, symlinked files/directories, binary content, oversized files,
and every write operation. Reads walk the indexed path through no-follow
directory descriptors so a post-index symlink swap cannot escape the root.
Search does not retain the whole help tree in memory, and direct reads use an
eight-document least-recently-used cache. The deterministic provider remains
the conformance authority; the Qwen adapter is an untrusted proposal source
behind the same validator.

The resident performs an authoritative observation at the start of every user
turn and supplies that result before requesting Qwen's first proposal. Safety
therefore does not depend on the model remembering to call `observe_room`; Qwen
may call it again when it needs to refresh state.

After Qwen successfully proposes `face_user`, the Ollama adapter requests one
plain-text final response without exposing tool schemas. The resident flattens,
bounds, and printable-ASCII sanitizes that untrusted text, constructs the
trusted `say` action itself, and then hands the same text to TTS. This avoids a
known Qwen/Ollama XML tool-serialization failure without allowing prose to
become an action or command.
