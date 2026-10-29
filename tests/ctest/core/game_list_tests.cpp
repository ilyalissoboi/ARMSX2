// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameList.h"
#include <gtest/gtest.h>

namespace
{
	GameList::Entry MakeDisc(std::string title, std::string path, GameList::Region region = GameList::Region::NTSC_U,
		std::string serial = "SLUS-12345")
	{
		GameList::Entry entry;
		entry.title = std::move(title);
		entry.path = std::move(path);
		entry.region = region;
		entry.serial = std::move(serial);
		return entry;
	}
} // namespace

TEST(GameListDiscSets, HistoryIncludesNonRepresentativeDiscsAndUpdatesLive)
{
	std::vector entries = {
		MakeDisc("Game Disc 1", "one.iso", GameList::Region::NTSC_U, "SLUS-12345"),
		MakeDisc("Game Disc 2", "two.iso", GameList::Region::NTSC_U, "SLUS-12346"),
	};
	entries[1].last_played_time = 200;
	entries[1].total_played_time = 60;
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].GetRepresentative(), &entries[0]);
	EXPECT_EQ(sets[0].GetPlayHistory().last_played_time, 200);
	EXPECT_EQ(sets[0].GetPlayHistory().total_played_time, 60);

	entries[0].last_played_time = 300;
	entries[0].total_played_time = 120;
	EXPECT_EQ(sets[0].GetPlayHistory().last_played_time, 300);
	EXPECT_EQ(sets[0].GetPlayHistory().total_played_time, 180);
}

TEST(GameListDiscSets, HistoryCountsCopiesAndSharedSerialsOnce)
{
	std::vector entries = {
		MakeDisc("Game Disc 1", "one.iso"),
		MakeDisc("Game Disc 1", "one.chd", GameList::Region::NTSC_U, "slus-12345"),
		MakeDisc("Game Disc 2", "two.iso"),
		MakeDisc("Game Disc 3", "three.iso", GameList::Region::NTSC_U, "SLUS-12346"),
	};
	entries[0].last_played_time = 100;
	entries[0].total_played_time = 120;
	entries[1].last_played_time = 200;
	entries[1].total_played_time = 180;
	entries[2].last_played_time = 200;
	entries[2].total_played_time = 180;
	entries[3].last_played_time = 150;
	entries[3].total_played_time = 60;
	GameList::DiscSet set;
	for (const auto& entry : entries)
		set.entries.push_back(&entry);
	EXPECT_EQ(set.GetPlayHistory().last_played_time, 200);
	EXPECT_EQ(set.GetPlayHistory().total_played_time, 240);
}

TEST(GameListDiscSets, HistoryDistinguishesDiscsWithoutSerials)
{
	std::vector entries = {
		MakeDisc("Game Disc 1", "one.iso", GameList::Region::NTSC_U, ""),
		MakeDisc("Game Disc 2", "two.iso", GameList::Region::NTSC_U, ""),
	};
	entries[0].total_played_time = 60;
	entries[1].total_played_time = 120;
	GameList::DiscSet set;
	set.entries = {&entries[0], &entries[1]};
	EXPECT_EQ(set.GetPlayHistory().total_played_time, 180);
}

TEST(GameListDiscSets, UncollapsedHistoryUsesOnlyTheSelectedDisc)
{
	auto entry = MakeDisc("Game Disc 2", "two.iso");
	entry.last_played_time = 200;
	entry.total_played_time = 60;
	const auto history = GameList::GetDisplayPlayHistory(&entry, false);
	EXPECT_EQ(history.last_played_time, 200);
	EXPECT_EQ(history.total_played_time, 60);
}

TEST(GameListDiscSets, CollapsesBothTitleFormatsAndSortsNumerically)
{
	const std::vector entries = {
		MakeDisc("Game Disc 10", "ten.iso"),
		MakeDisc("Game (Disc 2)", "two.iso"),
		MakeDisc("Game - Disc 1", "one.iso"),
	};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].title, "Game");
	EXPECT_EQ(sets[0].entries, (std::vector<const GameList::Entry*>{&entries[2], &entries[1], &entries[0]}));
}

