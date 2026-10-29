// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Arcade/AcGameFile.h"
#include "Arcade/ArcadeFiles.h"
#include "CDVD/CDVD.h"
#include "Elfheader.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "VMManager.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Error.h"
#include "common/HTTPDownloader.h"
#include "common/HeterogeneousContainers.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

namespace GameList
{
	enum : u32
	{
		GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
		GAME_LIST_CACHE_VERSION = 34,


		PLAYED_TIME_SERIAL_LENGTH = 32,
		PLAYED_TIME_LAST_TIME_LENGTH = 20, // uint64
		PLAYED_TIME_TOTAL_TIME_LENGTH = 20, // uint64
		PLAYED_TIME_LINE_LENGTH = PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1 + PLAYED_TIME_TOTAL_TIME_LENGTH,
	};

	struct PlayedTimeEntry
	{
		std::time_t last_played_time;
		std::time_t total_played_time;
	};

	using CacheMap = UnorderedStringMap<Entry>;
	using PlayedTimeMap = UnorderedStringMap<PlayedTimeEntry>;

	static bool IsScannableFilename(const std::string_view path);

	static bool GetIsoSerialAndCRC(const std::string& path, s32* disc_type, std::string* serial, u32* crc);
	static Region ParseDatabaseRegion(const std::string_view db_region);
	static bool GetElfListEntry(const std::string& path, GameList::Entry* entry);
	static bool GetArcadeListEntry(const std::string& path, GameList::Entry* entry);
	static bool GetIsoListEntry(const std::string& path, GameList::Entry* entry);

	static bool GetGameListEntryFromCache(const std::string& path, GameList::Entry* entry);
	static void ScanDirectory(const char* path, bool recursive, bool only_cache, const std::vector<std::string>& excluded_paths,
		const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini, ProgressCallback* progress);
	static bool AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map);
	static bool ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
		const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini);

	static void LoadCache();
	static bool LoadEntriesFromCache(std::FILE* stream);
	static bool OpenCacheForWriting();
	static bool WriteEntryToCache(const GameList::Entry* entry);
	static void CloseCacheFileStream();
	static void DeleteCacheFile();
	static void RewriteCacheFile();

	static std::string GetPlayedTimeFile();
	static bool ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry);
	static std::string MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry);
	static PlayedTimeMap LoadPlayedTimeMap(const std::string& path);
	static PlayedTimeEntry UpdatePlayedTimeFile(
		const std::string& path, const std::string& serial, std::time_t last_time, std::time_t add_time);

	static std::string GetCustomPropertiesFile();

	static std::string EncodeIniKey(const std::string_view& input);
} // namespace GameList

static std::vector<GameList::Entry> s_entries;
static std::recursive_mutex s_mutex;
static GameList::CacheMap s_cache_map;
static std::FILE* s_cache_write_stream = nullptr;
static bool s_disc_sets_dirty = true;
static std::vector<GameList::DiscSet> s_disc_sets;
static std::unordered_map<std::string, size_t> s_disc_set_by_path;

namespace
{
	struct NumberedDiscTitle
	{
		std::string title;
		u32 number;
	};

	bool IsSpace(char c)
	{
		return std::isspace(static_cast<unsigned char>(c)) != 0;
	}

	bool IsSeparator(char c)
	{
		return IsSpace(c) || c == '-' || c == '_' || c == ':' || c == '.';
	}

	std::string NormalizeDiscText(std::string_view text)
	{
		std::string result;
		for (size_t offset = 0; offset < text.size();)
		{
			char32_t ch;
			const size_t length = StringUtil::DecodeUTF8(text, offset, &ch);
			if (length == 0)
			{
				result.push_back(text[offset++]);
				continue;
			}
			offset += length;
			if (ch >= 0xFF01 && ch <= 0xFF5E)
				ch -= 0xFEE0; // Full-width ASCII letters, digits and delimiters.
			else if (ch == 0x3000 || ch == 0xA0)
				ch = ' ';
			else if (ch >= 0x2010 && ch <= 0x2014)
				ch = '-';
			StringUtil::EncodeAndAppendUTF8(result, ch);
		}
		return result;
	}

	std::string MatchingKey(std::string_view text)
	{
		const std::string normalized = NormalizeDiscText(text);
		std::string key;
		bool space = false;
		// Keep non-ASCII text intact; locale-dependent case folding needs proper testing.
		for (const unsigned char ch : normalized)
		{
			if (ch < 0x80 && !std::isalnum(ch))
			{
				space = !key.empty();
				continue;
			}
			if (space)
				key.push_back(' ');
			space = false;
			key.push_back(ch < 0x80 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch));
		}
		return key;
	}

	bool MatchesToken(std::string_view text, size_t offset, std::string_view token)
	{
		return offset + token.size() <= text.size() && StringUtil::compareNoCase(text.substr(offset, token.size()), token);
	}

	std::optional<u32> ReadDiscNumber(std::string_view text, size_t* offset)
	{
		const size_t start = *offset;
		while (*offset < text.size() && text[*offset] >= '0' && text[*offset] <= '9')
			++*offset;
		if (*offset != start)
		{
			const auto number = StringUtil::FromChars<u32>(text.substr(start, *offset - start));
			return number && *number > 0 && *number <= 99 ? number : std::nullopt;
		}
		while (*offset < text.size() && (text[*offset] == 'I' || text[*offset] == 'i' || text[*offset] == 'V' ||
											text[*offset] == 'v' || text[*offset] == 'X' || text[*offset] == 'x'))
		{
			++*offset;
		}
		static constexpr std::string_view roman[] = {
			"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
			"XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX"};
		for (u32 i = 0; i < std::size(roman); i++)
		{
			if (StringUtil::compareNoCase(text.substr(start, *offset - start), roman[i]))
				return i + 1;
		}
		return std::nullopt;
	}

	std::optional<NumberedDiscTitle> ParseDiscTitle(std::string_view title)
	{
		const std::string text = NormalizeDiscText(title);
		static constexpr std::string_view labels[] = {"disc", "disk", "dvd", "cd", "ディスク", "光盘", "光碟", "디스크"};
		for (size_t pos = 0; pos < text.size(); pos++)
		{
			if (pos > 0 && !IsSeparator(text[pos - 1]) && text[pos - 1] != '[' && text[pos - 1] != '(')
				continue;
			for (const std::string_view label : labels)
			{
				if (!MatchesToken(text, pos, label))
					continue;
				size_t start = pos;
				while (start > 0 && IsSpace(text[start - 1]))
					--start;
				char close = 0;
				if (start > 0 && (text[start - 1] == '[' || text[start - 1] == '('))
					close = text[--start] == '[' ? ']' : ')';
				size_t end = pos + label.size();
				while (end < text.size() && IsSeparator(text[end]))
					++end;
				const auto number = ReadDiscNumber(text, &end);
				if (!number)
					continue;
				const size_t number_end = end;
				while (end < text.size() && IsSpace(text[end]))
					++end;
				if (MatchesToken(text, end, "of") || (end < text.size() && text[end] == '/'))
				{
					end += text[end] == '/' ? 1 : 2;
					while (end < text.size() && IsSpace(text[end]))
						++end;
					const auto count = ReadDiscNumber(text, &end);
					if (!count || *count < *number)
						continue;
					while (end < text.size() && IsSpace(text[end]))
						++end;
				}
				else if (!close)
				{
					end = number_end;
				}
				if (close)
				{
					if (end == text.size() || text[end] != close)
						continue;
					++end;
				}
				if (end < text.size() && !IsSpace(text[end]) && text[end] != '[' && text[end] != '(' && text[end] != '-')
					continue;
				while (start > 0 && IsSeparator(text[start - 1]))
					--start;
				std::string base(text.substr(0, start));
				const std::string_view suffix = StringUtil::StripWhitespace(std::string_view(text).substr(end));
				if (!base.empty() && !suffix.empty())
					base.push_back(' ');
				base.append(suffix);
				StringUtil::StripWhitespace(&base);
				if (!base.empty())
					return NumberedDiscTitle{std::move(base), *number};
			}
		}
		return std::nullopt;
	}

	std::string GetDiscSetTitle(const std::string& title)
	{
		auto parsed = ParseDiscTitle(title);
		return parsed ? std::move(parsed->title) : title;
	}

	std::set<std::string> GetTitleTags(std::string_view title)
	{
		const std::string text = NormalizeDiscText(title);
		std::set<std::string> tags;
		for (size_t start = 0; start < text.size(); start++)
		{
			if (text[start] != '[' && text[start] != '(')
				continue;
			const size_t end = text.find(text[start] == '[' ? ']' : ')', start + 1);
			if (end == std::string::npos)
				continue;
			const std::string_view tag = std::string_view(text).substr(start, end - start + 1);
			const auto disc = ParseDiscTitle(fmt::format("Title {}", tag));
			if (!disc || disc->title != "Title")
				tags.insert(MatchingKey(tag));
			start = end;
		}
		return tags;
	}

	std::string GetReleaseQualifiers(const GameList::Entry& entry)
	{
		auto tags = GetTitleTags(Path::GetFileTitle(entry.path));
		// Database titles already carry edition information. Explicit metadata can also identify
		// per-disc labels such as Cy Girls' Ice/Aska, without stripping arbitrary filename tags.
		const std::string empty;
		for (const std::string* title : {&entry.title, &entry.title_en,
				 entry.disc_set ? &entry.disc_set->disc_name : &empty,
				 entry.disc_set ? &entry.disc_set->disc_name_en : &empty})
		{
			for (const std::string& tag : GetTitleTags(*title))
			{
				tags.erase(tag);
				if (entry.disc_set)
				{
					if (const auto disc = ParseDiscTitle(tag))
					{
						const std::string label = MatchingKey(disc->title);
						tags.erase(label);
						if (label == "english")
							tags.erase("en");
						else if (label == "japanese")
							tags.erase("ja");
					}
				}
			}
		}
		if (!entry.disc_set)
		{
			for (const std::string* title : {&entry.title, &entry.title_en})
			{
				const auto title_tags = GetTitleTags(*title);
				tags.insert(title_tags.begin(), title_tags.end());
			}
		}
		if (entry.region == GameList::Region::NTSC_U)
		{
			tags.erase("usa");
			tags.erase("us");
		}
		else if (entry.region == GameList::Region::NTSC_J)
		{
			tags.erase("japan");
			tags.erase("jp");
		}
		else if (entry.region >= GameList::Region::PAL_A && entry.region < GameList::Region::Count)
		{
			tags.erase("europe");
			tags.erase("eur");
		}
		std::string result;
		for (const std::string& tag : tags)
			fmt::format_to(std::back_inserter(result), "{}\n", tag);
		return result;
	}

	class DiscSetUnion
	{
	public:
		explicit DiscSetUnion(size_t size)
			: m_parents(size)
		{
			std::iota(m_parents.begin(), m_parents.end(), 0);
		}
		size_t Find(size_t i)
		{
			while (m_parents[i] != i)
			{
				m_parents[i] = m_parents[m_parents[i]];
				i = m_parents[i];
			}
			return i;
		}
		void Join(size_t lhs, size_t rhs)
		{
			lhs = Find(lhs);
			rhs = Find(rhs);
			m_parents[std::max(lhs, rhs)] = std::min(lhs, rhs);
		}

	private:
		std::vector<size_t> m_parents;
	};
} // namespace

