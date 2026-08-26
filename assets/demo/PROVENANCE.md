# Qwen/Kilix demo provenance

- Generated: 2026-08-25
- File: `qwen-kilix-studio-demo-30s.mp4`
- SHA-256:
  `0871c3b3b5458d4d022815ec54cf793b7f4458ceb827bec5b9c428765a48bf9b`
- Video: H.264, 1280×720, 30 fps, exactly 30.000 seconds
- Audio: AAC mono, 48 kHz
- Action model: Ollama `qwen3.5:9b`, Q4_K_M
- Speech engine: eSpeak NG 1.52.0, `en-us` voice
- Encoder: FFmpeg 7.1.5
- Generator: `tools/qwen_video_demo.py`

The model proposed this validated sequence:

1. `observe_room({})`
2. `go_to({"target_id":"computer"})`
3. `interact({"target_id":"computer"})`
4. `go_to({"target_id":"bookshelf"})`
5. `interact({"target_id":"bookshelf"})`
6. `face_user({})`
7. `say({"text":"Welcome to our cozy studio apartment!"})`

The visible animation uses the trusted room state, collision checks, semantic
target catalog, Kilix sprite atlas, and studio plate. Qwen output supplies only
validated semantic arguments and the bounded printable speech text. The local
TTS line begins at 25 seconds and is also displayed on screen.

This is a preflight-validated visual replay, not a closed-loop resident-agent
claim. Marked action-result fixtures elicited the complete model sequence before
the trusted renderer produced the video.

The video derives from the studio plate and Kilix atlas documented under
`assets/graphics/PROVENANCE.md`. It remains a private development asset; review
all source-asset, generated-image, font/rendering, TTS voice-data, encoder, and
model-output terms before any public distribution.