TEST(GameListDiscSets, PreservesRegionsEditionsAndSerialFamilies)
{
	const std::vector entries = {
		MakeDisc("Game (Disc 1)", "a.iso"),
		MakeDisc("Game (Disc 2)", "b.iso"),
		MakeDisc("Game (Disc 1)", "c.iso", GameList::Region::PAL_UK),
		MakeDisc("Game (Disc 2)", "d.iso", GameList::Region::PAL_UK),
		MakeDisc("Game (Disc 1) (Special Edition)", "e.iso"),
		MakeDisc("Game (Disc 2) (Special Edition)", "f.iso"),
		MakeDisc("Game Disc 2", "g.iso", GameList::Region::NTSC_U, "SCUS-12346"),
	};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 3u);
	EXPECT_EQ(sets[0].entries.size(), 2u);
	EXPECT_EQ(sets[1].entries.size(), 2u);
	EXPECT_EQ(sets[2].title, "Game (Special Edition)");
}

TEST(GameListDiscSets, CollapsesSquareBracketDatabaseTitles)
{
	const std::vector entries = {
		MakeDisc("Armored Core - Nexus [Disc 1]", "Armored Core - Nexus (USA) (Disc 1).chd", GameList::Region::NTSC_U, "SLUS-20986"),
		MakeDisc("Armored Core - Nexus [Disc 2]", "Armored Core - Nexus (USA) (Disc 2).chd", GameList::Region::NTSC_U, "SLUS-21079"),
	};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].title, "Armored Core - Nexus");
	EXPECT_EQ(sets[0].entries.size(), 2u);
}

TEST(GameListDiscSets, CollapsesDiscCountsAndPreservesBracketedEditions)
{
	const std::vector entries = {
		MakeDisc("Game [DISC 1 of 2]", "Game (Disc 1 of 2).iso"),
		MakeDisc("Game [Disc 2 of 2]", "Game (Disc 2 of 2).iso"),
		MakeDisc("Game [Disc 1] [Special Edition]", "special1.iso"),
		MakeDisc("Game [Disc 2] [Special Edition]", "special2.iso"),
	};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 2u);
	EXPECT_EQ(sets[0].title, "Game");
	EXPECT_EQ(sets[1].title, "Game [Special Edition]");
}

TEST(GameListDiscSets, PreservesFilenameEditionAndRevisionQualifiers)
{
	const std::vector entries = {
		MakeDisc("Game (Disc 1)", "Game (Disc 1) (Rev 1).iso"),
		MakeDisc("Game (Disc 2)", "Game (Disc 2) (Rev 1).iso"),
		MakeDisc("Game (Disc 1)", "Game (Disc 1) (Greatest Hits).iso"),
		MakeDisc("Game (Disc 2)", "Game (Disc 2) (Greatest Hits).iso"),
	};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 2u);
	EXPECT_EQ(sets[0].entries.size(), 2u);
	EXPECT_EQ(sets[1].entries.size(), 2u);
}

TEST(GameListDiscSets, LeavesSingleDiscsDuplicatesAndUnnumberedGamesAlone)
{
	const std::vector entries = {
		MakeDisc("Single (Disc 2)", "a.iso"),
		MakeDisc("Duplicate (Disc 1)", "b.iso"),
		MakeDisc("Duplicate (Disc 1)", "c.iso"),
		MakeDisc("Game", "d.iso"),
		MakeDisc("Game 2", "e.iso"),
		MakeDisc("Game Disc 0", "f.iso"),
		MakeDisc("Game Disc 999999999999999999999999999", "g.iso"),
		MakeDisc("Game Disc 1.5", "h.iso"),
		MakeDisc("Game (Disc 1", "i.iso"),
	};
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, KeepsElfAndConsoleTypesSeparate)
{
	std::vector entries = {MakeDisc("Game Disc 1", "a.iso"), MakeDisc("Game Disc 2", "b.elf")};
	entries[1].type = GameList::EntryType::ELF;
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
	entries[1].type = GameList::EntryType::PS1Disc;
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, NormalizesLocalizedTitlesAndRepresentsFirstDisc)
{
	std::vector entries = {MakeDisc("Game Disc 2", "b.iso"), MakeDisc("Game Disc 1", "a.iso")};
	entries[0].title_en = "English Game (Disc 2)";
	entries[0].title_sort = "Sorted Game Disc 2";
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].title_en, "English Game");
	EXPECT_EQ(sets[0].title_sort, "Sorted Game");
	EXPECT_EQ(sets[0].GetRepresentative(), &entries[1]);
}

