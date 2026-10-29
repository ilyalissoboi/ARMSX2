package com.armsx2.ui.home

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import androidx.core.content.edit

enum class HomeSort { Title, RecentlyPlayed, Compatibility }

enum class LibraryLayout { Grid, List, Shelf }

data class HomeUiState(
    val allGames: List<GameInfo> = emptyList(),
    val visibleGames: List<GameInfo> = emptyList(),
    val recentGames: List<GameInfo> = emptyList(),
    val query: String = "",
    val sort: HomeSort = HomeSort.Title,
    val layout: LibraryLayout = LibraryLayout.Grid,
    /** Category the LIST layout is filtered to, or null for everything. Only List uses it —
     *  Grid and Shelf show categories as their own sections instead, the way Recently Played
     *  works, so filtering the whole library there would hide the sections it is made of. */
    val categoryFilter: String? = null,
    val scanning: Boolean = false,
    val error: String? = null,
    val selectedIndex: Int = 0,
    val initialized: Boolean = false,
    /** A collapsed multi-disc game being launched, waiting for its disc to be picked. */
    val discPicker: DiscPicker? = null,
)

/** The disc picker's game (the collapsed entry, for its title) and the discs it offers. */
data class DiscPicker(val game: GameInfo, val discs: List<com.armsx2.data.library.DiscChoice>)

class HomeViewModel(application: Application) : AndroidViewModel(application) {
    private val repository = GameLibraryRepository(application)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var scanJob: Job? = null
    private var loaded = false
    private var pendingInitialScan = false
    private var directories: List<String> = emptyList()

    var state = androidx.compose.runtime.mutableStateOf(HomeUiState())
        private set

    fun load(romDirectories: List<String>, nativeReady: Boolean) {
        directories = romDirectories
        // A cached library can be listed before the core is up, and grouped without the GameDB's
        // disc sets then. Group again once it can answer.
        if (nativeReady && !collapsedWithNative) {
            collapsedWithNative = true
            collapsed = null
            if (loaded && com.armsx2.CollapseMultiDisc.enabled.value) state.value = buildState(state.value)
        }
        if (!loaded) {
            loaded = true
            val cached = repository.loadCached()
            val layout = runCatching {
                LibraryLayout.valueOf(
                    MainActivityRuntime.prefs.getString(LayoutPreference, LibraryLayout.Grid.name) ?: LibraryLayout.Grid.name,
                )
            }.getOrDefault(LibraryLayout.Grid)
            pendingInitialScan = romDirectories.isNotEmpty() && cached.key != repository.cacheKey(romDirectories)
            state.value = buildState(
                base = state.value.copy(
                    allGames = cached.games,
                    layout = layout,
                    initialized = cached.games.isNotEmpty() || !pendingInitialScan,
                ),
            )
            // Library-wide RetroAchievements progress. Hooked here because this is where the game
            // list lives, and the sync needs paths to hash. No-op unless a web API key is set.
            if (nativeReady) com.armsx2.RaLibrary.onLibraryLoaded(state.value.allGames)
            if (nativeReady && pendingInitialScan) refresh()
        } else if (nativeReady && pendingInitialScan) {
            refresh()
        }
    }

    /** The last [com.armsx2.data.library.LibraryRefresh] request this library has scanned for. */
    private var handledRefreshRequest = 0

    /** Scans [romDirectories] again for a LibraryRefresh request it has not handled yet, after a
     *  scan already running, so the files that request is about are certainly in the result. */
    fun onRefreshRequested(request: Int, romDirectories: List<String>) {
        if (request <= handledRefreshRequest) return
        handledRefreshRequest = request
        // The request can be for a folder just added to the library (the arcade folder).
        directories = romDirectories
        val running = scanJob?.takeIf { it.isActive }
        if (running == null) {
            refresh()
            return
        }
        scope.launch {
            running.join()
            refresh()
        }
    }

    fun refresh() {
        if (directories.isEmpty() || scanJob?.isActive == true) return
        scanJob = scope.launch {
            val initialScan = pendingInitialScan && state.value.allGames.isEmpty()
            state.value = state.value.copy(
                scanning = true,
                initialized = if (initialScan) false else state.value.initialized,
                error = null,
            )
            val result = runCatching { repository.scan(directories) }
            result.onSuccess { games ->
                pendingInitialScan = false
                state.value = buildState(
                    state.value.copy(allGames = games, scanning = false, initialized = true),
                )
                com.armsx2.RaLibrary.onLibraryLoaded(games)
            }.onFailure { failure ->
                pendingInitialScan = false
                state.value = state.value.copy(
                    scanning = false,
                    initialized = true,
                    error = failure.message ?: "Unable to scan the selected folders.",
                )
            }
        }
    }

