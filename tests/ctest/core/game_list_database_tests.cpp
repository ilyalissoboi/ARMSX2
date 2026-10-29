// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameList.h"
#include "Config.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/YAML.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <map>
#include <set>

TEST(GameListDatabase, ValidatesAllDeclaredDiscSetsAndAuditsFallbackCoverage)
{
	const std::string database_path = Path::ToNativePath(GAME_LIST_TEST_DATABASE_PATH);
	const auto contents = FileSystem::ReadFileToString(database_path.c_str());
	ASSERT_TRUE(contents.has_value()) << database_path;
	Error error;
	auto tree = ParseYAMLFromString(ryml::to_csubstr(*contents), ryml::to_csubstr("GameIndex.yaml"), &error, true);
	ASSERT_TRUE(tree.has_value()) << error.GetDescription();
	const std::string old_resources = EmuFolders::Resources;
	ScopedGuard restore_resources = [&old_resources]() { EmuFolders::Resources = old_resources; };
	EmuFolders::Resources = std::string(Path::GetDirectory(database_path));
	std::vector<GameList::Entry> entries;
	std::map<std::string, std::vector<size_t>> declared;
	std::map<std::string, std::string> regions;
	for (const auto node : tree->rootref().children())
	{
		if (!node.is_map())
			continue;
		GameList::Entry entry;
		entry.serial.assign(node.key().str, node.key().len);
		const auto* game = GameDatabase::findGame(entry.serial);
		ASSERT_NE(game, nullptr) << entry.serial;
		entry.title = game->name;
		entry.title_en = game->name_en;
		entry.title_sort = game->name_sort;
		entry.disc_set = game->disc_set;
		entry.path = entry.serial + ".iso";
		for (u32 region = 0; region < static_cast<u32>(GameList::Region::Count); region++)
		{
			if (game->region == GameList::RegionToString(static_cast<GameList::Region>(region), false))
				entry.region = static_cast<GameList::Region>(region);
		}
		EXPECT_EQ(node.has_child("discSet"), entry.disc_set.has_value()) << entry.serial;
		if (entry.disc_set)
		{
			declared[entry.disc_set->id].push_back(entries.size());
			const auto [it, inserted] = regions.emplace(entry.disc_set->id, game->region);
			EXPECT_EQ(it->second, game->region) << entry.disc_set->id;
		}
		entries.push_back(std::move(entry));
	}
	ASSERT_FALSE(declared.empty());
	const auto sets = GameList::BuildDiscSets(entries);
	std::map<std::string, const GameList::DiscSet*> by_path;
	size_t fallback_sets = 0;
	for (const auto& set : sets)
	{
		bool has_metadata = false;
		for (const auto* entry : set.entries)
		{
			EXPECT_TRUE(by_path.emplace(entry->path, &set).second);
			has_metadata |= entry->disc_set.has_value();
		}
		fallback_sets += !has_metadata;
	}
	for (const auto& [id, members] : declared)
	{
		SCOPED_TRACE(id);
		ASSERT_GE(members.size(), 2u);
		std::set<u32> numbers;
		const auto& first = *entries[members.front()].disc_set;
		const GameList::DiscSet* matched = nullptr;
		bool has_first_serial = false;
		for (const size_t index : members)
		{
			const auto& entry = entries[index];
			const auto& info = *entry.disc_set;
			EXPECT_TRUE(numbers.insert(info.number).second) << entry.serial;
			EXPECT_EQ(info.name, first.name);
			EXPECT_EQ(info.name_en, first.name_en);
			has_first_serial |= info.number == 1 && StringUtil::compareNoCase(entry.serial, id);
			const auto it = by_path.find(entry.path);
			ASSERT_NE(it, by_path.end()) << entry.serial;
			if (!matched)
				matched = it->second;
			EXPECT_EQ(it->second, matched);
		}
		EXPECT_TRUE(has_first_serial);
		EXPECT_EQ(*numbers.rbegin(), members.size());
		EXPECT_EQ(matched->entries.size(), members.size());
	}
	std::printf("Audited %zu database entries: %zu explicit disc sets, %zu conservative fallback sets.\n",
		entries.size(), declared.size(), fallback_sets);
	for (const auto& set : sets)
	{
		if (!set.entries.front()->disc_set)
			std::printf("Fallback set: %s (%zu images)\n", set.title.c_str(), set.entries.size());
	}
	size_t unmatched = 0;
	for (const auto& entry : entries)
	{
		if (GameList::GetDiscNumberForEntry(&entry) && by_path.find(entry.path) == by_path.end())
		{
			++unmatched;
			std::printf("Unmatched numbered title: %s - %s\n", entry.serial.c_str(), entry.GetTitle(true).c_str());
		}
	}
	std::printf("%zu numbered titles remain separate (including standalone demo issues and ambiguous releases).\n", unmatched);
}