TEST(GameListDiscSets, RecognizesTokenizedAndLocalizedMarkers)
{
	const std::pair<const char*, const char*> markers[] = {
		{"(Disc01)", "[disk-02]"},
		{"CD I", "DVD II"},
		{"[Disc 1/2]", "(Disc 2 of 2)"},
		{"［ＤＩＳＣ１／２］", "［ＤＩＳＣ２／２］"},
		{"[ディスク1/2]", "[ディスク2/2]"},
		{"[光盘1/2]", "[光盘2/2]"},
		{"[디스크1/2]", "[디스크2/2]"},
	};
	for (const auto& [first, second] : markers)
	{
		SCOPED_TRACE(first);
		const std::vector entries = {MakeDisc(std::string("Game ") + first, "first.iso"),
			MakeDisc(std::string("Game ") + second, "second.chd")};
		const auto sets = GameList::BuildDiscSets(entries);
		ASSERT_EQ(sets.size(), 1u);
		EXPECT_EQ(sets[0].title, "Game");
	}
}

TEST(GameListDiscSets, MatchesEnglishAliasesAndFilenameFallbacks)
{
	std::vector entries = {MakeDisc("別名", "one.iso"), MakeDisc("Other Native Name", "two.iso")};
	entries[0].title_en = "Game: Name [Disc 1]";
	entries[1].title_en = "game - name (Disk 2)";
	ASSERT_EQ(GameList::BuildDiscSets(entries).size(), 1u);
	entries = {MakeDisc("Library Title", "Different filename (CD 1).iso"),
		MakeDisc("Library Title", "Another filename (CD 2).chd")};
	EXPECT_EQ(GameList::BuildDiscSets(entries).size(), 1u);
}

TEST(GameListDiscSets, KeepsEditionsWhenOnlyOneTitleAliasContainsTheEdition)
{
	std::vector entries = {MakeDisc("Game Disc 1", "one.iso"), MakeDisc("Game Disc 2", "two.iso")};
	entries[0].title_en = "Game [Standard Edition] [Disc 1]";
	entries[1].title_en = "Game [Limited Edition] [Disc 2]";
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, ExplicitIdsIgnoreRenamedTitlesAndDifferentSerialFamilies)
{
	std::vector entries = {MakeDisc("Entirely renamed", "arbitrary.iso", GameList::Region::NTSC_U, "SLUS-00001"),
		MakeDisc("Another title", "another.chd", GameList::Region::NTSC_U, "SCUS-00002")};
	entries[0].disc_set = GameDatabaseSchema::DiscSetInfo{"SLUS-00001", "Known Game", "English Game", 1};
	entries[1].disc_set = GameDatabaseSchema::DiscSetInfo{"SLUS-00001", "Known Game", "English Game", 2};
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].title, "Known Game");
	EXPECT_EQ(sets[0].title_en, "English Game");
	EXPECT_EQ(sets[0].entries.size(), 2u);
}

TEST(GameListDiscSets, ExplicitIdsKeepSameNamedReleasesSeparate)
{
	std::vector<GameList::Entry> entries;
	for (int release = 0; release < 2; release++)
	{
		for (u32 number = 1; number <= 2; number++)
		{
			const std::string serial = "SLUS-" + std::to_string(release * 2 + number);
			auto entry = MakeDisc("Game Disc " + std::to_string(number), serial + ".iso", GameList::Region::NTSC_U, serial);
			entry.disc_set = GameDatabaseSchema::DiscSetInfo{"release" + std::to_string(release), "Game", "", number};
			entries.push_back(std::move(entry));
		}
	}
	entries.push_back(MakeDisc("Game Disc 3", "unknown.iso", GameList::Region::NTSC_U, "SLUS-99999"));
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 2u);
	EXPECT_EQ(sets[0].entries.size(), 2u);
	EXPECT_EQ(sets[1].entries.size(), 2u);
}

