package com.armsx2.data.library

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for [DiscSets], the library's "Collapse multi-disc games" grouping. The cases follow
 * desktop's tests/ctest/core/game_list_tests.cpp, which covers the same rules in GameList.cpp, so
 * the two libraries keep agreeing on which entries are one game. Desktop's serial/CRC cases are
 * left out: the Android scan reads no CRC, and [noCrcCopiesOfOneDiscStaySelectable] pins down what
 * happens instead.
 */
class DiscSetsTest {
    private fun disc(
        title: String,
        path: String,
        region: String? = "USA",
        serial: String? = "SLUS-12345",
        titleEn: String = "",
        titleSort: String = "",
        set: DiscSets.Info? = null,
        isDisc: Boolean = true,
        platform: String = "ps2",
    ) = DiscSets.Disc(
        key = path,
        isDisc = isDisc,
        platform = platform,
        region = region,
        serial = serial,
        title = title,
        titleEn = titleEn,
        titleSort = titleSort,
        fileTitle = path.substringBeforeLast('.'),
        set = set,
    )

    private fun info(id: String, name: String, number: Int, nameEn: String = "", discName: String = "") =
        DiscSets.Info(id = id, name = name, nameEn = nameEn, number = number, discName = discName, discNameEn = "")

    /** Each group's members, as the input entries' keys. */
    private fun keys(discs: List<DiscSets.Disc>, groups: List<DiscSets.Group>): List<List<String>> =
        groups.map { group -> group.members.map { discs[it].key } }