const GameList::Entry* GameList::DiscSet::GetRepresentative() const
{
	return entries.front();
}

GameList::PlayHistory GameList::DiscSet::GetPlayHistory() const
{
	PlayHistory history;
	std::unordered_map<std::string, std::time_t> serial_times;
	for (const Entry* entry : entries)
	{
		history.last_played_time = std::max(history.last_played_time, entry->last_played_time);
		const std::string key = entry->serial.empty() ? "path:" + entry->path : "serial:" + StringUtil::toUpper(entry->serial);
		// Copies and discs sharing a serial read the same cumulative playtime record.
		// Take its newest value once, rather than counting each image as a separate session.
		auto& time = serial_times[key];
		time = std::max(time, entry->total_played_time);
	}
	for (const auto& [serial, time] : serial_times)
		history.total_played_time += time;
	return history;
}

std::optional<u32> GameList::GetDiscNumberForEntry(const Entry* entry)
{
	if (entry->disc_set)
		return entry->disc_set->number;
	std::optional<u32> number;
	for (const std::string* title : {&entry->title, &entry->title_en})
	{
		if (const auto parsed = ParseDiscTitle(*title))
		{
			if (number && *number != parsed->number)
				return std::nullopt;
			number = parsed->number;
		}
	}
	if (!number)
	{
		if (const auto parsed = ParseDiscTitle(Path::GetFileTitle(entry->path)))
			number = parsed->number;
	}
	return number;
}

std::vector<GameList::DiscSet> GameList::BuildDiscSets(std::span<const Entry> entries)
{
	DiscSetUnion matches(entries.size());
	std::vector<u32> numbers(entries.size());
	std::vector<std::string> known_ids(entries.size());
	std::map<std::string, std::vector<size_t>> by_alias;
	std::map<std::string, std::vector<size_t>> by_image_id;
	std::unordered_map<std::string, size_t> by_set_id;

	for (size_t i = 0; i < entries.size(); i++)
	{
		const Entry& entry = entries[i];
		if (!entry.IsDisc())
			continue;
		numbers[i] = GetDiscNumberForEntry(&entry).value_or(0);
		const std::string release = fmt::format("{}\n{}\n{}", static_cast<u32>(entry.type),
			static_cast<u32>(entry.region), GetReleaseQualifiers(entry));
		const std::string family = StringUtil::toUpper(std::string_view(entry.serial).substr(0, entry.serial.find('-')));
		std::set<std::string> names;
		if (entry.disc_set)
		{
			known_ids[i] = StringUtil::toUpper(entry.disc_set->id);
			const auto [it, inserted] = by_set_id.emplace(release + '\n' + known_ids[i], i);
			if (!inserted)
				matches.Join(i, it->second);
		}
		else
		{
			if (numbers[i] != 0)
			{
				for (const std::string* title : {&entry.title, &entry.title_en})
				{
					if (const auto parsed = ParseDiscTitle(*title))
						names.insert(MatchingKey(parsed->title));
				}
			}
			if (numbers[i] != 0 && names.empty())
			{
				if (const auto parsed = ParseDiscTitle(Path::GetFileTitle(entry.path)))
				{
					numbers[i] = parsed->number;
					names.insert(MatchingKey(parsed->title));
					if (!entry.title.empty())
						names.insert(MatchingKey(entry.title));
				}
			}
			// Issue numbers on standalone demo/sampler discs are not disc-set ordinals.
			if (std::any_of(names.begin(), names.end(), [](const std::string& name) {
					return name.find("demo") != std::string::npos || name.find("sampler") != std::string::npos || name.find("magazine") != std::string::npos;
				}))
				names.clear();
		}
		for (const std::string& name : names)
		{
			if (!name.empty())
				by_alias[release + '\n' + family + '\n' + name].push_back(i);
		}
		if (!entry.serial.empty() && entry.crc != 0)
		{
			const std::string title = entry.disc_set ? entry.disc_set->name : GetDiscSetTitle(entry.title);
			by_image_id[fmt::format("{}\n{}\n{}\n{}", release, StringUtil::toUpper(entry.serial), entry.crc, MatchingKey(title))].push_back(i);
		}
	}

	for (const auto& [key, members] : by_alias)
	{
		for (const size_t i : members)
			matches.Join(members.front(), i);
	}
	for (const auto& [key, members] : by_image_id)
	{
		std::set<u32> ordinals;
		std::set<std::string> ids;
		for (const size_t i : members)
		{
			if (numbers[i] != 0)
				ordinals.insert(numbers[i]);
			if (!known_ids[i].empty())
				ids.insert(known_ids[i]);
		}
		// Some discs reuse an executable/serial. Only group candidate images of the same ordinal.
		if (ordinals.size() > 1 || ids.size() > 1)
			continue;
		for (const size_t i : members)
		{
			if (!ordinals.empty())
				numbers[i] = *ordinals.begin();
			matches.Join(members.front(), i);
		}
	}

	std::map<size_t, std::vector<size_t>> groups;
	for (size_t i = 0; i < entries.size(); i++)
	{
		if (entries[i].IsDisc())
			groups[matches.Find(i)].push_back(i);
	}
	std::vector<DiscSet> sets;
	for (auto& [root, members] : groups)
	{
		if (members.size() < 2)
			continue;
		std::set<u32> ordinals;
		std::set<std::pair<std::string, u32>> image_ids;
		std::map<std::pair<std::string, u32>, std::set<u32>> versions;
		std::map<u32, std::set<std::string>> serials_by_number;
		bool identified = true;
		for (const size_t i : members)
		{
			const Entry& entry = entries[i];
			if (numbers[i] != 0)
				ordinals.insert(numbers[i]);
			const std::string serial = StringUtil::toUpper(entry.serial);
			if (numbers[i] != 0 && !serial.empty())
				serials_by_number[numbers[i]].insert(serial);
			identified &= !serial.empty() && entry.crc != 0;
			image_ids.emplace(serial, entry.crc);
			if (!serial.empty() && entry.crc != 0)
				versions[{serial, numbers[i]}].insert(entry.crc);
		}
		if (std::any_of(versions.begin(), versions.end(), [](const auto& version) { return version.second.size() > 1; }))
			continue; // An unlabelled revision cannot safely be paired with the other discs.
		if (std::any_of(serials_by_number.begin(), serials_by_number.end(), [](const auto& number) { return number.second.size() > 1; }))
			continue; // More than one release supplies the same ordinal; naming alone is ambiguous.
		if (ordinals.size() < 2 && !(identified && image_ids.size() == 1))
			continue;
		std::sort(members.begin(), members.end(), [&entries, &numbers](size_t lhs, size_t rhs) {
			if (numbers[lhs] != numbers[rhs])
				return numbers[lhs] < numbers[rhs];
			if (entries[lhs].serial != entries[rhs].serial)
				return entries[lhs].serial < entries[rhs].serial;
			return entries[lhs].path < entries[rhs].path;
		});
		DiscSet set;
		for (const size_t i : members)
		{
			const Entry& entry = entries[i];
			set.entries.push_back(&entry);
			if (set.title.empty())
				set.title = entry.disc_set ? entry.disc_set->name : GetDiscSetTitle(entry.title);
			if (set.title_en.empty())
				set.title_en = entry.disc_set ? entry.disc_set->name_en : GetDiscSetTitle(entry.title_en);
			if (set.title_sort.empty())
				set.title_sort = entry.disc_set ? entry.disc_set->name : GetDiscSetTitle(entry.title_sort);
		}
		sets.push_back(std::move(set));
	}
	return sets;
}

