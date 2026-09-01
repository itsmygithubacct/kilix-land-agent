# Asset license

Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
(CC BY-NC-SA 4.0)

Copyright (c) 2026 itsmygithubacct

Everything in this `assets/` directory — the cast atlas, room plates, and the
demo capture — is licensed under CC BY-NC-SA 4.0. The source code of Kilix Land
Agent is licensed separately under the MIT License; see the `LICENSE` file at
the repository root.

The authoritative text of this license is published by Creative Commons at:

  https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode
  (summary: https://creativecommons.org/licenses/by-nc-sa/4.0/)

That text governs. The summary below is for orientation only and is not a
substitute for it.

You are free to share and adapt this material, provided that:

  Attribution   — you give appropriate credit, link to the license, and
                  indicate if changes were made.
  NonCommercial — you do not use the material for commercial purposes.
  ShareAlike    — if you remix or build upon the material, you distribute your
                  contributions under this same license.

No warranties are given. The license may not give you all of the permissions
necessary for your intended use.

## Provenance

None of these assets are third-party works.

- `graphics/casts/kilix-player.png` — a byte-identical copy of the Legend of
  Kilix player atlas, which `kilix-land-desktop` and `kilix-land` already
  distribute publicly under this same license as
  `assets/graphics/casts/legend-player.png`. Legend of Kilix is a separate
  project by the same author; this copy is covered by this license, which does
  not enlarge any rights in the source game itself.
- `graphics/rooms/kilix/` — original room plates for this project, produced
  with an image-generation pipeline and cooked to 1280x720 through
  `kilix-land-desktop/tools/prepare_plate.py`. Their generator, dimensions,
  hashes, prompt record, and the reference roles used for visual direction are
  recorded in `graphics/PROVENANCE.md`.
- `demo/` — a first-party screen capture of this project produced by
  `tools/qwen_video_demo.py`, with its encoder, model, speech engine, and hash
  recorded in `demo/PROVENANCE.md`.
