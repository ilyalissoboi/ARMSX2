package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * JVM tests for [ShaderRepo]'s source list and for [ShaderRepo.companionSubtree], which places
 * a companion pack by reading its presets' `../` depth rather than trusting the archive layout.
 */
class ShaderRepoSourceTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val stage = "../../../shaders_slang/crt/shaders/guest/advanced/stock.slang"

    private fun write(root: File, rel: String, text: String): File =
        File(root, rel).apply { parentFile!!.mkdirs(); writeText(text) }

    @Test
    fun everySourceHasExactlyOneWayToDownload() {
        for (s in ShaderRepo.sources()) {
            val github = s.githubRepo != null
            assertTrue("${s.id}: needs a url or a GitHub repo", (s.url != null) != github)
            assertEquals("${s.id}: a GitHub repo needs an asset pattern", github, s.assetPattern != null)
        }
    }

    @Test
    fun githubPacksFollowTheLatestRelease() {
        val github = ShaderRepo.sources().filter { it.githubRepo != null }.map { it.id }.toSet()
        assertEquals(setOf("satpixie_crt", "lola_retro", "retro_crisis_gdv_ntsc"), github)
        ShaderRepo.sources().forEach { s ->
            assertTrue("${s.id} pins a GitHub URL", s.url?.contains("github.com") != true)
        }
    }

    @Test
    fun assetPatternsPickTheRetroArchZip() {
        val pattern = ShaderRepo.sources().associate { it.id to it.assetPattern }
        val lola = pattern["lola_retro"]!!
        assertTrue(lola.matches("lola-retro-v1.19.zip"))
        assertTrue(listOf("86box", "amiberry", "kodi").none { lola.matches("lola-retro-$it-v1.19.zip") })
        val pixie = pattern["satpixie_crt"]!!
        assertTrue(pixie.matches("satpixie-crt-shader-20260122.zip"))
        assertTrue(!pixie.matches("satpixie-crt-shader-variants-20260122.zip"))
        assertTrue(pattern["retro_crisis_gdv_ntsc"]!!.matches("Retro.Crisis.GDV-NTSC.2026.09.27.zip"))
    }

    @Test
    fun bareCompanionLayoutMergesWhole() {
        // 2026.09.27 onwards: `retro crisis/<res>/x.slangp` at the archive root.
        val tree = tmp.newFolder("bare")
        write(tree, "retro crisis/4K Flat/a.slangp", "shader0 = \"$stage\"\n")
        write(tree, "retro crisis/4K Curved/b.slangp", "#reference \"../4K Flat/a.slangp\"\n")
        assertEquals(tree, ShaderRepo.companionSubtree(tree, "shaders_slang"))
    }

    @Test
    fun wrappedCompanionLayoutDropsTheWrapping() {
        // 2026.03.21: the same tree under `shaders/shaders_slang/`.
        val tree = tmp.newFolder("wrapped")
        write(tree, "shaders/shaders_slang/retro crisis/1440p Curved/a.slangp", "#reference \"../1440p Flat/a.slangp\"\n")
        write(tree, "shaders/shaders_slang/retro crisis/1440p Flat/a.slangp", "shader0 = \"$stage\"\n")
        assertEquals(File(tree, "shaders/shaders_slang"), ShaderRepo.companionSubtree(tree, "shaders_slang"))
    }

    @Test
    fun companionThatCannotReachItsDepthIsRefused() {
        val tree = tmp.newFolder("shallow")
        write(tree, "a.slangp", "shader0 = \"$stage\"\n")
        assertNull(ShaderRepo.companionSubtree(tree, "shaders_slang"))
    }

    @Test
    fun companionNamingNoBaseStageIsRefused() {
        val tree = tmp.newFolder("standalone")
        write(tree, "x/y/a.slangp", "shader0 = \"../shaders/a.slang\"\n")
        assertNull(ShaderRepo.companionSubtree(tree, "shaders_slang"))
        write(tree, "x/y/b.slangp", "shader0 = \"$stage\"\n")
        assertNotNull(ShaderRepo.companionSubtree(tree, "shaders_slang"))
    }
}
