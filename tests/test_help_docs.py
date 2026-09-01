from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest

from agent.help_docs import HelpError, HelpLibrary


class HelpLibraryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "docs").mkdir()
        (self.root / "docs" / "CONTROLS.md").write_text(
            "# Controls\n\nPress Tab to switch between chat and manual mode.\n",
            encoding="utf-8",
        )
        (self.root / "README.md").write_text(
            "# Kilix\n\nThe resident uses a private semantic protocol.\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_searches_and_reads_bounded_relative_documents(self) -> None:
        library = HelpLibrary(self.root, root_kind="test")
        search = library.search("manual mode")
        self.assertEqual(search["root"], "test")
        self.assertEqual(search["results"][0]["path"], "docs/CONTROLS.md")

        split_match = library.search("controls manual")
        self.assertEqual(
            split_match["results"][0]["path"], "docs/CONTROLS.md"
        )

        excerpt = library.read("docs/CONTROLS.md", 1)
        self.assertIn("Press Tab", excerpt["content"])
        self.assertEqual(excerpt["line_start"], 1)

    def test_rejects_escape_absolute_symlink_and_binary_paths(self) -> None:
        outside = Path("/etc/passwd")
        os.symlink(outside, self.root / "docs" / "ESCAPE.md")
        (self.root / "docs" / "BINARY.md").write_bytes(b"text\0binary")
        library = HelpLibrary(self.root)

        for path in ("../outside-help.md", str(outside), "docs/ESCAPE.md"):
            with self.assertRaises(HelpError):
                library.read(path, 1)
        with self.assertRaisesRegex(HelpError, "bounded text"):
            library.read("docs/BINARY.md", 1)

    def test_rejects_non_normalized_and_post_index_symlink_swaps(self) -> None:
        swap = self.root / "docs" / "SWAP.md"
        swap.write_text("safe before indexing\n", encoding="utf-8")
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "GUIDE.md").write_text("safe nested file\n", encoding="utf-8")
        library = HelpLibrary(self.root)

        for path in ("./README.md", "docs//CONTROLS.md"):
            with self.assertRaisesRegex(HelpError, "normalized"):
                library.read(path, 1)

        swap.unlink()
        os.symlink("/etc/passwd", swap)
        with self.assertRaisesRegex(HelpError, "confined regular file"):
            library.read("docs/SWAP.md", 1)

        moved = self.root / "nested-original"
        nested.rename(moved)
        os.symlink("/etc", nested)
        with self.assertRaisesRegex(HelpError, "confined regular file"):
            library.read("nested/GUIDE.md", 1)

    def test_search_does_not_retain_the_entire_help_tree(self) -> None:
        for index in range(12):
            (self.root / f"GUIDE-{index}.md").write_text(
                f"guide token {index}\n", encoding="utf-8"
            )
        library = HelpLibrary(self.root)
        library.search("guide token")
        self.assertEqual(library.cached_document_count, 0)
        for index in range(12):
            library.read(f"GUIDE-{index}.md", 1)
        self.assertEqual(library.cached_document_count, 8)

    def test_rejects_unbounded_or_non_printable_queries(self) -> None:
        library = HelpLibrary(self.root)
        for query in (" ", "x" * 81, "snowman \N{SNOWMAN}"):
            with self.assertRaises(HelpError):
                library.search(query)

    def test_rejects_out_of_range_lines_and_unusable_index_paths(self) -> None:
        (self.root / "UNICOD\N{LATIN SMALL LETTER E WITH ACUTE}.md").write_text(
            "not indexable by the ASCII tool contract\n", encoding="utf-8"
        )
        directory = self.root
        for index in range(6):
            directory /= f"segment-{index}-" + "x" * 80
            directory.mkdir()
        (directory / "README.md").write_text("too deep\n", encoding="utf-8")
        library = HelpLibrary(self.root)

        self.assertNotIn(
            "UNICOD\N{LATIN SMALL LETTER E WITH ACUTE}.md", library._files
        )
        self.assertFalse(any(len(path) > 512 for path in library._files))
        with self.assertRaisesRegex(HelpError, "beyond"):
            library.read("README.md", 100)


if __name__ == "__main__":
    unittest.main()