const GameList::DiscSet* GameList::GetDiscSetForEntry(const Entry* entry)
{
	if (s_disc_sets_dirty)
	{
		s_disc_set_by_path.clear();
		s_disc_sets = BuildDiscSets(s_entries);
		for (size_t i = 0; i < s_disc_sets.size(); i++)
		{
			for (const Entry* member : s_disc_sets[i].entries)
				s_disc_set_by_path.emplace(member->path, i);
		}
		s_disc_sets_dirty = false;
	}
	const auto it = s_disc_set_by_path.find(entry->path);
	return it != s_disc_set_by_path.end() ? &s_disc_sets[it->second] : nullptr;
}

const std::string& GameList::GetDisplayTitle(const Entry* entry, bool collapse_discs, bool force_en, bool sort)
{
	if (collapse_discs)
	{
		if (const DiscSet* set = GetDiscSetForEntry(entry))
		{
			if (force_en && !set->title_en.empty())
				return set->title_en;
			return sort && !set->title_sort.empty() ? set->title_sort : set->title;
		}
	}
	return sort ? entry->GetTitleSort(force_en) : entry->GetTitle(force_en);
}

GameList::PlayHistory GameList::GetDisplayPlayHistory(const Entry* entry, bool collapse_discs)
{
	if (collapse_discs)
	{
		if (const DiscSet* set = GetDiscSetForEntry(entry))
			return set->GetPlayHistory();
	}
	return {entry->last_played_time, entry->total_played_time};
}

void GameList::ClearPlayedTimeForEntry(const Entry* entry, bool collapse_discs)
{
	std::set<std::string> serials;
	{
		const auto lock = GetLock();
		const DiscSet* set = collapse_discs ? GetDiscSetForEntry(entry) : nullptr;
		if (set)
		{
			for (const Entry* disc : set->entries)
				serials.insert(disc->serial);
		}
		else
		{
			serials.insert(entry->serial);
		}
	}
	for (const std::string& serial : serials)
		ClearPlayedTimeForSerial(serial);
}

const char* GameList::EntryTypeToString(EntryType type, bool translate)
{
	static constexpr std::array<const char*, static_cast<int>(EntryType::Count)> names = {
		TRANSLATE_NOOP("GameList", "PS2 Disc"),
		TRANSLATE_NOOP("GameList", "PS1 Disc"),
		TRANSLATE_NOOP("GameList", "ELF"),
		TRANSLATE_NOOP("GameList", "Invalid"),
		TRANSLATE_NOOP("GameList", "Arcade"),
	};

	const char* name = names.at(static_cast<int>(type));
	if (translate)
		name = TRANSLATE("GameList", name);

	return name;
}

const char* GameList::RegionToString(Region region, bool translate)
{
	static constexpr std::array<const char*, static_cast<int>(Region::Count)> names = {
		TRANSLATE_NOOP("GameList", "NTSC-B"),
		TRANSLATE_NOOP("GameList", "NTSC-C"),
		TRANSLATE_NOOP("GameList", "NTSC-HK"),
		TRANSLATE_NOOP("GameList", "NTSC-J"),
		TRANSLATE_NOOP("GameList", "NTSC-K"),
		TRANSLATE_NOOP("GameList", "NTSC-T"),
		TRANSLATE_NOOP("GameList", "NTSC-U"),
		TRANSLATE_NOOP("GameList", "Other"),
		TRANSLATE_NOOP("GameList", "PAL-A"),
		TRANSLATE_NOOP("GameList", "PAL-AF"),
		TRANSLATE_NOOP("GameList", "PAL-AU"),
		TRANSLATE_NOOP("GameList", "PAL-BE"),
		TRANSLATE_NOOP("GameList", "PAL-E"),
		TRANSLATE_NOOP("GameList", "PAL-F"),
		TRANSLATE_NOOP("GameList", "PAL-FI"),
		TRANSLATE_NOOP("GameList", "PAL-G"),
		TRANSLATE_NOOP("GameList", "PAL-GR"),
		TRANSLATE_NOOP("GameList", "PAL-I"),
		TRANSLATE_NOOP("GameList", "PAL-IN"),
		TRANSLATE_NOOP("GameList", "PAL-M"),
		TRANSLATE_NOOP("GameList", "PAL-NL"),
		TRANSLATE_NOOP("GameList", "PAL-NO"),
		TRANSLATE_NOOP("GameList", "PAL-P"),
		TRANSLATE_NOOP("GameList", "PAL-PL"),
		TRANSLATE_NOOP("GameList", "PAL-R"),
		TRANSLATE_NOOP("GameList", "PAL-S"),
		TRANSLATE_NOOP("GameList", "PAL-SC"),
		TRANSLATE_NOOP("GameList", "PAL-SW"),
		TRANSLATE_NOOP("GameList", "PAL-SWI"),
		TRANSLATE_NOOP("GameList", "PAL-UK"),
	};

	const char* name = names.at(static_cast<int>(region));
	if (translate)
		name = TRANSLATE("GameList", name);

	return name;
}

const char* GameList::RegionToFlagFilename(Region region)
{
	static constexpr std::array<const char*, static_cast<int>(Region::Count)> flag_names = {
		"br",  // NTSC-B
		"cn",  // NTSC-C
		"hk",  // NTSC-HK
		"jp",  // NTSC-J
		"kr",  // NTSC-K
		"tw",  // NTSC-T
		"us",  // NTSC-U
		"Other",  // Other
		"au",  // PAL-A
		"za",  // PAL-AF
		"at",  // PAL-AU
		"be",  // PAL-BE
		"eu",  // PAL-E
		"fr",  // PAL-F
		"fi",  // PAL-FI
		"de",  // PAL-G
		"gr",  // PAL-GR
		"it",  // PAL-I
		"in",  // PAL-IN
		"eu",  // PAL-M
		"nl",  // PAL-NL
		"no",  // PAL-NO
		"pt",  // PAL-P
		"pl",  // PAL-PL
		"ru",  // PAL-R
		"es",  // PAL-S
		"scn",  // PAL-SC
		"se",  // PAL-SW
		"ch",  // PAL-SWI
		"gb",  // PAL-UK
	};

	return flag_names.at(static_cast<int>(region));
}

const char* GameList::EntryCompatibilityRatingToString(CompatibilityRating rating, bool translate)
{
	const char* name = "";
	switch (rating)
	{
		case CompatibilityRating::Unknown:
			name = TRANSLATE_NOOP("GameList", "Unknown");
			break;
		case CompatibilityRating::Nothing:
			name = TRANSLATE_NOOP("GameList", "Nothing");
			break;
		case CompatibilityRating::Intro:
			name = TRANSLATE_NOOP("GameList", "Intro");
			break;
		case CompatibilityRating::Menu:
			name = TRANSLATE_NOOP("GameList", "Menu");
			break;
		case CompatibilityRating::InGame:
			name = TRANSLATE_NOOP("GameList", "In-Game");
			break;
		case CompatibilityRating::Playable:
			name = TRANSLATE_NOOP("GameList", "Playable");
			break;
		case CompatibilityRating::Perfect:
			name = TRANSLATE_NOOP("GameList", "Perfect");
			break;
		default:
			return "";
	}

	if (translate)
		name = TRANSLATE("GameList", name);

	return name;
}

bool GameList::IsScannableFilename(const std::string_view path)
{
	return VMManager::IsDiscFileName(path) || VMManager::IsElfFileName(path) || VMManager::IsArcadeGameFileName(path);
}