    fun setQuery(value: String) {
        state.value = buildState(state.value.copy(query = value, selectedIndex = 0))
    }

    fun setSort(value: HomeSort) {
        state.value = buildState(state.value.copy(sort = value, selectedIndex = 0))
    }

    fun toggleLayout() {
        val entries = LibraryLayout.entries
        val next = entries[(state.value.layout.ordinal + 1) % entries.size]
        MainActivityRuntime.prefs.edit { putString(LayoutPreference, next.name) }
        state.value = state.value.copy(layout = next)
    }

    fun moveSelection(delta: Int) {
        val count = state.value.visibleGames.size
        if (count == 0) return
        state.value = state.value.copy(selectedIndex = (state.value.selectedIndex + delta).coerceIn(0, count - 1))
    }

    fun setSelection(index: Int) {
        if (state.value.visibleGames.isEmpty()) return
        state.value = state.value.copy(selectedIndex = index.coerceIn(state.value.visibleGames.indices))
    }

    fun selectedGame(): GameInfo? = state.value.visibleGames.getOrNull(state.value.selectedIndex)

    /** List-layout category filter. Null shows everything. */
    fun setCategoryFilter(category: String?) {
        state.value = buildState(state.value.copy(categoryFilter = category, selectedIndex = 0))
    }

    /**
     * Boots [game], or, for a collapsed multi-disc game, opens the disc picker instead: the entry
     * stands for every disc, and which one to boot is the player's call (a save on disc 2 is no use
     * from disc 1). [launchDisc] boots the one picked.
     */
    fun launch(game: GameInfo) {
        val discs = collapsedLibrary()?.discsOf(game)
        if (discs != null) {
            state.value = state.value.copy(discPicker = DiscPicker(game, discs))
            return
        }
        launchDisc(game)
    }

    /** Boots the disc picked from the disc picker. */
    fun pickDisc(disc: GameInfo) {
        state.value = state.value.copy(discPicker = null)
        launchDisc(disc)
    }

    fun dismissDiscPicker() {
        state.value = state.value.copy(discPicker = null)
    }

    /** The discs [game] stands for when it is a collapsed multi-disc game, else null. */
    fun discsOf(game: GameInfo): List<com.armsx2.data.library.DiscChoice>? = collapsedLibrary()?.discsOf(game)

    /** Lists the library again if "Collapse multi-disc games" was flipped (in App settings) since
     *  it was last listed. Called on every return to the library, so it does nothing otherwise:
     *  rebuilding would throw away the selection. */
    fun syncCollapseMultiDisc() {
        if (builtCollapsed == com.armsx2.CollapseMultiDisc.enabled.value) return
        state.value = buildState(state.value.copy(selectedIndex = 0, discPicker = null))
    }

    private fun launchDisc(game: GameInfo) {
        repository.markPlayed(game)
        state.value = buildState(state.value)
        val launchPath = if (game.uri.scheme == "file") game.uri.path ?: game.uri.toString() else game.uri.toString()
        MainActivityRuntime.launchGame(launchPath, game)
    }

    /** Mark a game hidden (or un-hidden) and refresh the visible list. A collapsed multi-disc
     *  game hides every disc, so it stays hidden with the setting turned off again. */
    fun setHidden(game: GameInfo, value: Boolean) {
        members(game).forEach { com.armsx2.HiddenGames.setHidden(it, value) }
        state.value = buildState(state.value)
    }

    /** Toggle whether hidden games are shown (so they can be un-hidden). */
    fun setShowHidden(value: Boolean) {
        com.armsx2.HiddenGames.setShowHidden(value)
        state.value = buildState(state.value)
    }

    /** Only the arcade games, or every game (the ⋮ menu's Arcade games only). */
    fun setArcadeOnly(value: Boolean) {
        com.armsx2.ArcadeOnly.set(value)
        state.value = buildState(state.value.copy(selectedIndex = 0))
    }

    /** Drop one game from Recently Played (it returns when next launched). */
    fun removeFromRecent(game: GameInfo) {
        members(game).forEach(repository::removeFromRecent)
        state.value = buildState(state.value)
    }

    /** Empty Recently Played. Games return as they're launched again; the library is untouched. */
    fun clearRecent() {
        repository.clearRecent()
        state.value = buildState(state.value)
    }

    /** The library grouped for "Collapse multi-disc games", built once per scan, or null while
     *  the setting is off. */
    private var collapsed: Pair<List<GameInfo>, com.armsx2.data.library.CollapsedLibrary>? = null
    /** Whether the core is up to look disc sets up in the GameDB; until then only titles group. */
    private var collapsedWithNative = false
    /** The setting the visible list was last built with; see [syncCollapseMultiDisc]. */
    private var builtCollapsed = false

