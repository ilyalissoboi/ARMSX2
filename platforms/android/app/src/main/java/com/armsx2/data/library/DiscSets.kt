// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.data.library

import java.util.TreeSet

/**
 * Which library entries are discs of one multi-disc game, for "Collapse multi-disc games".
 *
 * A port of GameList::BuildDiscSets (pcsx2/GameList.cpp) to the Android library, which scans in
 * Kotlin and never fills a GameList::Entry. The same three rules decide membership, in order:
 *  1. The GameDB's `discSet` (Docs/GameIndex.md, "Library Disc Sets"), which names the set by its
 *     first disc's serial, so discs titled differently ("Cy Girls [Ice]" / "[Aska]") still pair.
 *  2. A "Disc N" / "CD 2 of 3" / "ディスク2" label in a title or, failing that, the file name, read
 *     with the label removed. Discs whose remaining titles match are one set, but only within one
 *     serial family (SLUS, SLPM, ...), region and set of release tags ("(Rev 1)", "(Demo)").
 *  3. A group survives only with at least two disc numbers and no number claimed by two serials:
 *     two releases each supplying a Disc 1 is ambiguous, and nothing is collapsed rather than
 *     pairing the wrong discs.
 *
 * Desktop also pairs images with an equal serial and executable CRC. The Android scan reads no
 * CRC, so that rule is not carried over: copies of a single-disc game stay separate entries, and
 * copies of one disc of a set join it through their label like any other disc.
 *
 * Grouping is a view of the library only. Every disc keeps its own serial, settings, save states
 * and launch path; the collapsed entry just asks which one to boot.
 */
object DiscSets {
    /** A GameDB `discSet`: the set's id (its first disc's serial) and name, and this disc's number.
     *  [discName] and [discNameEn] are the disc's own database titles, which identify per-disc
     *  labels in a file name ("[Ice]") as labels rather than as a different release. */
    data class Info(
        val id: String,
        val name: String,
        val nameEn: String,
        val number: Int,
        val discName: String,
        val discNameEn: String,
    )

    /** What the grouping needs to know about one library entry. */
    data class Disc(
        /** Identity used as the last sort key and in tests; the library passes the URI. */
        val key: String,
        /** False for ELFs and arcade games, which are never grouped. */
        val isDisc: Boolean,
        /** "ps1" or "ps2": a PS1 and a PS2 disc are never one game. */
        val platform: String,
        /** The library's region label ("USA", "Japan", "Europe", ...), or null. */
        val region: String?,
        val serial: String?,
        val title: String,
        val titleEn: String = "",
        val titleSort: String = "",
        /** The image's file name without its extension. */
        val fileTitle: String = "",
        val set: Info? = null,
    )

    /** One collapsed game: its [members] (indices into the input) in disc order, the disc number
     *  of each, and the titles the collapsed entry shows. */
    data class Group(
        val title: String,
        val titleEn: String,
        val titleSort: String,
        val members: List<Int>,
        val numbers: List<Int>,
    )

    /** Parses [kr.co.iefriends.pcsx2.NativeApp.getDiscSetForSerial]'s six newline-separated fields. */
    fun parseInfo(raw: String?): Info? {
        if (raw.isNullOrEmpty()) return null
        val parts = raw.split('\n')
        val id = parts.getOrNull(0).orEmpty()
        val name = parts.getOrNull(1).orEmpty()
        val number = parts.getOrNull(3)?.toIntOrNull() ?: 0
        if (id.isEmpty() || name.isEmpty() || number !in 1..99) return null
        return Info(
            id = id.uppercase(),
            name = name,
            nameEn = parts.getOrNull(2).orEmpty(),
            number = number,
            discName = parts.getOrNull(4).orEmpty(),
            discNameEn = parts.getOrNull(5).orEmpty(),
        )
    }

    /** This disc's number: the database's, else a label in its titles, else in its file name.
     *  Null when there is none, or when its two titles disagree. */
    fun discNumber(disc: Disc): Int? {
        disc.set?.let { return it.number }
        var number: Int? = null
        for (title in listOf(disc.title, disc.titleEn)) {
            val parsed = parseDiscTitle(title) ?: continue
            if (number != null && number != parsed.number) return null
            number = parsed.number
        }
        return number ?: parseDiscTitle(disc.fileTitle)?.number
    }

    /** [title] without its disc label, or [title] itself when it has none. */
    fun setTitle(title: String): String = parseDiscTitle(title)?.title ?: title

