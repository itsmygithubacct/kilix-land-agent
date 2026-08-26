# Graphics provenance

## Studio apartment

- Generated: 2026-08-25
- Generator: OpenAI built-in image generation
- Source: `rooms/kilix/studio-apartment-source.png`
- Source dimensions: 1672×941 RGB PNG
- Source SHA-256:
  `92fb35a9f18432edc6dcc9698bd12002c6050df0b0f109772dbb648c37a9f3bd`
- Runtime plate: `rooms/kilix/studio-apartment.png`
- Runtime dimensions: 1280×720 RGB PNG
- Runtime SHA-256:
  `ed64787649958eee21bfa7593c93f0176db42e552bb7ecb3c524ad7b8fc66367`
- Cook: center-fit and Lanczos resize through
  `kilix-land-desktop/tools/prepare_plate.py`
- Prompt and reference-role record:
  `~/research/gpu_terminal/kilix-apps/kilix-land-agent/ROOM-ASSET-2026-08-25.md`

Reference inputs were used only for visual direction:

1. `kilix-land-desktop/assets/graphics/rooms/legend/living.png` — room style,
   camera, rendering density, and walkable-floor staging.
2. `legend-of-kilix/artwork/kilix-brand/kilix-firekitten-512.png` — Kilix color
   identity. The prompt explicitly excluded characters from the room plate.

## Kilix player atlas

- File: `casts/kilix-player.png`
- Dimensions: 1024×512 RGBA PNG
- Grid: 16 columns × 8 rows; 64×64 cells
- SHA-256:
  `065f71f24064e34abca7d0827abb8f78a5390f5fc1729cfbe6c4d680ea840515`
- Source: byte-identical copy of
  `kilix-land-desktop/assets/graphics/casts/legend-player.png`, parity-managed
  there from Legend of Kilix.
- License: CC BY-NC-SA 4.0; see `assets/LICENSE.md`.
