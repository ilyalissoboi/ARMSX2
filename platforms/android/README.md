# ARMSX2 
PlayStation 2 Emulator for Android based on the work of [PCSX2](https://github.com/PCSX2/pcsx2)

## App version

The Android app version is configured in `app/build.gradle.kts` under
`android.defaultConfig`:

```kotlin
versionCode = providers.gradleProperty("armsx2.versionCode").orNull?.toInt() ?: 1088
versionName = providers.gradleProperty("armsx2.versionName").orNull ?: "2.5.8"
```

Change `versionName` to the user-facing release version and increment the integer
`versionCode` for every published APK or AAB. Release scripts can still override
these defaults with `-Parmsx2.versionName=...` and `-Parmsx2.versionCode=...`.

## Android APK builds

Release/test APKs should be built with the universal page-size builder:

```bash
./tools/build-universal-page-apk.sh ~/Downloads/ARMSX2-Refresh-UniversalPage.apk
```

The script compiles both 4K and 16K ARM64 emucore variants, packages them into
one APK, signs it, and verifies 16K zip alignment. This keeps one distributable
APK working correctly on both older 4K-page devices and newer 16K-page devices.

## Memory card sync

Settings > Network > Memory card sync keeps the slot 1 and 2 memory cards in step with other devices
through S3-compatible storage the player owns, using [emusync](https://github.com/ilyalissoboi/emusync).
It syncs per save, so devices that played different games offline never conflict, and it
interoperates with PCSX2 on a computer running `emusync watch`.

- **Core:** `pcsx2/SIO/Memcard/MemcardSync.{h,cpp}`. Hooks in `FileMcd_Reopen` (before
  `MemcardBackup`), `FileMcd_Save`/`EraseBlock`/`Flush`/`EmuClose` and `VMManager::Shutdown`.
  Newer saves are applied before a game opens its cards, waiting at most 3 s for the network.
- **App:** `com.armsx2.sync.MemcardSyncSettings` keeps the settings. The secret access key and the
  optional end-to-end encryption passphrase are encrypted with an Android Keystore key. emusync's
  own data lives in `noBackupFilesDir/emusync-<hash of the storage address>`, one folder per
  address, since emusync binds a data folder to one storage for good.
  The settings UI is `ui/settings/MemcardSyncSection.kt`.
- **Encryption:** with a passphrase set, new storage is created end-to-end encrypted and encrypted
  storage is unlocked; storage that isn't encrypted is refused rather than used unencrypted. It
  works with encrypted repositories made by `emusync remote add … --encrypt` on a computer, given
  the same passphrase. Encryption is chosen when the storage is created, so moving existing saves
  to encrypted storage means a new address (a new folder in the bucket) on every device.
- **Build:** `app/src/main/cpp/3rdparty/emusync` fetches emusync at a pinned commit (`EMUSYNC_PIN`)
  and cargo-builds `libemusync.so`, the same way as librashader. Like librashader it is optional:
  without cargo or the Rust Android target, `ARMSX2_HAS_EMUSYNC` is off and the feature compiles
  out. `-DARMSX2_DISABLE_EMUSYNC=ON` turns it off; `-DEMUSYNC_SOURCE_DIR=<checkout>` builds from a
  local emusync checkout.
