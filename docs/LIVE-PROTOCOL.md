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
1–30000. The room remembers at most 64 unique action IDs and never re-executes a
duplicate. `expected_revision` is checked immediately before every stateful
action; `observe` and `cancel` are revision-independent so they can recover from
concurrent manual input.

| Action | Additional field | Trusted effect |
|---|---|---|
| `observe` | none | Return current state and projected capabilities. |
| `go_to` | `target_id` | Navigate toward one catalogued approach point. |
| `interact` | `target_id` | Interact only when that same target is nearby. |
| `face_user` | none | Face down toward the room viewer. |
| `say` | `text` | Accept up to 512 printable-ASCII characters for a bounded room notice and trusted speech handoff. |
| `cancel` | `cancel_action_id` | Cancel only the named in-flight action. |

No request schema contains a command, executable, path, URL, environment,
desktop-input, file, session-administration, shutdown, or power field.

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

## Provider boundary

Providers see only the current user request, the fixed tool schemas, and the
minimum live results required for the turn. They do not receive the host
environment, filesystem, credentials, arbitrary desktop input, or an execution
primitive. The deterministic provider is the conformance authority; the Qwen
adapter is an untrusted proposal source behind the same validator.
