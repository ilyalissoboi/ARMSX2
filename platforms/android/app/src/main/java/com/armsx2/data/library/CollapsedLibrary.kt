// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.data.library

import com.armsx2.GameInfo
import com.armsx2.arcade.Arcade

/** One disc of a collapsed game, as the disc picker lists it. */
data class DiscChoice(val game: GameInfo, val number: Int)

/**
 * The library with each multi-disc game listed once ([com.armsx2.CollapseMultiDisc]).
 *
 * A set is listed as its first disc's entry, retitled to the set ("Xenosaga Episode III" rather
 * than "... [Disc 1]"). That entry keeps disc 1's URI, serial and cover, so anything keyed by
 * those (per-game settings, custom names, covers) still finds disc 1's. [discsOf] says which
 * entries stand for a set, and which discs to offer when one is launched.
 *
 * [discSet] looks a serial up in the GameDB; it is a parameter so the native lookup stays out of
 * this class.
 */
class CollapsedLibrary(source: List<GameInfo>, discSet: (String) -> DiscSets.Info?) {
    /** What the library lists: every game, with each set's members replaced by one entry. */
    val games: List<GameInfo>

    private val entryByMember = HashMap<String, GameInfo>()
    private val discsByEntry = HashMap<String, List<DiscChoice>>()

    init {
        val discs = source.map { it.toDisc(discSet) }
        val leaders = HashMap<Int, GameInfo>()
        for (group in DiscSets.build(discs)) {
            val first = source[group.members.first()]
            val entry = first.copy(title = group.title, titleEn = group.titleEn, titleSort = group.titleSort)
            leaders[group.members.first()] = entry
            discsByEntry[entry.uri.toString()] = group.members.mapIndexed { i, member -> DiscChoice(source[member], group.numbers[i]) }
            group.members.forEach { entryByMember[source[it].uri.toString()] = entry }
        }
        games = source.mapIndexedNotNull { i, game ->
            leaders[i] ?: game.takeIf { it.uri.toString() !in entryByMember }
        }
    }

    /** The discs [game] stands for, in disc order, or null when it is not a collapsed set. */
    fun discsOf(game: GameInfo): List<DiscChoice>? = discsByEntry[game.uri.toString()]

    /** The entry listed for [game]: its set's, when it is one disc of a set, otherwise itself. */
    fun entryFor(game: GameInfo): GameInfo = entryByMember[game.uri.toString()] ?: game

    /** Every disc [game] stands for: its set's discs, or just itself. */
    fun members(game: GameInfo): List<GameInfo> = discsOf(game)?.map { it.game } ?: listOf(game)

    private companion object {
        fun GameInfo.toDisc(discSet: (String) -> DiscSets.Info?): DiscSets.Disc {
            val elf = extension.equals("ELF", ignoreCase = true) || uri.toString().endsWith(".elf", ignoreCase = true)
            return DiscSets.Disc(
                key = uri.toString(),
                isDisc = !elf && extension != Arcade.BADGE,
                platform = platform.key,
                region = region,
                serial = serial,
                title = title,
                titleEn = titleEn,
                titleSort = titleSort,
                fileTitle = fileName?.substringBeforeLast('.').orEmpty(),
                set = serial?.takeIf { it.isNotBlank() }?.let(discSet),
            )
        }
    }
}