void GameList::FillBootParametersForEntry(VMBootParameters* params, const Entry* entry)
{
	if (entry->type == GameList::EntryType::PS1Disc || entry->type == GameList::EntryType::PS2Disc)
	{
		params->filename = entry->path;
		params->source_type = CDVD_SourceType::Iso;
		params->elf_override.clear();
	}
	else if (entry->type == GameList::EntryType::ELF)
	{
		params->filename = VMManager::GetDiscOverrideFromGameSettings(entry->path);
		params->source_type = params->filename.empty() ? CDVD_SourceType::NoDisc : CDVD_SourceType::Iso;
		params->elf_override = entry->path;
	}
	else if (entry->type == GameList::EntryType::Arcade)
	{
		// The .acgame names the board's files itself. A source type would take the boot away from the arcade
		// branch of VMManager::Initialize, which only looks at the file name when none is given.
		params->filename = entry->path;
		params->source_type.reset();
		params->elf_override.clear();
	}
	else
	{
		params->filename.clear();
		params->source_type = CDVD_SourceType::NoDisc;
		params->elf_override.clear();
	}
}

bool GameList::GetIsoSerialAndCRC(const std::string& path, s32* disc_type, std::string* serial, u32* crc)
{
	Error error;

	// This isn't great, we really want to make it all thread-local...
	CDVD = &CDVDapi_Iso;
	if (!CDVD->open(path, &error))
	{
		Console.Error(fmt::format("(GameList::GetIsoSerialAndCRC) CDVD open of '{}' failed: {}", path, error.GetDescription()));
		return false;
	}

	// TODO: we could include the version in the game list?
	*disc_type = DoCDVDdetectDiskType();
	cdvdGetDiscInfo(serial, nullptr, nullptr, nullptr, crc, nullptr);
	DoCDVDclose();
	return true;
}

bool GameList::GetElfListEntry(const std::string& path, GameList::Entry* entry)
{
	ElfObject eo;
	if (!eo.OpenFile(path, false, nullptr))
	{
		Console.Error("Failed to parse ELF '%s'", path.c_str());
		return false;
	}

	entry->path = path;
	entry->serial.clear();
	entry->title = Path::GetFileTitle(path);
	entry->region = Region::Other;
	entry->type = EntryType::ELF;
	entry->compatibility_rating = CompatibilityRating::Unknown;
	entry->crc = eo.GetCRC();
	entry->total_size = eo.GetSize();

	std::string disc_path(VMManager::GetDiscOverrideFromGameSettings(path));
	if (!disc_path.empty())
	{
		s32 disc_type;
		u32 disc_crc;
		if (GetIsoSerialAndCRC(disc_path, &disc_type, &entry->serial, &disc_crc))
		{
			// use serial/region/compat info from the db
			if (const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(entry->serial))
			{
				entry->compatibility_rating = db_entry->compat;
				entry->region = ParseDatabaseRegion(db_entry->region);
			}
		}
	}

	return true;
}

bool GameList::GetArcadeListEntry(const std::string& path, GameList::Entry* entry)
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd))
		return false;

	// An .acgame, or an image kept as a file of its own with the game's ID in its name.
	const std::optional<Arcade::AcGameInfo> game =
		VMManager::IsArcadeGameFileName(path) ? Arcade::ReadAcGameInfo(path) : Arcade::ReadLooseImageInfo(path);
	if (!game.has_value())
	{
		// A file that is no arcade game is cached as invalid, so it is not read again on every scan.
		entry->type = EntryType::Invalid;
		entry->path = path;
		entry->total_size = 0;
		entry->compatibility_rating = CompatibilityRating::Unknown;
		entry->title.clear();
		entry->region = Region::Other;
		return true;
	}

	entry->type = EntryType::Arcade;
	entry->path = path;
	entry->serial = game->gameid;
	entry->crc = 0;
	entry->compatibility_rating = CompatibilityRating::Unknown;
	entry->region = Region::Other;

	// The .acgame's own name for the game comes first, then the game database's.
	const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(game->gameid);
	if (db_entry)
	{
		entry->compatibility_rating = db_entry->compat;
		entry->region = ParseDatabaseRegion(db_entry->region);
	}
	if (!game->name.empty())
	{
		entry->title = game->name;
	}
	else if (db_entry && !db_entry->name.empty())
	{
		entry->title = db_entry->name;
		entry->title_sort = db_entry->name_sort;
		entry->title_en = db_entry->name_en;
	}
	else
	{
		entry->title = Path::GetFileTitle(path);
	}

	// A game is as big as its media image; 0 when that is not there (yet). It is listed all the same, and booting it
	// says what is missing.
	FILESYSTEM_STAT_DATA media_stat;
	entry->total_size = FileSystem::StatFile(game->media_path.c_str(), &media_stat) ? static_cast<u64>(media_stat.Size) : 0;
	return true;
}

GameList::Region GameList::ParseDatabaseRegion(const std::string_view db_region)
{
	// clang-format off
						////// NTSC //////
						//////////////////
	if (db_region.starts_with("NTSC-B"))
		return Region::NTSC_B;
	else if (db_region.starts_with("NTSC-C"))
		return Region::NTSC_C;
	else if (db_region.starts_with("NTSC-HK"))
		return Region::NTSC_HK;
	else if (db_region.starts_with("NTSC-J"))
		return Region::NTSC_J;
	else if (db_region.starts_with("NTSC-K"))
		return Region::NTSC_K;
	else if (db_region.starts_with("NTSC-T"))
		return Region::NTSC_T;
	else if (db_region.starts_with("NTSC-U"))
		return Region::NTSC_U;
						////// PAL //////
						//////////////////
	else if (db_region.starts_with("PAL-AF"))
		return Region::PAL_AF;
	else if (db_region.starts_with("PAL-AU"))
		return Region::PAL_AU;
	else if (db_region.starts_with("PAL-A"))
		return Region::PAL_A;
	else if (db_region.starts_with("PAL-BE"))
		return Region::PAL_BE;
	else if (db_region.starts_with("PAL-E"))
		return Region::PAL_E;
	else if (db_region.starts_with("PAL-FI"))
		return Region::PAL_FI;
	else if (db_region.starts_with("PAL-F"))
		return Region::PAL_F;
	else if (db_region.starts_with("PAL-GR"))
		return Region::PAL_GR;
	else if (db_region.starts_with("PAL-G"))
		return Region::PAL_G;
	else if (db_region.starts_with("PAL-IN"))
		return Region::PAL_IN;
	else if (db_region.starts_with("PAL-I"))
		return Region::PAL_I;
	else if (db_region.starts_with("PAL-M"))
		return Region::PAL_M;
	else if (db_region.starts_with("PAL-NL"))
		return Region::PAL_NL;
	else if (db_region.starts_with("PAL-NO"))
		return Region::PAL_NO;
	else if (db_region.starts_with("PAL-PL"))
		return Region::PAL_PL;
	else if (db_region.starts_with("PAL-P"))
		return Region::PAL_P;
	else if (db_region.starts_with("PAL-R"))
		return Region::PAL_R;
	else if (db_region.starts_with("PAL-SC"))
		return Region::PAL_SC;
	else if (db_region.starts_with("PAL-SWI"))
		return Region::PAL_SWI;
	else if (db_region.starts_with("PAL-SW"))
		return Region::PAL_SW;
	else if (db_region.starts_with("PAL-S"))
		return Region::PAL_S;
	else if (db_region.starts_with("PAL-UK"))
		return Region::PAL_UK;
	else
		return Region::Other;
	// clang-format on
}

bool GameList::GetIsoListEntry(const std::string& path, GameList::Entry* entry)
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd))
		return false;

	s32 disc_type;
	if (!GetIsoSerialAndCRC(path, &disc_type, &entry->serial, &entry->crc))
		return false;

	switch (disc_type)
	{
		case CDVD_TYPE_PSCD:
		case CDVD_TYPE_PSCDDA:
			entry->type = EntryType::PS1Disc;
			break;

		case CDVD_TYPE_PS2CD:
		case CDVD_TYPE_PS2CDDA:
		case CDVD_TYPE_PS2DVD:
			entry->type = EntryType::PS2Disc;
			break;

		case CDVD_TYPE_ILLEGAL:
		default:
		{
			// Create empty invalid entry, so we don't repeatedly scan it every time.
			entry->type = EntryType::Invalid;
			entry->path = path;
			entry->total_size = 0;
			entry->compatibility_rating = CompatibilityRating::Unknown;
			entry->title.clear();
			entry->region = Region::Other;
			return true;
		}
	}

	entry->path = path;
	entry->total_size = sd.Size;
	entry->compatibility_rating = CompatibilityRating::Unknown;

	if (const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(entry->serial))
	{
		entry->title = std::move(db_entry->name);
		entry->title_sort = std::move(db_entry->name_sort);
		entry->title_en = std::move(db_entry->name_en);
		entry->disc_set = db_entry->disc_set;
		entry->compatibility_rating = db_entry->compat;
		entry->region = ParseDatabaseRegion(db_entry->region);
	}
	else
	{
		entry->title = Path::GetFileTitle(path);
		entry->region = Region::Other;
	}

	return true;
}

