// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>

// Memory card sync through the cloud, with emusync (https://github.com/ilyalissoboi/emusync).
//
// The slot 1 and 2 cards are synced, per save, with every other device using the same storage:
// other ARMSX2 installs, and PCSX2 on a computer running `emusync watch`. A save made on one device
// shows up on the others; two devices playing different games never conflict, and when the same
// save was changed on two devices the newest version wins and the other stays in emusync's history.
//
// Which cards: the ones in slots 1 and 2, file cards (.ps2) and folder cards. A card's identity on
// every device is its name without the extension, with the default names ("Mcd001.ps2",
// "mcd001.ps2", a folder "Mcd001") all meaning the card "Mcd001", so a desktop PCSX2 Mcd001.ps2
// and this app's mcd001.ps2 are the same card. PS1 cards and the multitap slots are not synced.
//
// When cards are read and written (with the hooks below in place):
//   Boot: FileMcd_Reopen calls OnCardsReopening() with the cards closed. Newer saves from other
//   devices are written into them before the game opens them, waiting at most 3 s for the network;
//   offline, the game starts with the cards as they are. From then until OnVMShutdown(), emusync
//   never writes to those cards; changes that arrive meanwhile wait and are applied after the game.
//   Play: OnCardWritten() (every card write) schedules a snapshot once writes have been quiet for
//   2 s; OnCardsFlushed() (pause, card close) snapshots at once. Snapshots go to an on-disk queue
//   first, so a save is not lost if the process is killed before the upload.
//   Shutdown: OnVMShutdown() ends the session, uploads, and applies waiting changes.
//   Background: SetAppBackgrounded(true) queues every card and pushes what it can.
// Order with MemcardBackup at boot: the app's own pre-boot snapshot, then this sync, then
// MemcardBackup::OnCardsReopening(), so the history always has the card from before sync changed it.
//
// Without ARMSX2_HAS_EMUSYNC (no Rust toolchain at build time, or not an Android build) every
// function here is a no-op and Start() fails, saying the build has no sync.
//
// Threading: everything is thread-safe. Start() and the boot hook block (local database open, and
// the network as described); every other hook returns quickly.
namespace MemcardSync
{
	/// Whether this build includes memory card sync.
	bool IsAvailable();

	struct Config
	{
		/// Private folder for emusync's database and queue. Must not be backed up and restored onto
		/// another device. Bound to one storage for good, so use one per remote_uri (Android:
		/// Context.getNoBackupFilesDir()/emusync-<hash of remote_uri>).
		std::string data_dir;
		/// Shown on other devices, e.g. in a save's history.
		std::string device_name;
		std::string emulator_version;
		/// The storage, e.g. "s3://my-saves/emusync/v1?region=eu-west-1".
		std::string remote_uri;
		std::string access_key_id;
		std::string secret_access_key;
		/// Optional: the storage's end-to-end encryption passphrase. Set, it creates the storage
		/// encrypted if it is new, or unlocks it if it already is; storage that is not encrypted is
		/// then refused. Empty, unencrypted storage is used as it is, and encrypted storage this
		/// device has already unlocked keeps working (emusync keeps its key in data_dir).
		std::string passphrase;
	};

	/// Overwrites a secret's characters, then empties the string and frees its buffer. Plain
	/// clear() only changes the size, leaving the characters readable in memory.
	void WipeSecret(std::string& secret);

	/// Starts syncing, replacing a client that is already running. Does not keep
	/// config.passphrase; the caller should WipeSecret() its own copy afterwards. Blocks while the local database
	/// opens and for up to 10 s while the storage is checked. Returns false with `error` set when
	/// sync cannot start (wrong address, keys rejected, unreadable data folder). Storage that cannot
	/// be reached right now is not an error: sync starts working once it can be reached.
	bool Start(const Config& config, std::string* error);

	/// Stops syncing. Cards are left as they are. Waits up to `timeout_ms` for queued uploads.
	void Stop(u32 timeout_ms);

	bool IsRunning();

	/// One line about the last sync, for the settings screen.
	std::string GetStatus();

	/// Queues a full sync: download, apply to cards no game has open, upload.
	void SyncNow();

	/// The app went out of sight (Android onStop) or came back (onStart). Going to the background
	/// queues every card on disk and uploads what it can, on a background thread. Call it after the
	/// cards were flushed: on Android, queued on the CPU thread behind the pause's FileMcd_Flush().
	void SetAppBackgrounded(bool backgrounded);

	// ---- core hooks ----------------------------------------------------------------------

	/// FileMcd_Reopen, with the cards closed and their types known (after FileMcd_SetType()).
	/// While the VM is starting, begins the session (see above). At any other time it only follows
	/// a change of the cards in the slots.
	void OnCardsReopening();

	/// FileMcd_Save and FileMcd_EraseBlock, with the combined slot number (0-7).
	void OnCardWritten(uint slot);

	/// FileMcd_Flush and FileMcd_EmuClose: the cards are on disk now.
	void OnCardsFlushed();

	/// VMManager::Shutdown after FileMcd_EmuClose(), and a boot that failed after
	/// OnCardsReopening(). Ends the session; safe to call when none is open.
	void OnVMShutdown();
} // namespace MemcardSync