    fun build(discs: List<Disc>): List<Group> {
        val matches = Union(discs.size)
        val numbers = IntArray(discs.size)
        val knownIds = arrayOfNulls<String>(discs.size)
        val byAlias = HashMap<String, MutableList<Int>>()
        val bySetId = HashMap<String, Int>()

        discs.forEachIndexed { i, disc ->
            if (!disc.isDisc) return@forEachIndexed
            numbers[i] = discNumber(disc) ?: 0
            val release = "${disc.platform}\n${regionKey(disc.region)}\n${releaseQualifiers(disc)}"
            val serial = disc.serial.orEmpty()
            val family = serial.substringBefore('-').uppercase()
            val names = LinkedHashSet<String>()
            val set = disc.set
            if (set != null) {
                knownIds[i] = set.id.uppercase()
                val other = bySetId.putIfAbsent(release + '\n' + knownIds[i], i)
                if (other != null) matches.join(i, other)
            } else {
                if (numbers[i] != 0) {
                    for (title in listOf(disc.title, disc.titleEn))
                        parseDiscTitle(title)?.let { names += matchingKey(it.title) }
                }
                if (numbers[i] != 0 && names.isEmpty()) {
                    parseDiscTitle(disc.fileTitle)?.let { parsed ->
                        numbers[i] = parsed.number
                        names += matchingKey(parsed.title)
                        if (disc.title.isNotEmpty()) names += matchingKey(disc.title)
                    }
                }
                // Issue numbers on standalone demo/sampler discs are not disc-set ordinals.
                if (names.any { "demo" in it || "sampler" in it || "magazine" in it }) names.clear()
            }
            for (name in names) {
                if (name.isNotEmpty()) byAlias.getOrPut("$release\n$family\n$name") { mutableListOf() } += i
            }
        }

        for (members in byAlias.values) members.forEach { matches.join(members.first(), it) }

        val groups = sortedMapOf<Int, MutableList<Int>>()
        discs.indices.filter { discs[it].isDisc }.forEach { groups.getOrPut(matches.find(it)) { mutableListOf() } += it }

        val sets = mutableListOf<Group>()
        for (members in groups.values) {
            if (members.size < 2) continue
            val ordinals = members.map { numbers[it] }.filter { it != 0 }.toSet()
            val serialsByNumber = HashMap<Int, MutableSet<String>>()
            members.forEach { i ->
                val serial = discs[i].serial.orEmpty().uppercase()
                if (numbers[i] != 0 && serial.isNotEmpty()) serialsByNumber.getOrPut(numbers[i]) { HashSet() } += serial
            }
            // More than one release supplies the same ordinal; naming alone is ambiguous.
            if (serialsByNumber.values.any { it.size > 1 }) continue
            if (ordinals.size < 2) continue
            val sorted = members.sortedWith(
                compareBy<Int> { numbers[it] }.thenBy { discs[it].serial.orEmpty() }.thenBy { discs[it].key },
            )
            var title = ""
            var titleEn = ""
            var titleSort = ""
            for (i in sorted) {
                val disc = discs[i]
                val set = disc.set
                if (title.isEmpty()) title = set?.name ?: setTitle(disc.title)
                if (titleEn.isEmpty()) titleEn = set?.nameEn ?: setTitle(disc.titleEn)
                if (titleSort.isEmpty()) titleSort = set?.name ?: setTitle(disc.titleSort)
            }
            sets += Group(title, titleEn, titleSort, sorted, sorted.map { numbers[it] })
        }
        return sets
    }

    internal data class NumberedTitle(val title: String, val number: Int)

    private val labels = listOf("disc", "disk", "dvd", "cd", "ディスク", "光盘", "光碟", "디스크")
    private val roman = listOf(
        "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X",
        "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX",
    )

    /** "Game (Disc 2)" -> ("Game", 2). Also "[Disc 1 of 2]", "CD-II", "Disk 3/3", "ディスク2"; a label
     *  must start a word and be followed by a number of 1..99 or a roman numeral up to XX. */
    internal fun parseDiscTitle(title: String): NumberedTitle? {
        val text = normalize(title)
        for (pos in text.indices) {
            if (pos > 0 && !isSeparator(text[pos - 1]) && text[pos - 1] != '[' && text[pos - 1] != '(') continue
            for (label in labels) {
                if (!text.regionMatches(pos, label, 0, label.length, ignoreCase = true)) continue
                var start = pos
                while (start > 0 && isSpace(text[start - 1])) start--
                var close = 0.toChar()
                if (start > 0 && (text[start - 1] == '[' || text[start - 1] == '(')) {
                    start--
                    close = if (text[start] == '[') ']' else ')'
                }
                var end = pos + label.length
                while (end < text.length && isSeparator(text[end])) end++
                val (number, numberEnd) = readNumber(text, end) ?: continue
                end = numberEnd
                while (end < text.length && isSpace(text[end])) end++
                if (text.regionMatches(end, "of", 0, 2, ignoreCase = true) || (end < text.length && text[end] == '/')) {
                    end += if (text[end] == '/') 1 else 2
                    while (end < text.length && isSpace(text[end])) end++
                    val (count, countEnd) = readNumber(text, end) ?: continue
                    if (count < number) continue
                    end = countEnd
                    while (end < text.length && isSpace(text[end])) end++
                } else if (close == 0.toChar()) {
                    end = numberEnd
                }
                if (close != 0.toChar()) {
                    if (end == text.length || text[end] != close) continue
                    end++
                }
                if (end < text.length && !isSpace(text[end]) && text[end] != '[' && text[end] != '(' && text[end] != '-') continue
                while (start > 0 && isSeparator(text[start - 1])) start--
                val suffix = text.substring(end).trim(::isSpace)
                val base = buildString {
                    append(text, 0, start)
                    if (isNotEmpty() && suffix.isNotEmpty()) append(' ')
                    append(suffix)
                }.trim(::isSpace)
                if (base.isNotEmpty()) return NumberedTitle(base, number)
            }
        }
        return null
    }

