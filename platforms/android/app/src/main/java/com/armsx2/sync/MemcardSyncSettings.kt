// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

package com.armsx2.sync

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import com.armsx2.BuildConfig
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import java.util.concurrent.Executors
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
 * - The secret access key and the optional encryption passphrase are encrypted with an AES key in
 *   the Android Keystore, which never leaves this device. Android's backup may copy the encrypted
 *   values to a new phone, but not the key, so there they simply read as "not set" and the user
 *   enters them again. The key itself is not exportable.
 * - emusync's own data (database, upload queue, and the key of encrypted storage) lives in
 *   noBackupFilesDir/emusync-<hash of the storage address>. It must not be restored onto another
 *   device: two installs with one device identity would mix their logs. One folder per address,
 *   because emusync binds a data folder to one storage for good: switching to another address (for
 *   example a new folder for encrypted saves) starts a fresh folder, and switching back finds the
 *   old one again.
 *
 * Starting talks to the network for up to ~10 s, so [start] blocks; the settings screen uses
 * [startAsync], and [startInBackground] is the fire-and-forget form used at app start.
 */
object MemcardSyncSettings {
    private const val TAG = "MemcardSync"
    private const val PREFS = "memcard_sync"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_REMOTE = "remoteUri"
    private const val KEY_ACCESS_KEY_ID = "accessKeyId"
    private const val KEY_SECRET = "secretAccessKey.enc"
    private const val KEY_PASSPHRASE = "passphrase.enc"
    private const val KEY_DEVICE = "deviceName"
    private const val KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "armsx2.memcardSync.secret"

    data class Values(
        val enabled: Boolean,
        val remoteUri: String,
        val accessKeyId: String,
        /** A secret is stored and can be decrypted on this device. */
        val hasSecret: Boolean,
        /** An encryption passphrase is stored and can be decrypted on this device. */
        val hasPassphrase: Boolean,
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
            hasSecret = unseal(context, KEY_SECRET) != null,
            hasPassphrase = unseal(context, KEY_PASSPHRASE) != null,
            deviceName = p.getString(KEY_DEVICE, "").orEmpty().ifBlank { defaultDeviceName() },
        )
    }

    fun setEnabled(context: Context, enabled: Boolean) = edit(context) { putBoolean(KEY_ENABLED, enabled) }
    fun setRemoteUri(context: Context, value: String) = edit(context) { putString(KEY_REMOTE, value.trim()) }
    fun setAccessKeyId(context: Context, value: String) = edit(context) { putString(KEY_ACCESS_KEY_ID, value.trim()) }
    fun setDeviceName(context: Context, value: String) = edit(context) { putString(KEY_DEVICE, value.trim()) }

    /** Stores the secret encrypted; an empty value removes it. */
    fun setSecret(context: Context, value: String) = seal(context, KEY_SECRET, value.trim())

    /**
     * Stores the end-to-end encryption passphrase encrypted; an empty value removes it. Passed to
     * the core at every start (MemcardSync::Config::passphrase): it creates new storage encrypted,
     * unlocks encrypted storage, and makes storage that is not encrypted an error. Not trimmed:
     * spaces can be part of a passphrase.
     */
    fun setPassphrase(context: Context, value: String) = seal(context, KEY_PASSPHRASE, value)

    /**
     * The stored passphrase, for the settings screen's Show: the user needs it again to set up
     * their other devices, and losing it loses the saves. Null when none is stored or it can't be
     * decrypted here. (The secret access key has no such getter: it can be issued again.)
     */
    fun passphrase(context: Context): String? = unseal(context, KEY_PASSPHRASE)

    /** emusync's private folder for [remoteUri]: not backed up, see the class comment. */
    fun dataDir(context: Context, remoteUri: String): File {
        val digest = MessageDigest.getInstance("SHA-256").digest(remoteUri.toByteArray(Charsets.UTF_8))
        val id = digest.take(8).joinToString("") { "%02x".format(it) }
        return File(context.noBackupFilesDir, "emusync-$id").apply { mkdirs() }
    }

    /**
     * Starts sync with the stored settings, or stops it when sync is switched off or incomplete.
     * Blocks (local database, up to ~10 s of network): never call it on the main thread. Returns
     * null when sync is running or deliberately off, else a sentence for the user.
     */
    fun start(context: Context): String? {
        if (!isAvailable()) return null
        val v = load(context)
        val secret = unseal(context, KEY_SECRET)
        if (!v.enabled || v.remoteUri.isBlank() || v.accessKeyId.isBlank() || secret == null) {
            runCatching { NativeApp.memcardSyncStop() }
            return null
        }
        return runCatching {
            NativeApp.memcardSyncStart(
                dataDir(context, v.remoteUri).absolutePath,
                v.deviceName,
                BuildConfig.VERSION_NAME,
                v.remoteUri,
                v.accessKeyId,
                secret,
                unseal(context, KEY_PASSPHRASE),
            )
        }.getOrElse { e ->
            Log.w(TAG, "start failed", e)
            e.message ?: e.toString()
        }
    }

    /** [start] in the background, for app start: nothing waits for it. */
    fun startInBackground(context: Context) = startAsync(context) { it?.let { e -> Log.w(TAG, "not syncing: $e") } }

    /**
     * [start] in the background, then [onDone] with its result on the main thread.
     *
     * Starts run one at a time, in order, on a thread that belongs to the app rather than to a
     * screen: a settings screen that closes right after a change (a field saved as it leaves the
     * screen) must not cancel the restart, or the stored settings and the running sync disagree.
     * Each start reads the settings when it runs, so after several quick changes the last one wins.
     */
    fun startAsync(context: Context, onDone: (String?) -> Unit) {
        val app = context.applicationContext
        starter.execute {
            val result = start(app)
            mainHandler.post { onDone(result) }
        }
    }

    private val starter = Executors.newSingleThreadExecutor { Thread(it, "MemcardSyncStart") }
    private val mainHandler = Handler(Looper.getMainLooper())

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

    private fun seal(context: Context, key: String, value: String) {
        if (value.isEmpty()) {
            edit(context) { remove(key) }
            return
        }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, keystoreKey())
        val sealed = cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        edit(context) { putString(key, Base64.encodeToString(sealed, Base64.NO_WRAP)) }
    }

    private fun unseal(context: Context, key: String): String? {
        val sealed = prefs(context).getString(key, null) ?: return null
        return runCatching {
            val bytes = Base64.decode(sealed, Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            // GCM's IV is 12 bytes; the tag is the last 16 of the ciphertext.
            cipher.init(Cipher.DECRYPT_MODE, existingKey() ?: return null, GCMParameterSpec(128, bytes, 0, 12))
            String(cipher.doFinal(bytes, 12, bytes.size - 12), Charsets.UTF_8)
        }.getOrElse {
            // A value restored from another device's backup, or a key the system wiped: unreadable
            // here, so ask for it again rather than fail every start.
            Log.w(TAG, "stored $key cannot be decrypted on this device", it)
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
