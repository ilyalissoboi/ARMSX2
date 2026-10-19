// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.sync

import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import com.armsx2.BuildConfig
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Memory card sync (emusync): its settings, and starting or stopping it to match them.
 *
 * The core syncs the slot 1 and 2 cards (pcsx2/SIO/Memcard/MemcardSync.h); this object owns what
 * the user typed in, which the core only holds in memory while sync runs:
 *
 * - The storage address, access key ID and device name are in SharedPreferences ("memcard_sync").
 * - The secret access key is encrypted with an AES key in the Android Keystore, which never leaves
 *   this device. Android's backup may copy the encrypted value to a new phone, but not the key, so
 *   there it simply reads as "no secret" and the user enters it again. The key itself is not
 *   exportable.
 * - emusync's own data (database and upload queue) lives in noBackupFilesDir/emusync. It must not
 *   be restored onto another device: two installs with one device identity would mix their logs.
 *
 * Starting talks to the network for up to ~10 s, so [start] blocks and the UI calls it on a
 * background thread; [startInBackground] is the fire-and-forget form used at app start.
 */
object MemcardSyncSettings {
    private const val TAG = "MemcardSync"
    private const val PREFS = "memcard_sync"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_REMOTE = "remoteUri"
    private const val KEY_ACCESS_KEY_ID = "accessKeyId"
    private const val KEY_SECRET = "secretAccessKey.enc"
    private const val KEY_DEVICE = "deviceName"
    private const val KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "armsx2.memcardSync.secret"

    data class Values(
        val enabled: Boolean,
        val remoteUri: String,
        val accessKeyId: String,
        /** A secret is stored and can be decrypted on this device. */
        val hasSecret: Boolean,
        val deviceName: String,
    )

    /** Whether this build includes sync (no Rust toolchain at build time compiles it out). */
    fun isAvailable(): Boolean = runCatching { NativeApp.memcardSyncAvailable() }.getOrDefault(false)

    fun defaultDeviceName(): String =
        listOf(Build.MANUFACTURER, Build.MODEL).filter { it.isNotBlank() }.distinct().joinToString(" ")
            .ifBlank { "Android" }

    fun load(context: Context): Values {
        val p = prefs(context)
        return Values(
            enabled = p.getBoolean(KEY_ENABLED, false),
            remoteUri = p.getString(KEY_REMOTE, "").orEmpty(),
            accessKeyId = p.getString(KEY_ACCESS_KEY_ID, "").orEmpty(),
            hasSecret = secret(context) != null,
            deviceName = p.getString(KEY_DEVICE, "").orEmpty().ifBlank { defaultDeviceName() },
        )
    }

    fun setEnabled(context: Context, enabled: Boolean) = edit(context) { putBoolean(KEY_ENABLED, enabled) }
    fun setRemoteUri(context: Context, value: String) = edit(context) { putString(KEY_REMOTE, value.trim()) }
    fun setAccessKeyId(context: Context, value: String) = edit(context) { putString(KEY_ACCESS_KEY_ID, value.trim()) }
    fun setDeviceName(context: Context, value: String) = edit(context) { putString(KEY_DEVICE, value.trim()) }

    /** Stores the secret encrypted; an empty value removes it. */
    fun setSecret(context: Context, value: String) {
        val trimmed = value.trim()
        if (trimmed.isEmpty()) {
            edit(context) { remove(KEY_SECRET) }
            return
        }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, keystoreKey())
        val sealed = cipher.iv + cipher.doFinal(trimmed.toByteArray(Charsets.UTF_8))
        edit(context) { putString(KEY_SECRET, Base64.encodeToString(sealed, Base64.NO_WRAP)) }
    }

    /** emusync's private folder: not backed up, see the class comment. */
    fun dataDir(context: Context): File = File(context.noBackupFilesDir, "emusync").apply { mkdirs() }

    /**
     * Starts sync with the stored settings, or stops it when sync is switched off or incomplete.
     * Blocks (local database, up to ~10 s of network): never call it on the main thread. Returns
     * null when sync is running or deliberately off, else a sentence for the user.
     */
    fun start(context: Context): String? {
        if (!isAvailable()) return null
        val v = load(context)
        val secret = secret(context)
        if (!v.enabled || v.remoteUri.isBlank() || v.accessKeyId.isBlank() || secret == null) {
            runCatching { NativeApp.memcardSyncStop() }
            return null
        }
        return runCatching {
            NativeApp.memcardSyncStart(
                dataDir(context).absolutePath,
                v.deviceName,
                BuildConfig.VERSION_NAME,
                v.remoteUri,
                v.accessKeyId,
                secret,
            )
        }.getOrElse { e ->
            Log.w(TAG, "start failed", e)
            e.message ?: e.toString()
        }
    }

    /** [start] on its own thread, for app start: nothing waits for it. */
    fun startInBackground(context: Context) {
        val app = context.applicationContext
        Thread({
            start(app)?.let { Log.w(TAG, "not syncing: $it") }
        }, "MemcardSyncStart").start()
    }

    fun stop() {
        runCatching { NativeApp.memcardSyncStop() }
    }

    fun isRunning(): Boolean = runCatching { NativeApp.memcardSyncRunning() }.getOrDefault(false)

    fun syncNow() {
        runCatching { NativeApp.memcardSyncNow() }
    }

    /** One line about the last sync; empty when nothing happened yet. */
    fun status(): String = runCatching { NativeApp.memcardSyncStatus() }.getOrDefault("")

    // ---- storage ---------------------------------------------------------------------------

    private fun prefs(context: Context) = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private inline fun edit(context: Context, block: android.content.SharedPreferences.Editor.() -> Unit) {
        prefs(context).edit().apply(block).apply()
    }

    private fun secret(context: Context): String? {
        val sealed = prefs(context).getString(KEY_SECRET, null) ?: return null
        return runCatching {
            val bytes = Base64.decode(sealed, Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            // GCM's IV is 12 bytes; the tag is the last 16 of the ciphertext.
            cipher.init(Cipher.DECRYPT_MODE, existingKey() ?: return null, GCMParameterSpec(128, bytes, 0, 12))
            String(cipher.doFinal(bytes, 12, bytes.size - 12), Charsets.UTF_8)
        }.getOrElse {
            // A value restored from another device's backup, or a key the system wiped: unreadable
            // here, so ask for the secret again rather than fail every start.
            Log.w(TAG, "stored secret cannot be decrypted on this device", it)
            null
        }
    }

    private fun existingKey(): SecretKey? {
        val ks = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        return (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.secretKey
    }

    private fun keystoreKey(): SecretKey = existingKey() ?: KeyGenerator.getInstance(
        KeyProperties.KEY_ALGORITHM_AES, KEYSTORE,
    ).apply {
        init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
    }.generateKey()
}
