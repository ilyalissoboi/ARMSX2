#!/usr/bin/env python3
"""The named shader packs follow their latest GitHub release and land where their presets reach."""

import re
import unittest

from ios_source import SWIFT, at, block, read

IMPORTER = SWIFT / "Models/ShaderPackImporter.swift"
SECTION = SWIFT / "Views/Settings/ShaderChainSection.swift"


def packs(text):
    return re.findall(r'ShaderPackSource\(\s*name: "([^"]+)", repository: "([^"]+)",\s*asset: #"([^"]+)"#',
                      text)


class PackSources(unittest.TestCase):
    def setUp(self):
        self.importer = read(IMPORTER)

    def test_the_three_packs_are_offered(self):
        repos = {repo for _, repo, _ in packs(self.importer)}
        self.assertEqual(repos, {"Conkwer/satpixie-crt-shader", "zoran123456/lola-retro",
                                 "RetroCrisis/Retro-Crisis-GDV-NTSC"})

    def test_no_release_is_pinned(self):
        self.assertNotIn("/releases/download/", self.importer,
                         "a pack names a fixed release asset instead of the latest release")
        self.assertIn('"https://api.github.com/repos/\\(pack.repository)/releases/latest"', self.importer)

    def test_the_asset_patterns_pick_the_retroarch_zip(self):
        assets = {repo: re.compile(pattern) for _, repo, pattern in packs(self.importer)}
        lola = assets["zoran123456/lola-retro"]
        self.assertTrue(lola.fullmatch("lola-retro-v1.19.zip"))
        for port in ("86box", "amiberry", "kodi"):
            self.assertIsNone(lola.fullmatch("lola-retro-%s-v1.19.zip" % port))
        pixie = assets["Conkwer/satpixie-crt-shader"]
        self.assertTrue(pixie.fullmatch("satpixie-crt-shader-20260122.zip"))
        self.assertIsNone(pixie.fullmatch("satpixie-crt-shader-variants-20260122.zip"))
        self.assertTrue(assets["RetroCrisis/Retro-Crisis-GDV-NTSC"].fullmatch(
            "Retro.Crisis.GDV-NTSC.2026.09.27.zip"))

    def test_the_asset_is_matched_whole(self):
        """A prefix match would take lola-retro-kodi-v1.19.zip for lola-retro-v1.19.zip's pattern."""
        self.assertIn("wholeMatch(of: pattern)", block(self.importer, "private static func latestAsset"))

    def test_retro_crisis_is_placed_from_its_references(self):
        """Its releases have shipped both a bare retro crisis/ and one under shaders/shaders_slang/."""
        self.assertRegex(self.importer, r'name: "Retro Crisis GDV-NTSC"[^)]*needsBasePack: true')
        place = block(self.importer, "private nonisolated static func place(")
        self.assertIn("companionWrapping(in: destination)", place)

    def test_a_pack_replaces_its_earlier_copy(self):
        install = block(self.importer, "func installPack(")
        self.assertIn("await download(pack.name,", install,
                      "installPack doesn't go through the download that replaces an earlier copy")


class PackRows(unittest.TestCase):
    def test_each_pack_has_a_row_and_retries_the_chain(self):
        section = read(SECTION)
        self.assertIn("ForEach(ShaderPackImporter.packs)", section)
        get = block(section, "private func getPack(")
        self.assertLess(at(get, "await importer.installPack(pack)", "the pack download"),
                        at(get, "ARMSX2Bridge.retryShaderChain()", "the retry after it"),
                        "a preset waiting on the pack isn't retried once it arrives")


if __name__ == "__main__":
    unittest.main()