bool GameList::PopulateEntryFromPath(const std::string& path, GameList::Entry* entry)
{
	if (VMManager::IsElfFileName(path.c_str()))
		return GetElfListEntry(path, entry);
	else if (VMManager::IsArcadeGameFileName(path) || !Arcade::LooseImageGameId(path).empty())
		return GetArcadeListEntry(path, entry);
	else
		return GetIsoListEntry(path, entry);
}

bool GameList::GetGameListEntryFromCache(const std::string& path, GameList::Entry* entry)
{
	auto iter = s_cache_map.find(path);
	if (iter == s_cache_map.end())
		return false;

	*entry = std::move(iter->second);
	s_cache_map.erase(iter);
	return true;
}

static bool ReadString(std::FILE* stream, std::string* dest)
{
	u32 size;
	if (std::fread(&size, sizeof(size), 1, stream) != 1)
		return false;

	dest->resize(size);
	if (size > 0 && std::fread(dest->data(), size, 1, stream) != 1)
		return false;

	return true;
}

static bool ReadU8(std::FILE* stream, u8* dest)
{
	return std::fread(dest, sizeof(u8), 1, stream) > 0;
}

static bool ReadU32(std::FILE* stream, u32* dest)
{
	return std::fread(dest, sizeof(u32), 1, stream) > 0;
}

static bool ReadU64(std::FILE* stream, u64* dest)
{
	return std::fread(dest, sizeof(u64), 1, stream) > 0;
}

static bool WriteString(std::FILE* stream, const std::string& str)
{
	const u32 size = static_cast<u32>(str.size());
	return (std::fwrite(&size, sizeof(size), 1, stream) > 0 && (size == 0 || std::fwrite(str.data(), size, 1, stream) > 0));
}

static bool WriteU8(std::FILE* stream, u8 dest)
{
	return std::fwrite(&dest, sizeof(u8), 1, stream) > 0;
}

static bool WriteU32(std::FILE* stream, u32 dest)
{
	return std::fwrite(&dest, sizeof(u32), 1, stream) > 0;
}

static bool WriteU64(std::FILE* stream, u64 dest)
{
	return std::fwrite(&dest, sizeof(u64), 1, stream) > 0;
}

bool GameList::LoadEntriesFromCache(std::FILE* stream)
{
	u32 file_signature, file_version;
	s64 start_pos, file_size;
	if (!ReadU32(stream, &file_signature) || !ReadU32(stream, &file_version) || file_signature != GAME_LIST_CACHE_SIGNATURE ||
		file_version != GAME_LIST_CACHE_VERSION || (start_pos = FileSystem::FTell64(stream)) < 0 ||
		FileSystem::FSeek64(stream, 0, SEEK_END) != 0 || (file_size = FileSystem::FTell64(stream)) < 0 ||
		FileSystem::FSeek64(stream, start_pos, SEEK_SET) != 0)
	{
		Console.Warning("Game list cache is corrupted");
		return false;
	}

	while (FileSystem::FTell64(stream) != file_size)
	{
		std::string path;
		GameList::Entry ge;

		u8 type;
		u8 region;
		u8 compatibility_rating;
		u64 last_modified_time;

		if (!ReadString(stream, &path) || !ReadString(stream, &ge.serial) || !ReadString(stream, &ge.title) || !ReadString(stream, &ge.title_sort) ||
			!ReadString(stream, &ge.title_en) || !ReadU8(stream, &type) || !ReadU8(stream, &region) || !ReadU64(stream, &ge.total_size) ||
			!ReadU64(stream, &last_modified_time) || !ReadU32(stream, &ge.crc) || !ReadU8(stream, &compatibility_rating) ||
			region >= static_cast<u8>(Region::Count) || type >= static_cast<u8>(EntryType::Count) ||
			compatibility_rating > static_cast<u8>(CompatibilityRating::Perfect))
		{
			Console.Warning("Game list cache entry is corrupted");
			return false;
		}

		ge.path = path;
		ge.region = static_cast<Region>(region);
		ge.type = static_cast<EntryType>(type);
		ge.compatibility_rating = static_cast<CompatibilityRating>(compatibility_rating);
		ge.last_modified_time = static_cast<std::time_t>(last_modified_time);

		auto iter = s_cache_map.find(ge.path);
		if (iter != s_cache_map.end())
			iter->second = std::move(ge);
		else
			s_cache_map.emplace(std::move(path), std::move(ge));
	}

	return true;
}

static std::string GetCacheFilename()
{
	return Path::Combine(EmuFolders::Cache, "gamelist.cache");
}

void GameList::LoadCache()
{
	const std::string cache_filename(GetCacheFilename());
	auto stream = FileSystem::OpenManagedCFile(cache_filename.c_str(), "rb");
	if (!stream)
		return;

	if (!LoadEntriesFromCache(stream.get()))
	{
		Console.Warning("Deleting corrupted cache file '%s'", cache_filename.c_str());
		stream.reset();
		s_cache_map.clear();
		DeleteCacheFile();
		return;
	}
}

bool GameList::OpenCacheForWriting()
{
	const std::string cache_filename(GetCacheFilename());
	if (cache_filename.empty())
		return false;

	pxAssert(!s_cache_write_stream);
	s_cache_write_stream = FileSystem::OpenCFile(cache_filename.c_str(), "r+b");
	if (s_cache_write_stream)
	{
		// check the header
		u32 signature, version;
		if (ReadU32(s_cache_write_stream, &signature) && signature == GAME_LIST_CACHE_SIGNATURE &&
			ReadU32(s_cache_write_stream, &version) && version == GAME_LIST_CACHE_VERSION &&
			FileSystem::FSeek64(s_cache_write_stream, 0, SEEK_END) == 0)
		{
			return true;
		}

		std::fclose(s_cache_write_stream);
	}

	Console.WriteLn("Creating new game list cache file: '%s'", cache_filename.c_str());

	s_cache_write_stream = FileSystem::OpenCFile(cache_filename.c_str(), "w+b");
	if (!s_cache_write_stream)
		return false;


	// new cache file, write header
	if (!WriteU32(s_cache_write_stream, GAME_LIST_CACHE_SIGNATURE) || !WriteU32(s_cache_write_stream, GAME_LIST_CACHE_VERSION))
	{
		Console.Error("Failed to write game list cache header");
		std::fclose(s_cache_write_stream);
		s_cache_write_stream = nullptr;
		FileSystem::DeleteFilePath(cache_filename.c_str());
		return false;
	}

	return true;
}

bool GameList::WriteEntryToCache(const Entry* entry)
{
	bool result = true;
	result &= WriteString(s_cache_write_stream, entry->path);
	result &= WriteString(s_cache_write_stream, entry->serial);
	result &= WriteString(s_cache_write_stream, entry->title);
	result &= WriteString(s_cache_write_stream, entry->title_sort);
	result &= WriteString(s_cache_write_stream, entry->title_en);
	result &= WriteU8(s_cache_write_stream, static_cast<u8>(entry->type));
	result &= WriteU8(s_cache_write_stream, static_cast<u8>(entry->region));
	result &= WriteU64(s_cache_write_stream, entry->total_size);
	result &= WriteU64(s_cache_write_stream, static_cast<u64>(entry->last_modified_time));
	result &= WriteU32(s_cache_write_stream, entry->crc);
	result &= WriteU8(s_cache_write_stream, static_cast<u8>(entry->compatibility_rating));

	// flush after each entry, that way we don't end up with a corrupted file if we crash scanning.
	if (result)
		result = (std::fflush(s_cache_write_stream) == 0);

	return result;
}

void GameList::CloseCacheFileStream()
{
	if (!s_cache_write_stream)
		return;

	std::fclose(s_cache_write_stream);
	s_cache_write_stream = nullptr;
}

void GameList::DeleteCacheFile()
{
	pxAssert(!s_cache_write_stream);

	const std::string cache_filename(GetCacheFilename());
	if (cache_filename.empty() || !FileSystem::FileExists(cache_filename.c_str()))
		return;

	if (FileSystem::DeleteFilePath(cache_filename.c_str()))
		Console.WriteLn("Deleted game list cache '%s'", cache_filename.c_str());
	else
		Console.Warning("Failed to delete game list cache '%s'", cache_filename.c_str());
}

void GameList::RewriteCacheFile()
{
	CloseCacheFileStream();
	DeleteCacheFile();

	if (OpenCacheForWriting())
	{
		for (const GameList::Entry& entry : s_entries)
			WriteEntryToCache(&entry);

		CloseCacheFileStream();
	}
}

static bool IsPathExcluded(const std::vector<std::string>& excluded_paths, const std::string& path)
{
	return std::find_if(excluded_paths.begin(), excluded_paths.end(), [&path](const std::string& entry) { return !entry.empty() && path.starts_with(entry); }) != excluded_paths.end();
}