    @Test
    fun collapsesBothTitleFormatsAndSortsNumerically() {
        val discs = listOf(
            disc("Game Disc 10", "ten.iso"),
            disc("Game (Disc 2)", "two.iso"),
            disc("Game - Disc 1", "one.iso"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(1, sets.size)
        assertEquals("Game", sets[0].title)
        assertEquals(listOf(listOf("one.iso", "two.iso", "ten.iso")), keys(discs, sets))
        assertEquals(listOf(1, 2, 10), sets[0].numbers)
    }

    @Test
    fun preservesRegionsEditionsAndSerialFamilies() {
        val discs = listOf(
            disc("Game (Disc 1)", "a.iso"),
            disc("Game (Disc 2)", "b.iso"),
            disc("Game (Disc 1)", "c.iso", region = "Europe"),
            disc("Game (Disc 2)", "d.iso", region = "Europe"),
            disc("Game (Disc 1) (Special Edition)", "e.iso"),
            disc("Game (Disc 2) (Special Edition)", "f.iso"),
            disc("Game Disc 2", "g.iso", serial = "SCUS-12346"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(listOf(listOf("a.iso", "b.iso"), listOf("c.iso", "d.iso"), listOf("e.iso", "f.iso")), keys(discs, sets))
        assertEquals("Game (Special Edition)", sets[2].title)
    }

    @Test
    fun collapsesSquareBracketDatabaseTitles() {
        val discs = listOf(
            disc("Armored Core - Nexus [Disc 1]", "Armored Core - Nexus (USA) (Disc 1).chd", serial = "SLUS-20986"),
            disc("Armored Core - Nexus [Disc 2]", "Armored Core - Nexus (USA) (Disc 2).chd", serial = "SLUS-21079"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(1, sets.size)
        assertEquals("Armored Core - Nexus", sets[0].title)
        assertEquals(2, sets[0].members.size)
    }

    @Test
    fun collapsesDiscCountsAndPreservesBracketedEditions() {
        val discs = listOf(
            disc("Game [DISC 1 of 2]", "Game (Disc 1 of 2).iso"),
            disc("Game [Disc 2 of 2]", "Game (Disc 2 of 2).iso"),
            disc("Game [Disc 1] [Special Edition]", "special1.iso"),
            disc("Game [Disc 2] [Special Edition]", "special2.iso"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(listOf("Game", "Game [Special Edition]"), sets.map { it.title })
    }

    @Test
    fun preservesFilenameEditionAndRevisionQualifiers() {
        val discs = listOf(
            disc("Game (Disc 1)", "Game (Disc 1) (Rev 1).iso"),
            disc("Game (Disc 2)", "Game (Disc 2) (Rev 1).iso"),
            disc("Game (Disc 1)", "Game (Disc 1) (Greatest Hits).iso"),
            disc("Game (Disc 2)", "Game (Disc 2) (Greatest Hits).iso"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(
            listOf(
                listOf("Game (Disc 1) (Rev 1).iso", "Game (Disc 2) (Rev 1).iso"),
                listOf("Game (Disc 1) (Greatest Hits).iso", "Game (Disc 2) (Greatest Hits).iso"),
            ),
            keys(discs, sets),
        )
    }

    @Test
    fun regionTagsMatchingTheDiscsOwnRegionAreNotQualifiers() {
        val discs = listOf(
            disc("Game (Disc 1)", "Game (USA) (Disc 1).iso"),
            disc("Game (Disc 2)", "Game (Disc 2).iso"),
        )
        assertEquals(1, DiscSets.build(discs).size)
    }

    @Test
    fun leavesSingleDiscsDuplicatesAndUnnumberedGamesAlone() {
        val discs = listOf(
            disc("Single (Disc 2)", "a.iso"),
            disc("Duplicate (Disc 1)", "b.iso"),
            disc("Duplicate (Disc 1)", "c.iso"),
            disc("Game", "d.iso"),
            disc("Game 2", "e.iso"),
            disc("Game Disc 0", "f.iso"),
            disc("Game Disc 999999999999999999999999999", "g.iso"),
            disc("Game Disc 1.5", "h.iso"),
            disc("Game (Disc 1", "i.iso"),
        )
        assertTrue(DiscSets.build(discs).isEmpty())
    }

    @Test
    fun keepsElfAndConsoleTypesSeparate() {
        assertTrue(DiscSets.build(listOf(disc("Game Disc 1", "a.iso"), disc("Game Disc 2", "b.elf", isDisc = false))).isEmpty())
        assertTrue(DiscSets.build(listOf(disc("Game Disc 1", "a.iso"), disc("Game Disc 2", "b.iso", platform = "ps1"))).isEmpty())
    }

    @Test
    fun normalizesLocalizedTitlesAndRepresentsFirstDisc() {
        val discs = listOf(
            disc("Game Disc 2", "b.iso", titleEn = "English Game (Disc 2)", titleSort = "Sorted Game Disc 2"),
            disc("Game Disc 1", "a.iso"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(1, sets.size)
        assertEquals("English Game", sets[0].titleEn)
        assertEquals("Sorted Game", sets[0].titleSort)
        // The first member is disc 1, which is the entry the library lists the set as.
        assertEquals("a.iso", discs[sets[0].members.first()].key)
    }

    @Test
    fun recognizesTokenizedAndLocalizedMarkers() {
        val markers = listOf(
            "(Disc01)" to "[disk-02]",
            "CD I" to "DVD II",
            "[Disc 1/2]" to "(Disc 2 of 2)",
            "［ＤＩＳＣ１／２］" to "［ＤＩＳＣ２／２］",
            "[ディスク1/2]" to "[ディスク2/2]",
            "[光盘1/2]" to "[光盘2/2]",
            "[디스크1/2]" to "[디스크2/2]",
        )
        for ((first, second) in markers) {
            val sets = DiscSets.build(listOf(disc("Game $first", "first.iso"), disc("Game $second", "second.chd")))
            assertEquals(first, 1, sets.size)
            assertEquals(first, "Game", sets[0].title)
        }
    }

    @Test
    fun matchesEnglishAliasesAndFilenameFallbacks() {
        assertEquals(
            1,
            DiscSets.build(
                listOf(
                    disc("別名", "one.iso", titleEn = "Game: Name [Disc 1]"),
                    disc("Other Native Name", "two.iso", titleEn = "game - name (Disk 2)"),
                ),
            ).size,
        )
        assertEquals(
            1,
            DiscSets.build(
                listOf(
                    disc("Library Title", "Different filename (CD 1).iso"),
                    disc("Library Title", "Another filename (CD 2).chd"),
                ),
            ).size,
        )
    }

    @Test
    fun keepsEditionsWhenOnlyOneTitleAliasContainsTheEdition() {
        val discs = listOf(
            disc("Game Disc 1", "one.iso", titleEn = "Game [Standard Edition] [Disc 1]"),
            disc("Game Disc 2", "two.iso", titleEn = "Game [Limited Edition] [Disc 2]"),
        )
        assertTrue(DiscSets.build(discs).isEmpty())
    }

    @Test
    fun explicitIdsIgnoreRenamedTitlesAndDifferentSerialFamilies() {
        val discs = listOf(
            disc("Entirely renamed", "arbitrary.iso", serial = "SLUS-00001", set = info("SLUS-00001", "Known Game", 1, nameEn = "English Game")),
            disc("Another title", "another.chd", serial = "SCUS-00002", set = info("SLUS-00001", "Known Game", 2, nameEn = "English Game")),
        )
        val sets = DiscSets.build(discs)
        assertEquals(1, sets.size)
        assertEquals("Known Game", sets[0].title)
        assertEquals("English Game", sets[0].titleEn)
        assertEquals(2, sets[0].members.size)
    }

    @Test
    fun explicitIdsKeepSameNamedReleasesSeparate() {
        val discs = buildList {
            for (release in 0..1) for (number in 1..2) {
                val serial = "SLUS-${release * 2 + number}"
                add(disc("Game Disc $number", "$serial.iso", serial = serial, set = info("RELEASE$release", "Game", number)))
            }
            add(disc("Game Disc 3", "unknown.iso", serial = "SLUS-99999"))
        }
        val sets = DiscSets.build(discs)
        assertEquals(listOf(listOf("SLUS-1.iso", "SLUS-2.iso"), listOf("SLUS-3.iso", "SLUS-4.iso")), keys(discs, sets))
    }

    @Test
    fun renamedDiscsUseOfficialPerDiscLabels() {
        val first = disc("Renamed first", "Anything (Ice).iso", set = info("SET", "Cy Girls", 1, discName = "Cy Girls [Ice] [Disc 1]"))
        val second = disc("Renamed second", "Something (Aska).chd", set = info("SET", "Cy Girls", 2, discName = "Cy Girls [Aska] [Disc 2]"))
        assertEquals(1, DiscSets.build(listOf(first, second)).size)
        assertEquals(2, DiscSets.discNumber(second))
        assertTrue(DiscSets.build(listOf(first, second.copy(region = "Japan"))).isEmpty())
    }

    @Test
    fun conflictingTitleOrdinalsDoNotFallBackToFilenames() {
        val discs = listOf(
            disc("Game Disc 1", "Game Disc 1.iso", titleEn = "Game Disc 2"),
            disc("Game Disc 2", "Game Disc 2.iso"),
        )
        assertNull(DiscSets.discNumber(discs[0]))
        assertTrue(DiscSets.build(discs).isEmpty())
    }

    @Test
    fun leavesDuplicateOrdinalsFromDifferentSerialsAmbiguous() {
        val discs = listOf(
            disc("Game Disc 1", "one.iso"),
            disc("Game Disc 1", "another.iso", serial = "SLUS-12346"),
            disc("Game Disc 2", "two.iso"),
        )
        assertTrue(DiscSets.build(discs).isEmpty())
    }

    @Test
    fun noCrcCopiesOfOneDiscStaySelectable() {
        // Desktop would compare executable CRCs here. Without them, an .iso and a .chd of disc 1
        // (same serial) both join the set, so the picker offers each image rather than guessing.
        val discs = listOf(
            disc("Game Disc 1", "one.iso"),
            disc("Game Disc 1", "one.chd"),
            disc("Game Disc 2", "two.iso", serial = "SLUS-12346"),
        )
        val sets = DiscSets.build(discs)
        assertEquals(listOf(listOf("one.chd", "one.iso", "two.iso")), keys(discs, sets))
        assertEquals(listOf(1, 1, 2), sets[0].numbers)
        // ...and two copies of a single-disc game are not a set at all.
        assertTrue(DiscSets.build(listOf(disc("Game", "game.iso"), disc("Game", "game.chd"))).isEmpty())
    }

    @Test
    fun rejectsMalformedMarkersAndStandaloneDemoIssues() {
        for (title in listOf("Game [Disc 1)", "Game (Disc 1]", "Game [Disc 3/2]", "Game Disc 1.5", "Game Disc 0")) {
            assertTrue(title, DiscSets.build(listOf(disc(title, "bad.iso"), disc("Game Disc 2", "two.iso"))).isEmpty())
        }
        assertTrue(DiscSets.build(listOf(disc("Official Demo Disc 1", "one.iso"), disc("Official Demo Disc 2", "two.iso"))).isEmpty())
    }

    @Test
    fun parsesTheNativeDiscSetRecord() {
        assertEquals(
            DiscSets.Info("SLUS-20986", "Armored Core - Nexus", "", 2, "Armored Core - Nexus [Disc 2]", ""),
            DiscSets.parseInfo("slus-20986\nArmored Core - Nexus\n\n2\nArmored Core - Nexus [Disc 2]\n"),
        )
        assertNull(DiscSets.parseInfo(""))
        assertNull(DiscSets.parseInfo(null))
        assertNull(DiscSets.parseInfo("SLUS-1\nGame\n\n0\n\n"))
        assertNull(DiscSets.parseInfo("SLUS-1\n\n\n1\n\n"))
    }
}