    /** The number at [offset] (decimal 1..99, or a roman numeral I..XX) and where it ends. */
    private fun readNumber(text: String, offset: Int): Pair<Int, Int>? {
        var end = offset
        while (end < text.length && text[end] in '0'..'9') end++
        if (end != offset) {
            val number = text.substring(offset, end).toIntOrNull() ?: return null
            return if (number in 1..99) number to end else null
        }
        while (end < text.length && text[end] in "IiVvXx") end++
        val index = roman.indexOfFirst { it.equals(text.substring(offset, end), ignoreCase = true) }
        return if (index >= 0) (index + 1) to end else null
    }

    /** Full-width ASCII, ideographic and no-break spaces, and dash variants as plain ASCII. */
    private fun normalize(text: String): String = buildString(text.length) {
        var i = 0
        while (i < text.length) {
            var ch = text.codePointAt(i)
            i += Character.charCount(ch)
            when (ch) {
                in 0xFF01..0xFF5E -> ch -= 0xFEE0
                0x3000, 0xA0 -> ch = ' '.code
                in 0x2010..0x2014 -> ch = '-'.code
            }
            appendCodePoint(ch)
        }
    }

    /** Lower-case ASCII words separated by single spaces; other scripts are kept as they are. */
    internal fun matchingKey(text: String): String = buildString {
        var space = false
        for (ch in normalize(text)) {
            if (ch.code < 0x80 && !ch.isLetterOrDigit()) {
                space = isNotEmpty()
                continue
            }
            if (space) append(' ')
            space = false
            append(if (ch.code < 0x80) ch.lowercaseChar() else ch)
        }
    }

    /** The bracketed tags in [title], as matching keys, leaving out disc labels ("(Disc 1)"). */
    private fun titleTags(title: String): Set<String> {
        val text = normalize(title)
        val tags = TreeSet<String>()
        var start = 0
        while (start < text.length) {
            val open = text[start]
            if (open == '[' || open == '(') {
                val end = text.indexOf(if (open == '[') ']' else ')', start + 1)
                if (end >= 0) {
                    val tag = text.substring(start, end + 1)
                    val disc = parseDiscTitle("Title $tag")
                    if (disc == null || disc.title != "Title") tags += matchingKey(tag)
                    start = end
                }
            }
            start++
        }
        return tags
    }

    /** The tags that tell this release apart from another of the same game: the file name's,
     *  less those its database titles already carry, less its own region. */
    private fun releaseQualifiers(disc: Disc): String {
        val tags = TreeSet(titleTags(disc.fileTitle))
        val set = disc.set
        // Database titles already carry edition information. Explicit metadata can also identify
        // per-disc labels such as Cy Girls' Ice/Aska, without stripping arbitrary filename tags.
        for (title in listOf(disc.title, disc.titleEn, set?.discName.orEmpty(), set?.discNameEn.orEmpty())) {
            for (tag in titleTags(title)) {
                tags -= tag
                if (set != null) {
                    val label = parseDiscTitle(tag)?.let { matchingKey(it.title) } ?: continue
                    tags -= label
                    if (label == "english") tags -= "en"
                    else if (label == "japanese") tags -= "ja"
                }
            }
        }
        if (set == null) {
            for (title in listOf(disc.title, disc.titleEn)) tags += titleTags(title)
        }
        when (regionKey(disc.region)) {
            "NTSC-U" -> tags.removeAll(setOf("usa", "us"))
            "NTSC-J" -> tags.removeAll(setOf("japan", "jp"))
            "PAL" -> tags.removeAll(setOf("europe", "eur"))
        }
        return tags.joinToString("") { "$it\n" }
    }

    /** The library's region labels, coarsened to what a dump's region tag can name. */
    private fun regionKey(region: String?): String = when (region) {
        "USA" -> "NTSC-U"
        "Japan" -> "NTSC-J"
        "Europe", "India" -> "PAL"
        null -> ""
        else -> region
    }

    private fun isSpace(c: Char): Boolean = c == ' ' || c in '\t'..'\r'

    private fun isSeparator(c: Char): Boolean = isSpace(c) || c == '-' || c == '_' || c == ':' || c == '.'

    private class Union(size: Int) {
        private val parents = IntArray(size) { it }

        fun find(index: Int): Int {
            var i = index
            while (parents[i] != i) {
                parents[i] = parents[parents[i]]
                i = parents[i]
            }
            return i
        }

        fun join(lhs: Int, rhs: Int) {
            val a = find(lhs)
            val b = find(rhs)
            parents[maxOf(a, b)] = minOf(a, b)
        }
    }
}