void GameList::ScanDirectory(const char* path, bool recursive, bool only_cache, const std::vector<std::string>& excluded_paths,
	const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini, ProgressCallback* progress)
{
	Console.WriteLn("Scanning %s%s", path, recursive ? " (recursively)" : "");

	progress->PushState();

	if (recursive)
		progress->SetStatusText(
			fmt::format(TRANSLATE_FS("GameList", "Scanning directory {} (recursively)..."), path).c_str());
	else
		progress->SetStatusText(
			fmt::format(TRANSLATE_FS("GameList", "Scanning directory {}..."), path).c_str());

	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(path, "*",
		recursive ? (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE) :
					(FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES),
		&files, progress);

	u32 files_scanned = 0;
	progress->SetProgressRange(static_cast<u32>(files.size()));
	progress->SetProgressValue(0);

	// An arcade game's boot program, image and dongle are parts of a game, not discs or ELFs of their own. The one
	// exception is the image of a game with no .acgame, which is the game.
	const Arcade::SetClaims arcade_claims(files);

	for (FILESYSTEM_FIND_DATA& ffd : files)
	{
		files_scanned++;

		if (progress->IsCancelled() || !GameList::IsScannableFilename(ffd.FileName) || IsPathExcluded(excluded_paths, ffd.FileName) ||
			(arcade_claims.Owns(ffd.FileName) && !arcade_claims.IsLooseImage(ffd.FileName)))
		{
			continue;
		}

		std::unique_lock lock(s_mutex);
		if (GetEntryForPath(ffd.FileName.c_str()) || AddFileFromCache(ffd.FileName, ffd.ModificationTime, played_time_map) || only_cache)
		{
			continue;
		}

		const std::string_view filename = Path::GetFileName(ffd.FileName);
		progress->SetStatusText(fmt::format(TRANSLATE_FS("GameList", "Scanning {}..."), filename.data()).c_str());
		ScanFile(std::move(ffd.FileName), ffd.ModificationTime, lock, played_time_map, custom_attributes_ini);
		progress->SetProgressValue(files_scanned);
	}

	progress->SetProgressValue(files_scanned);
	progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map)
{
	Entry entry;
	if (!GetGameListEntryFromCache(path, &entry) || entry.last_modified_time != timestamp)
		return false;

	// Skip over invalid entries.
	if (entry.type == EntryType::Invalid)
		return true;
	if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(entry.serial))
		entry.disc_set = game->disc_set;

	auto iter = played_time_map.find(entry.serial);
	if (iter != played_time_map.end())
	{
		entry.last_played_time = iter->second.last_played_time;
		entry.total_played_time = iter->second.total_played_time;
	}

	s_entries.push_back(std::move(entry));
	s_disc_sets_dirty = true;
	return true;
}

bool GameList::ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
	const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini)
{
	// don't block UI while scanning
	lock.unlock();

	DevCon.WriteLn("Scanning '%s'...", path.c_str());

	Entry entry;
	if (!PopulateEntryFromPath(path, &entry))
		return false;

	entry.last_modified_time = timestamp;

	if (s_cache_write_stream || OpenCacheForWriting())
	{
		if (!WriteEntryToCache(&entry))
			Console.Warning("Failed to write entry '%s' to cache", entry.path.c_str());
	}

	if (entry.type == EntryType::Invalid)
	{
		// don't add invalid entries to list
		lock.lock();
		return true;
	}

	const auto iter = played_time_map.find(entry.serial);
	if (iter != played_time_map.end())
	{
		entry.last_played_time = iter->second.last_played_time;
		entry.total_played_time = iter->second.total_played_time;
	}

	auto custom_title = custom_attributes_ini.GetOptionalStringValue(EncodeIniKey(entry.path).c_str(), "Title");
	if (custom_title)
	{
		entry.title = std::move(custom_title.value());
	}
	const auto custom_region = custom_attributes_ini.GetOptionalIntValue(EncodeIniKey(entry.path).c_str(), "Region");
	if (custom_region)
	{
		const int custom_region_value = custom_region.value();
		if (custom_region_value >= 0 && custom_region_value < static_cast<int>(Region::Count))
		{
			entry.region = static_cast<Region>(custom_region_value);
		}
	}

	lock.lock();

	// remove if present
	auto it = std::find_if(
		s_entries.begin(), s_entries.end(), [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
	if (it != s_entries.end())
		s_entries.erase(it);

	s_entries.push_back(std::move(entry));
	s_disc_sets_dirty = true;
	return true;
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
	return std::unique_lock<std::recursive_mutex>(s_mutex);
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
	return (index < s_entries.size()) ? &s_entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(const char* path)
{
	const size_t path_length = std::strlen(path);
	for (const Entry& entry : s_entries)
	{
		if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
			return &entry;
	}

	return nullptr;
}

const GameList::Entry* GameList::GetEntryByCRC(u32 crc)
{
	for (const Entry& entry : s_entries)
	{
		if (entry.crc == crc)
			return &entry;
	}

	return nullptr;
}

const GameList::Entry* GameList::GetEntryBySerialAndCRC(const std::string_view serial, u32 crc)
{
	for (const Entry& entry : s_entries)
	{
		if (entry.crc == crc && StringUtil::compareNoCase(entry.serial, serial))
			return &entry;
	}

	return nullptr;
}

u32 GameList::GetEntryCount()
{
	return static_cast<u32>(s_entries.size());
}

void GameList::Refresh(bool invalidate_cache, bool only_cache, ProgressCallback* progress /* = nullptr */)
{
	if (!progress)
		progress = ProgressCallback::NullProgressCallback;

	Error cdvd_lock_error;
	if (!cdvdLock(&cdvd_lock_error))
	{
		progress->DisplayError(cdvd_lock_error.GetDescription().c_str());
		return;
	}

	ScopedGuard unlock_cdvd = &cdvdUnlock;

	if (invalidate_cache)
		DeleteCacheFile();
	else
		LoadCache();

	// don't delete the old entries, since the frontend might still access them
	std::vector<Entry> old_entries;
	{
		std::unique_lock lock(s_mutex);
		old_entries.swap(s_entries);
		s_disc_sets_dirty = true;
	}

	const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
	const std::vector<std::string> dirs(Host::GetBaseStringListSetting("GameList", "Paths"));
	const std::vector<std::string> recursive_dirs(Host::GetBaseStringListSetting("GameList", "RecursivePaths"));
	const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));
	INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
	custom_attributes_ini.Load();

	if (!dirs.empty() || !recursive_dirs.empty())
	{
		progress->SetProgressRange(static_cast<u32>(dirs.size() + recursive_dirs.size()));
		progress->SetProgressValue(0);

		// we manually count it here, because otherwise pop state updates it itself
		int directory_counter = 0;
		for (const std::string& dir : dirs)
		{
			if (progress->IsCancelled())
				break;

			ScanDirectory(dir.c_str(), false, only_cache, excluded_paths, played_time, custom_attributes_ini, progress);
			progress->SetProgressValue(++directory_counter);
		}
		for (const std::string& dir : recursive_dirs)
		{
			if (progress->IsCancelled())
				break;

			ScanDirectory(dir.c_str(), true, only_cache, excluded_paths, played_time, custom_attributes_ini, progress);
			progress->SetProgressValue(++directory_counter);
		}
	}

	// don't need unused cache entries
	CloseCacheFileStream();
	s_cache_map.clear();
}

bool GameList::RescanPath(const std::string& path)
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd))
		return false;

	std::unique_lock lock(s_mutex);

	const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));
	INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
	custom_attributes_ini.Load();

	{
		// cancel if excluded
		const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
		if (IsPathExcluded(excluded_paths, path))
			return false;
	}

	// re-scan!
	if (!ScanFile(path, sd.ModificationTime, lock, played_time, custom_attributes_ini))
		return true;

	// update cache.. this is far from ideal, but since everything's variable length, all we can do.
	RewriteCacheFile();
	return true;
}

bool GameList::GetSerialAndCRCForFilename(const char* filename, std::string* serial, u32* crc)
{
	if (const GameList::Entry* entry = GetEntryForPath(filename); entry)
	{
		*serial = entry->serial;
		*crc = entry->crc;
		return true;
	}

	// Just scan it.. hopefully it'll come back okay.
	GameList::Entry temp_entry;
	if (GameList::PopulateEntryFromPath(filename, &temp_entry))
	{
		*serial = std::move(temp_entry.serial);
		*crc = temp_entry.crc;
		return true;
	}

	return false;
}

std::string GameList::GetPlayedTimeFile()
{
	return Path::Combine(EmuFolders::Settings, "playtime.dat");
}