    private fun collapsedLibrary(games: List<GameInfo> = state.value.allGames): com.armsx2.data.library.CollapsedLibrary? {
        if (!com.armsx2.CollapseMultiDisc.enabled.value) return null
        collapsed?.takeIf { it.first === games }?.let { return it.second }
        val useDatabase = collapsedWithNative
        val library = com.armsx2.data.library.CollapsedLibrary(games) { serial ->
            if (!useDatabase) null
            else com.armsx2.data.library.DiscSets.parseInfo(
                runCatching { kr.co.iefriends.pcsx2.NativeApp.getDiscSetForSerial(serial) }.getOrNull(),
            )
        }
        collapsed = games to library
        return library
    }

    /** Every disc [game] stands for: a collapsed game's whole set, otherwise just itself. */
    private fun members(game: GameInfo): List<GameInfo> = collapsedLibrary()?.members(game) ?: listOf(game)

    private fun buildState(base: HomeUiState): HomeUiState {
        val library = collapsedLibrary(base.allGames)
        builtCollapsed = library != null
        // Arcade games only narrows Recently Played as well as the library.
        val recents = repository.recentGames(base.allGames.filter(com.armsx2.ArcadeOnly::shows))
            .let { played -> if (library == null) played else played.map(library::entryFor).distinctBy { it.uri } }
        val recentOrder = recents.mapIndexed { index, game -> game.uri.toString() to index }.toMap()
        val forceEn = com.armsx2.EnglishTitles.enabled.value
        // A category filter narrows the pool before anything else. Cleared implicitly when the
        // category stops existing (renamed or deleted while selected) rather than leaving the
        // library stuck showing nothing with no obvious way back.
        val activeCategory = base.categoryFilter?.takeIf { it in com.armsx2.GameCategories.names() }
        val members = activeCategory?.let { com.armsx2.GameCategories.members(it) }
        val query = base.query.trim()
        val filtered = (library?.games ?: base.allGames).filter { game ->
            // A collapsed multi-disc game is each of its discs: filed under a category if any
            // disc is, found by any disc's title or serial, and hidden only once every disc is.
            val discs = library?.members(game) ?: listOf(game)
            (members == null || discs.any { disc -> disc.settingsKey?.let { it in members } == true }) &&
            com.armsx2.ArcadeOnly.shows(game) &&
            // Exclude games the user marked hidden (long-press → Hide), unless "Show hidden" is on.
            (com.armsx2.HiddenGames.showHidden.value || !discs.all(com.armsx2.HiddenGames::isHidden)) &&
                (query.isBlank() ||
                    (discs + game).any { disc ->
                        // Match BOTH names regardless of which is displayed: someone typing
                        // "Katakamuna" should find a game listed as 片神名, and vice versa.
                        disc.title.contains(query, ignoreCase = true) ||
                            disc.titleEn.contains(query, ignoreCase = true) ||
                            disc.titleSort.contains(query, ignoreCase = true) ||
                            disc.serial?.contains(query, ignoreCase = true) == true ||
                            disc.extension.contains(query, ignoreCase = true)
                    })
        }
        // sortKey(), not the displayed title: a Japanese title sorts by its kana reading
        // (GameDB name-sort), because sorting the kanji sorts by codepoint — which is the
        // "sort by name-sort" half of issue #338.
        val sorted = when (base.sort) {
            HomeSort.Title -> filtered.sortedBy { it.sortKey(forceEn).lowercase() }
            HomeSort.RecentlyPlayed -> filtered.sortedWith(
                compareBy<GameInfo> { recentOrder[it.uri.toString()] ?: Int.MAX_VALUE }
                    .thenBy { it.sortKey(forceEn).lowercase() },
            )
            HomeSort.Compatibility -> filtered.sortedWith(
                compareByDescending<GameInfo> { it.compatibility }
                    .thenBy { it.sortKey(forceEn).lowercase() },
            )
        }
        return base.copy(
            visibleGames = sorted,
            recentGames = recents,
            selectedIndex = base.selectedIndex.coerceIn(0, (sorted.size - 1).coerceAtLeast(0)),
        )
    }

    override fun onCleared() {
        scope.cancel()
        super.onCleared()
    }

    private companion object {
        // String-valued now (Grid/List/Shelf). New key so the old boolean pref is
        // ignored and everyone starts at Grid rather than mis-parsing "true"/"false".
        const val LayoutPreference = "library.layout.mode"
    }
}