TEST(GameListDiscSets, RenamedDiscsUseOfficialPerDiscLabels)
{
	std::vector entries = {MakeDisc("Renamed first", "Anything (Ice).iso"), MakeDisc("Renamed second", "Something (Aska).chd")};
	entries[0].disc_set = GameDatabaseSchema::DiscSetInfo{"set", "Cy Girls", "", 1, "Cy Girls [Ice] [Disc 1]", ""};
	entries[1].disc_set = GameDatabaseSchema::DiscSetInfo{"set", "Cy Girls", "", 2, "Cy Girls [Aska] [Disc 2]", ""};
	ASSERT_EQ(GameList::BuildDiscSets(entries).size(), 1u);
	EXPECT_EQ(GameList::GetDiscNumberForEntry(&entries[1]), 2u);
	entries[1].region = GameList::Region::NTSC_J;
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, ConflictingTitleOrdinalsDoNotFallBackToFilenames)
{
	std::vector entries = {MakeDisc("Game Disc 1", "Game Disc 1.iso"), MakeDisc("Game Disc 2", "Game Disc 2.iso")};
	entries[0].title_en = "Game Disc 2";
	EXPECT_FALSE(GameList::GetDiscNumberForEntry(&entries[0]).has_value());
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, ExplicitIdsPreserveFilenameRevisions)
{
	std::vector<GameList::Entry> entries;
	for (int revision = 1; revision <= 2; revision++)
	{
		for (u32 number = 1; number <= 2; number++)
		{
			const std::string serial = "SLUS-" + std::to_string(number);
			auto entry = MakeDisc("Game Disc " + std::to_string(number), serial + " (Rev " + std::to_string(revision) + ").iso",
				GameList::Region::NTSC_U, serial);
			entry.disc_set = GameDatabaseSchema::DiscSetInfo{"SLUS-1", "Game", "", number};
			entry.crc = revision * 10 + number;
			entries.push_back(std::move(entry));
		}
	}
	EXPECT_EQ(GameList::BuildDiscSets(entries).size(), 2u);
}

TEST(GameListDiscSets, SerialCrcCandidatesKeepAllImagePaths)
{
	std::vector entries = {MakeDisc("Game", "game.iso"), MakeDisc("Game", "game.chd")};
	entries[0].crc = entries[1].crc = 0x12345678;
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	ASSERT_EQ(sets[0].entries.size(), 2u);
	EXPECT_NE(sets[0].entries[0]->path, sets[0].entries[1]->path);
	entries[1].crc++;
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
	entries[1].crc = entries[0].crc;
	entries[1].serial = "SLUS-12346";
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, ReusedExecutablesDoNotEraseDistinctDiscs)
{
	std::vector entries = {MakeDisc("Game Disc 1", "one.iso"), MakeDisc("Game Disc 2", "two.iso")};
	entries[0].crc = entries[1].crc = 0x12345678;
	const auto sets = GameList::BuildDiscSets(entries);
	ASSERT_EQ(sets.size(), 1u);
	EXPECT_EQ(sets[0].entries.size(), 2u);
}

TEST(GameListDiscSets, LeavesUnlabelledRevisionsAndDuplicateOrdinalsAmbiguous)
{
	std::vector entries = {MakeDisc("Game Disc 1", "one.iso"), MakeDisc("Game Disc 1", "another.iso"),
		MakeDisc("Game Disc 2", "two.iso")};
	entries[0].crc = 1;
	entries[1].crc = 2;
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
	entries[1].crc = 0;
	entries[0].crc = 0;
	entries[1].serial = "SLUS-12346";
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}

TEST(GameListDiscSets, RejectsMalformedMarkersAndStandaloneDemoIssues)
{
	for (const char* title : {"Game [Disc 1)", "Game (Disc 1]", "Game [Disc 3/2]", "Game Disc 1.5", "Game Disc 0"})
	{
		SCOPED_TRACE(title);
		const std::vector entries = {MakeDisc(title, "bad.iso"), MakeDisc("Game Disc 2", "two.iso")};
		EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
	}
	const std::vector entries = {MakeDisc("Official Demo Disc 1", "one.iso"), MakeDisc("Official Demo Disc 2", "two.iso")};
	EXPECT_TRUE(GameList::BuildDiscSets(entries).empty());
}