bool GameList::ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry)
{
	size_t len = std::strlen(line);
	if (len != (PLAYED_TIME_LINE_LENGTH + 1)) // \n
	{
		Console.Warning("(ParsePlayedTimeLine) Malformed line: '%s'", line);
		return false;
	}

	const std::string_view serial_tok(StringUtil::StripWhitespace(std::string_view(line, PLAYED_TIME_SERIAL_LENGTH)));
	const std::string_view total_played_time_tok(
		StringUtil::StripWhitespace(std::string_view(line + PLAYED_TIME_SERIAL_LENGTH + 1, PLAYED_TIME_LAST_TIME_LENGTH)));
	const std::string_view last_played_time_tok(StringUtil::StripWhitespace(
		std::string_view(line + PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1, PLAYED_TIME_TOTAL_TIME_LENGTH)));

	const std::optional<u64> total_played_time(StringUtil::FromChars<u64>(total_played_time_tok));
	const std::optional<u64> last_played_time(StringUtil::FromChars<u64>(last_played_time_tok));
	if (serial_tok.empty() || !last_played_time.has_value() || !total_played_time.has_value())
	{
		Console.Warning("(ParsePlayedTimeLine) Malformed line: '%s'", line);
		return false;
	}

	serial = serial_tok;
	entry.last_played_time = static_cast<std::time_t>(last_played_time.value());
	entry.total_played_time = static_cast<std::time_t>(total_played_time.value());
	return true;
}

std::string GameList::MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry)
{
	return fmt::format("{:<{}} {:<{}} {:<{}}\n", serial, static_cast<unsigned>(PLAYED_TIME_SERIAL_LENGTH), entry.total_played_time,
		static_cast<unsigned>(PLAYED_TIME_TOTAL_TIME_LENGTH), entry.last_played_time, static_cast<unsigned>(PLAYED_TIME_LAST_TIME_LENGTH));
}

GameList::PlayedTimeMap GameList::LoadPlayedTimeMap(const std::string& path)
{
	PlayedTimeMap ret;

	// Use write mode here, even though we're not writing, so we can lock the file from other updates.
	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");

#ifdef _WIN32
	// On Windows, the file is implicitly locked.
	while (!fp && GetLastError() == ERROR_SHARING_VIOLATION)
	{
		Sleep(10);
		fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");
	}
#endif

	if (fp)
	{
#ifndef _WIN32
		FileSystem::POSIXLock flock(fp.get());
#endif

		char line[256];
		while (std::fgets(line, sizeof(line), fp.get()))
		{
			std::string serial;
			PlayedTimeEntry entry;
			if (!ParsePlayedTimeLine(line, serial, entry))
				continue;

			if (ret.find(serial) != ret.end())
			{
				Console.Warning("(LoadPlayedTimeMap) Duplicate entry: '%s'", serial.c_str());
				continue;
			}

			ret.emplace(std::move(serial), entry);
		}
	}

	return ret;
}

GameList::PlayedTimeEntry GameList::UpdatePlayedTimeFile(
	const std::string& path, const std::string& serial, std::time_t last_time, std::time_t add_time)
{
	const PlayedTimeEntry new_entry{last_time, add_time};

	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");

#ifdef _WIN32
	// On Windows, the file is implicitly locked.
	while (!fp && GetLastError() == ERROR_SHARING_VIOLATION)
	{
		Sleep(10);
		fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");
	}
#endif

	// Doesn't exist? Create it.
	if (!fp && errno == ENOENT)
		fp = FileSystem::OpenManagedCFile(path.c_str(), "w+b");

	if (!fp)
	{
		Console.Error("Failed to open '%s' for update.", path.c_str());
		return new_entry;
	}

#ifndef _WIN32
	FileSystem::POSIXLock flock(fp.get());
#endif

	for (;;)
	{
		char line[256];
		const s64 line_pos = FileSystem::FTell64(fp.get());
		if (!std::fgets(line, sizeof(line), fp.get()))
			break;

		std::string line_serial;
		PlayedTimeEntry line_entry;
		if (!ParsePlayedTimeLine(line, line_serial, line_entry))
			continue;

		if (line_serial != serial)
			continue;

		// found it!
		line_entry.last_played_time = (last_time != 0) ? last_time : 0;
		line_entry.total_played_time = (last_time != 0) ? (line_entry.total_played_time + add_time) : 0;

		std::string new_line(MakePlayedTimeLine(serial, line_entry));
		if (FileSystem::FSeek64(fp.get(), line_pos, SEEK_SET) != 0 || std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1 ||
			std::fflush(fp.get()) != 0)
		{
			Console.Error("Failed to update '%s'.", path.c_str());
		}

		return line_entry;
	}

	if (last_time != 0)
	{
		// new entry.
		std::string new_line(MakePlayedTimeLine(serial, new_entry));
		if (FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0 || std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
		{
			Console.Error("Failed to write '%s'.", path.c_str());
		}
	}

	return new_entry;
}

void GameList::AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time)
{
	if (serial.empty())
		return;

	const PlayedTimeEntry pt(UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, last_time, add_time));
	Console.WriteLn("Add %u seconds play time to %s -> now %u", static_cast<unsigned>(add_time), serial.c_str(),
		static_cast<unsigned>(pt.total_played_time));

	std::unique_lock<std::recursive_mutex> lock(s_mutex);
	for (GameList::Entry& entry : s_entries)
	{
		if (entry.serial != serial)
			continue;

		entry.last_played_time = pt.last_played_time;
		entry.total_played_time = pt.total_played_time;
	}
}

void GameList::ClearPlayedTimeForSerial(const std::string& serial)
{
	if (serial.empty())
		return;

	UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, 0, 0);

	std::unique_lock<std::recursive_mutex> lock(s_mutex);
	for (GameList::Entry& entry : s_entries)
	{
		if (entry.serial != serial)
			continue;

		entry.last_played_time = 0;
		entry.total_played_time = 0;
	}
}


std::time_t GameList::GetPlayedTimeForSerial(const std::string& serial)
{
	const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));
	const auto it = played_time.find(serial);
	return (it != played_time.end()) ? it->second.total_played_time : 0;
}

std::time_t GameList::GetCachedPlayedTimeForSerial(const std::string& serial)
{
	if (serial.empty())
		return 0;

	std::unique_lock<std::recursive_mutex> lock(s_mutex);
	for (GameList::Entry& entry : s_entries)
	{
		if (entry.serial == serial)
			return entry.total_played_time;
	}

	return 0;
}

std::string GameList::FormatTimestamp(std::time_t timestamp)
{
	std::string ret;

	if (timestamp == 0)
	{
		ret = TRANSLATE_STR("GameList", "Never");
	}
	else
	{
		struct tm ctime = {};
		struct tm ttime = {};
		const std::time_t ctimestamp = std::time(nullptr);
#ifdef _MSC_VER
		localtime_s(&ctime, &ctimestamp);
		localtime_s(&ttime, &timestamp);
#else
		localtime_r(&ctimestamp, &ctime);
		localtime_r(&timestamp, &ttime);
#endif

		if (ctime.tm_year == ttime.tm_year && ctime.tm_yday == ttime.tm_yday)
		{
			ret = TRANSLATE_STR("GameList", "Today");
		}
		else if ((ctime.tm_year == ttime.tm_year && ctime.tm_yday == (ttime.tm_yday + 1)) ||
				 (ctime.tm_yday == 0 && (ctime.tm_year - 1) == ttime.tm_year))
		{
			ret = TRANSLATE_STR("GameList", "Yesterday");
		}
		else
		{
#ifdef _WIN32
			wchar_t buf[128];
			std::wcsftime(buf, std::size(buf), L"%x", &ttime);
			ret = StringUtil::WideStringToUTF8String(buf);
#else
			char buf[128];
			std::strftime(buf, std::size(buf), "%x", &ttime);
			ret.assign(buf);
#endif
		}
	}

	return ret;
}

std::string GameList::FormatTimespan(const std::time_t timespan, const bool long_format)
{
	const u32 hours = static_cast<u32>(timespan / 3600);
	const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
	const u32 seconds = static_cast<u32>((timespan % 3600) % 60);

	std::string ret;
	if (!long_format)
	{
		if (hours >= 100)
			ret = fmt::format(TRANSLATE_FS("GameList", "{}h {}m"), hours, minutes);
		else if (hours > 0)
			ret = fmt::format(TRANSLATE_FS("GameList", "{}h {}m {}s"), hours, minutes, seconds);
		else if (minutes > 0)
			ret = fmt::format(TRANSLATE_FS("GameList", "{}m {}s"), minutes, seconds);
		else if (seconds > 0)
			ret = fmt::format(TRANSLATE_FS("GameList", "{}s"), seconds);
		else
			ret = "None";
	}
	else
	{
		if (hours > 0)
			ret.assign(TRANSLATE_PLURAL_STR("GameList", "%n hours", "", hours));
		else if (minutes > 0)
			ret.assign(TRANSLATE_PLURAL_STR("GameList", "%n minutes", "", minutes));
		else
			ret.assign(TRANSLATE_PLURAL_STR("GameList", "%n seconds", "", seconds));
	}

	return ret;
}

std::string GameList::GetCoverImagePathForEntry(const Entry* entry)
{
	static const char* extensions[] = {".jpg", ".jpeg", ".png", ".webp"};

	// TODO(Stenzek): Port to filesystem...

	std::string cover_path;
	for (const char* extension : extensions)
	{

		// Prioritize file title since users can change these (e.g. for modded games)
		const std::string_view file_title(Path::GetFileTitle(entry->path));
		if (!file_title.empty() && entry->title != file_title)
		{
			std::string cover_filename = fmt::format("{}{}", file_title, extension);
			Path::SanitizeFileName(&cover_filename);

			cover_path = Path::Combine(EmuFolders::Covers, cover_filename);
			if (FileSystem::FileExists(cover_path.c_str()))
				return cover_path;
		}

		// Lookup by serial (most specific)
		if (!entry->serial.empty())
		{
			const std::string cover_filename(entry->serial + extension);
			cover_path = Path::Combine(EmuFolders::Covers, cover_filename);
			if (FileSystem::FileExists(cover_path.c_str()))
				return cover_path;
		}

		// Last resort, check the game title
		if (!entry->title.empty())
		{
			std::string cover_filename = fmt::format("{}{}", entry->title, extension);
			Path::SanitizeFileName(&cover_filename);

			cover_path = Path::Combine(EmuFolders::Covers, cover_filename);
			if (FileSystem::FileExists(cover_path.c_str()))
				return cover_path;
		}
		// EN title too
		if (!entry->title_en.empty())
		{
			std::string cover_filename = fmt::format("{}{}", entry->title_en, extension);
			Path::SanitizeFileName(&cover_filename);

			cover_path = Path::Combine(EmuFolders::Covers, cover_filename);
			if (FileSystem::FileExists(cover_path.c_str()))
				return cover_path;
		}
	}

	cover_path.clear();
	return cover_path;
}

std::string GameList::GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial)
{
	const std::string_view extension = Path::GetExtension(new_filename);
	if (extension.empty())
		return {};

	const std::string existing_filename = GetCoverImagePathForEntry(entry);
	if (!existing_filename.empty())
	{
		const std::string_view existing_extension = Path::GetExtension(existing_filename);
		if (!existing_extension.empty() && existing_extension == extension)
			return existing_filename;
	}

	std::string cover_filename = fmt::format("{}.{}", use_serial ? entry->serial : entry->title, extension);
	Path::SanitizeFileName(&cover_filename);
	return Path::Combine(EmuFolders::Covers, cover_filename);
}

bool GameList::DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial, ProgressCallback* progress,
	std::function<void(const Entry*, std::string)> save_callback)
{
	if (!progress)
		progress = ProgressCallback::NullProgressCallback;

	bool has_title = false;
	bool has_file_title = false;
	bool has_serial = false;
	for (const std::string& url_template : url_templates)
	{
		if (!has_title && url_template.find("${title}") != std::string::npos)
			has_title = true;
		if (!has_file_title && url_template.find("${filetitle}") != std::string::npos)
			has_file_title = true;
		if (!has_serial && url_template.find("${serial}") != std::string::npos)
			has_serial = true;
	}
	if (!has_title && !has_file_title && !has_serial)
	{
		progress->DisplayError("URL template must contain at least one of ${title}, ${filetitle}, or ${serial}.");
		return false;
	}

	if (!FileSystem::CreateDirectoryPath(EmuFolders::Covers.c_str(), false))
	{
		progress->DisplayError(fmt::format("Failed to create covers directory: {}", EmuFolders::Covers).c_str());
		return false;
	}

	std::vector<std::pair<std::string, std::string>> download_urls;
	{
		std::unique_lock lock(s_mutex);
		for (const GameList::Entry& entry : s_entries)
		{
			const std::string existing_path(GetCoverImagePathForEntry(&entry));
			if (!existing_path.empty())
				continue;

			for (const std::string& url_template : url_templates)
			{
				std::string url(url_template);
				if (has_title)
					StringUtil::ReplaceAll(&url, "${title}", Path::URLEncode(entry.title));
				if (has_file_title)
					StringUtil::ReplaceAll(&url, "${filetitle}", Path::URLEncode(Path::GetFileTitle(entry.path)));
				if (has_serial)
					StringUtil::ReplaceAll(&url, "${serial}", Path::URLEncode(entry.serial));

				download_urls.emplace_back(entry.path, std::move(url));
			}
		}
	}
	if (download_urls.empty())
	{
		progress->DisplayError("No URLs to download enumerated.");
		return false;
	}

	std::unique_ptr<HTTPDownloader> downloader(HTTPDownloader::Create(Host::GetHTTPUserAgent()));
	if (!downloader)
	{
		progress->DisplayError("Failed to create HTTP downloader.");
		return false;
	}

	progress->SetCancellable(true);
	progress->SetProgressRange(static_cast<u32>(download_urls.size()));

	for (auto& [entry_path, url] : download_urls)
	{
		if (progress->IsCancelled())
			break;

		// make sure it didn't get done already
		{
			std::unique_lock lock(s_mutex);
			const GameList::Entry* entry = GetEntryForPath(entry_path.c_str());
			if (!entry || !GetCoverImagePathForEntry(entry).empty())
			{
				progress->IncrementProgressValue();
				continue;
			}

			progress->SetStatusText(fmt::format(TRANSLATE_FS("GameList", "Downloading cover for {0} [{1}]..."), entry->title, entry->serial).c_str());
		}

		// we could actually do a few in parallel here...
		std::string filename = Path::URLDecode(url);
		downloader->CreateRequest(
			std::move(url), [use_serial, &save_callback, entry_path = std::move(entry_path), filename = std::move(filename)](
								s32 status_code, const std::string& content_type, HTTPDownloader::Request::Data data) {
				if (status_code != HTTPDownloader::HTTP_STATUS_OK || data.empty())
					return;

				std::unique_lock lock(s_mutex);
				const GameList::Entry* entry = GetEntryForPath(entry_path.c_str());
				if (!entry || !GetCoverImagePathForEntry(entry).empty())
					return;

				// prefer the content type from the response for the extension
				// otherwise, if it's missing, and the request didn't have an extension.. fall back to jpegs.
				std::string template_filename;
				std::string content_type_extension(HTTPDownloader::GetExtensionForContentType(content_type));

				// don't treat the domain name as an extension..
				const std::string::size_type last_slash = filename.find('/');
				const std::string::size_type last_dot = filename.find('.');
				if (!content_type_extension.empty())
					template_filename = fmt::format("cover.{}", content_type_extension);
				else if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash)
					template_filename = Path::GetFileName(filename);
				else
					template_filename = "cover.jpg";

				std::string write_path(GetNewCoverImagePathForEntry(entry, template_filename.c_str(), use_serial));
				if (write_path.empty())
					return;

				if (FileSystem::WriteBinaryFile(write_path.c_str(), data.data(), data.size()) && save_callback)
					save_callback(entry, std::move(write_path));
			});
		downloader->WaitForAllRequests();
		progress->IncrementProgressValue();
	}

	return true;
}

std::string GameList::GetCustomPropertiesFile()
{
	return Path::Combine(EmuFolders::Settings, "custom_properties.ini");
}

void GameList::CheckCustomAttributesForPath(const std::string& path, bool& has_custom_title, bool& has_custom_region)
{
	INISettingsInterface names(GetCustomPropertiesFile());
	if (names.Load())
	{
		has_custom_title = names.ContainsValue(EncodeIniKey(path).c_str(), "Title");
		has_custom_region = names.ContainsValue(EncodeIniKey(path).c_str(), "Region");
	}
}

void GameList::SaveCustomTitleForPath(const std::string& path, const std::string& custom_title)
{
	INISettingsInterface names(GetCustomPropertiesFile());
	names.Load();

	if (!custom_title.empty())
	{
		names.SetStringValue(EncodeIniKey(path).c_str(), "Title", custom_title.c_str());
	}
	else
	{
		names.DeleteValue(EncodeIniKey(path).c_str(), "Title");
	}

	if (names.Save())
	{
		// Let the cache update by rescanning
		RescanPath(path);
	}
}

void GameList::SaveCustomRegionForPath(const std::string& path, int custom_region)
{
	INISettingsInterface names(GetCustomPropertiesFile());
	names.Load();

	if (custom_region >= 0)
	{
		names.SetIntValue(EncodeIniKey(path).c_str(), "Region", custom_region);
	}
	else
	{
		names.DeleteValue(EncodeIniKey(path).c_str(), "Region");
	}

	if (names.Save())
	{
		// Let the cache update by rescanning
		RescanPath(path);
	}
}

std::string GameList::GetCustomTitleForPath(const std::string& path)
{
	std::string ret;

	std::unique_lock lock(s_mutex);
	const GameList::Entry* entry = GetEntryForPath(EncodeIniKey(path).c_str());
	if (entry)
	{
		ret = entry->title;
	}

	return ret;
}

static std::string GameList::EncodeIniKey(const std::string_view& input)
{
	std::string out;
	out.reserve(input.size());

	for (char c : input)
	{
		if (c == '[')
			out += "{{";
		else if (c == ']')
			out += "}}";
		else
			out += c;
	}

	return out;
}
