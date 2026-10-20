// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemcardSync.h"

// With or without emusync: the JNI side wipes its copy of the passphrase either way.
void MemcardSync::WipeSecret(std::string& secret)
{
	// Through a volatile pointer, so the stores can't be dropped as dead before the release.
	volatile char* p = secret.data();
	for (size_t i = 0; i < secret.size(); i++)
		p[i] = 0;
	secret.clear();
	secret.shrink_to_fit();
}

#ifndef ARMSX2_HAS_EMUSYNC

namespace MemcardSync
{
	bool IsAvailable() { return false; }
	bool Start(const Config&, std::string* error)
	{
		if (error)
			*error = "This build has no memory card sync.";
		return false;
	}
	void Stop(u32) {}
	bool IsRunning() { return false; }
	std::string GetStatus() { return {}; }
	void SyncNow() {}
	void SetAppBackgrounded(bool) {}
	void OnCardsReopening() {}
	bool IsSyncing(uint) { return false; }
	void OnCardWritten(uint) {}
	void OnCardsFlushed() {}
	void OnVMShutdown() {}
} // namespace MemcardSync

#else

#include "Config.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <emusync.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

namespace MemcardSync
{
	/// Slots 1 and 2 only, like MemcardBackup: the multitap cards are not synced.
	static constexpr size_t SYNCED_SLOTS = 2;
	/// How long boot waits for the network before the game starts with the cards as they are.
	static constexpr u32 BOOT_SYNC_TIMEOUT_MS = 3000;
	/// A sync with no event to trigger it, while the app is in front.
	static constexpr std::chrono::minutes PERIODIC_SYNC{5};
	/// What backgrounding gets for queueing and uploading before Android may reclaim the process.
	static constexpr u32 SUSPEND_BUDGET_MS = 4000;

	struct Mounted
	{
		std::array<es_container_id, SYNCED_SLOTS> ids{};
		std::array<std::string, SYNCED_SLOTS> paths;
		bool operator==(const Mounted& o) const { return ids == o.ids && paths == o.paths; }
	};

	/// Serialises Start() and Stop() with each other.
	static std::mutex s_lifecycle_lock;
	/// The client's lifetime. Every es_* call holds it shared; Start() and Stop() hold it unique, so
	/// the client is never destroyed under a call. The longest shared hold is the boot sync (3 s).
	static std::shared_mutex s_client_lock;
	static es_client* s_client = nullptr;

	/// Everything else, held briefly.
	static std::mutex s_state_lock;
	static Config s_config;
	static Mounted s_mounted;
	static es_session_id s_session = 0;
	static std::chrono::steady_clock::time_point s_session_started;
	static std::string s_status;

	/// The worker thread: periodic syncs, the background push, and draining emusync's events.
	static std::thread s_worker;
	static std::mutex s_worker_lock;
	static std::condition_variable s_worker_cv;
	static bool s_worker_stop = false;
	static bool s_worker_wake = false;
	static bool s_suspend_requested = false;
	static std::atomic<bool> s_backgrounded{false};

	static void SetStatus(std::string status)
	{
		std::lock_guard lock(s_state_lock);
		s_status = std::move(status);
	}

	static std::string TimeOfDay()
	{
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_r(&now, &local);
		char buf[16];
		std::strftime(buf, sizeof(buf), "%H:%M", &local);
		return buf;
	}

	static void Log(void*, es_log_level level, const char*, const char* msg)
	{
		switch (level)
		{
			case ES_LOG_ERROR:
				Console.Error("(MemcardSync) %s", msg);
				break;
			case ES_LOG_WARN:
				Console.Warning("(MemcardSync) %s", msg);
				break;
			case ES_LOG_INFO:
				Console.WriteLn("(MemcardSync) %s", msg);
				break;
			default:
				DevCon.WriteLn("(MemcardSync) %s", msg);
				break;
		}
	}

	/// The credential provider: the keys from Start(), kept in memory only.
	static int GetCredentials(void*, const char*, es_credentials* out)
	{
		std::lock_guard lock(s_state_lock);
		if (s_config.access_key_id.empty() || s_config.secret_access_key.empty())
			return ES_E_AUTH;
		out->kind = ES_CRED_S3;
		StringUtil::Strlcpy(out->access_key_id, s_config.access_key_id, sizeof(out->access_key_id));
		StringUtil::Strlcpy(out->secret_access_key, s_config.secret_access_key, sizeof(out->secret_access_key));
		return 0;
	}

	static void WakeWorker()
	{
		{
			std::lock_guard lock(s_worker_lock);
			s_worker_wake = true;
		}
		s_worker_cv.notify_one();
	}

	/// Called by emusync on its own thread when events are waiting; must not call into emusync.
	static void OnEventsWaiting(void*) { WakeWorker(); }

	/// The logical card a card file or folder stands for: its name without ".ps2", with the default
	/// names normalised so this app's "mcd001.ps2" and PCSX2's "Mcd001.ps2" are one card.
	static std::string CardName(const std::string& filename)
	{
		std::string stem(filename);
		if (StringUtil::EndsWithNoCase(stem, ".ps2"))
			stem.resize(stem.size() - 4);
		for (const char* name : {"Mcd001", "Mcd002"})
		{
			if (StringUtil::compareNoCase(stem, name))
				return name;
		}
		return stem;
	}

	/// Registers the slot 1 and 2 cards that can be synced. Needs the client lock (shared).
	static Mounted RegisterSlots()
	{
		Mounted m;
		for (size_t slot = 0; slot < SYNCED_SLOTS; slot++)
		{
			const Pcsx2Config::McdOptions& mcd = EmuConfig.Mcd[slot];
			if (!mcd.Enabled || mcd.Type == MemoryCardType::Empty || mcd.Filename.empty())
				continue;
			const std::string path = EmuConfig.FullpathToMcd(static_cast<uint>(slot));
			const char* format;
			if (mcd.Type == MemoryCardType::Folder)
			{
				if (!FileSystem::DirectoryExists(path.c_str()))
					continue;
				format = "ps2.card.folder";
			}
			else
			{
				// A missing card is created by the core when first used; it is registered on the next
				// boot. A file without the .ps2 extension (a PS1 card) is not a PS2 card image.
				if (!FileSystem::FileExists(path.c_str()) || !StringUtil::EndsWithNoCase(path, ".ps2"))
					continue;
				format = "ps2.card";
			}
			const std::string card = CardName(mcd.Filename);
			es_container_desc desc = ES_CONTAINER_DESC_INIT;
			desc.format = format;
			desc.location = path.c_str();
			desc.platform = "ps2";
			desc.card = card.c_str();
			es_container_id id = 0;
			const es_status st = es_container_add(s_client, &desc, &id);
			if (st != ES_OK)
			{
				Console.Warning("(MemcardSync) Not syncing %s: %s", path.c_str(), es_status_str(st));
				continue;
			}
			m.ids[slot] = id;
			m.paths[slot] = path;
		}
		return m;
	}

	/// Begins a session on the cards in `m`. Needs the client lock (shared). The state lock must NOT
	/// be held: this can wait for the network.
	static es_session_id BeginSession(const Mounted& m, u32 timeout_ms, es_session_info* info)
	{
		std::vector<es_container_id> ids;
		for (const es_container_id id : m.ids)
		{
			if (id != 0)
				ids.push_back(id);
		}
		if (ids.empty())
			return 0;

		const std::string serial = VMManager::GetDiscSerial();
		const std::string title_name = VMManager::GetTitle(true);
		es_title title = ES_TITLE_INIT;
		title.platform = "ps2";
		// The serial picks which saves are fetched first; a BIOS boot has none.
		title.serial = serial.empty() ? "BIOS" : serial.c_str();
		title.display_name = title_name.empty() ? nullptr : title_name.c_str();

		es_session_begin_args args = ES_SESSION_BEGIN_INIT;
		args.title = &title;
		args.mounted = ids.data();
		args.mounted_count = ids.size();
		args.timeout_ms = timeout_ms;
		*info = ES_SESSION_INFO_INIT;
		const es_status st = es_session_begin(s_client, &args, info);
		if (st < 0)
		{
			Console.Warning("(MemcardSync) Could not start syncing the cards in use: %s", es_status_str(st));
			return 0;
		}
		return info->id;
	}

	/// Ends the open session, if any. Needs the client lock (shared).
	static void EndSession()
	{
		es_session_id id;
		u64 played_ms;
		{
			std::lock_guard lock(s_state_lock);
			id = s_session;
			s_session = 0;
			played_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - s_session_started).count());
		}
		if (id != 0)
			es_session_end(s_client, id, played_ms);
	}

	/// Starts following the cards a running VM has open: used when sync starts mid-game, or the
	/// cards in the slots changed. Never waits for the network (the game is running).
	static void FollowRunningVM()
	{
		const Mounted m = RegisterSlots();
		{
			std::lock_guard lock(s_state_lock);
			if (s_session != 0 && m == s_mounted)
				return;
		}
		EndSession();
		es_session_info info;
		// 1 ms: the cards are marked in use at once; what the network brings waits for the game to end.
		const es_session_id id = BeginSession(m, 1, &info);
		std::lock_guard lock(s_state_lock);
		s_mounted = m;
		s_session = id;
		s_session_started = std::chrono::steady_clock::now();
	}

	static constexpr const char* LOCKED_STATUS =
		"This storage is encrypted. Enter its passphrase in Settings > Network to sync.";

	static std::string DescribeEvent(const es_event& ev)
	{
		const char* save = ev.unit_name ? ev.unit_name : "a save";
		if (ev.status == ES_E_LOCKED && (ev.type == ES_EV_SYNC_FINISHED || ev.type == ES_EV_ERROR))
			return LOCKED_STATUS;
		switch (ev.type)
		{
			case ES_EV_SYNC_FINISHED:
				if (ev.status == ES_OK)
					return fmt::format("Synced at {}.", TimeOfDay());
				if (ev.status == ES_OFFLINE)
					return fmt::format("Offline at {}: changes upload once the storage can be reached.", TimeOfDay());
				return fmt::format("Sync failed at {}: {}", TimeOfDay(), ev.message ? ev.message : es_status_str(ev.status));
			case ES_EV_UNIT_APPLIED:
				return fmt::format("Updated {}{} at {}.", save,
					ev.device_name ? fmt::format(" from {}", ev.device_name) : std::string(), TimeOfDay());
			case ES_EV_APPLY_BLOCKED:
				if (ev.status == ES_E_NO_SPACE)
					return fmt::format("{} does not fit on the card. Free some space on it.", save);
				if (ev.status == ES_E_BUSY)
					return {}; // a card in use; it is applied when the game ends
				return fmt::format("Could not update {}: {}", save, ev.message ? ev.message : es_status_str(ev.status));
			case ES_EV_AUTH_REQUIRED:
				return "The storage rejected the access key. Check it in Settings > Network.";
			case ES_EV_ERROR:
				return fmt::format("Sync error: {}", ev.message ? ev.message : es_status_str(ev.status));
			default:
				return {};
		}
	}

	/// Drains emusync's events into the log and the status line. Needs the client lock (shared).
	static void DrainEvents()
	{
		es_event ev = ES_EVENT_INIT;
		while (es_event_poll(s_client, &ev) == 1)
		{
			std::string text = DescribeEvent(ev);
			if (ev.type == ES_EV_UNIT_APPLIED || ev.type == ES_EV_UNIT_UPLOADED)
				Console.WriteLn("(MemcardSync) %s %s", ev.type == ES_EV_UNIT_APPLIED ? "applied" : "uploaded",
					ev.unit_id ? ev.unit_id : "?");
			else if (!text.empty() && ev.type != ES_EV_SYNC_FINISHED)
				Console.Warning("(MemcardSync) %s", text.c_str());
			if (!text.empty())
				SetStatus(std::move(text));
			es_event_release(s_client, &ev);
			ev = ES_EVENT_INIT;
		}
	}

	static void WorkerLoop()
	{
		auto next_sync = std::chrono::steady_clock::now() + PERIODIC_SYNC;
		for (;;)
		{
			bool suspend = false;
			{
				std::unique_lock lock(s_worker_lock);
				s_worker_cv.wait_until(lock, next_sync, [] { return s_worker_stop || s_worker_wake || s_suspend_requested; });
				if (s_worker_stop)
					return;
				suspend = std::exchange(s_suspend_requested, false);
				s_worker_wake = false;
			}

			std::shared_lock client(s_client_lock);
			if (!s_client)
				return;
			if (suspend)
			{
				const es_status st = es_app_suspend(s_client, SUSPEND_BUDGET_MS);
				DevCon.WriteLn("(MemcardSync) background push: %s", es_status_str(st));
			}
			else if (std::chrono::steady_clock::now() >= next_sync)
			{
				if (!s_backgrounded.load(std::memory_order_acquire))
					es_sync_now(s_client);
				next_sync = std::chrono::steady_clock::now() + PERIODIC_SYNC;
			}
			DrainEvents();
		}
	}

	/// Stops the worker and destroys the client. Needs the lifecycle lock and the client lock (unique).
	static void StopLocked(u32 timeout_ms)
	{
		if (!s_client)
			return;
		{
			std::lock_guard lock(s_worker_lock);
			s_worker_stop = true;
		}
		s_worker_cv.notify_one();
		// The worker takes the client lock shared, which this thread holds unique: release it while
		// the worker winds down, so a worker blocked on it can see the stop flag.
		// Nothing else can start or stop the client meanwhile: the lifecycle lock is held.
		s_client_lock.unlock();
		if (s_worker.joinable())
			s_worker.join();
		s_client_lock.lock();

		es_event_set_notifier(s_client, nullptr, nullptr);
		es_client_destroy(s_client, timeout_ms);
		s_client = nullptr;

		std::lock_guard lock(s_state_lock);
		s_session = 0;
		s_mounted = {};
		s_config = {};
		s_status.clear();
	}

	bool IsAvailable() { return true; }

	bool Start(const Config& config, std::string* error)
	{
		std::lock_guard lifecycle(s_lifecycle_lock);
		std::unique_lock client(s_client_lock);
		StopLocked(2000);

		{
			std::lock_guard lock(s_state_lock);
			s_config = config;
			// Only needed below, once: not kept for the life of the client like the keys.
			WipeSecret(s_config.passphrase);
		}

		es_config cfg = ES_CONFIG_INIT;
		cfg.data_dir = config.data_dir.c_str();
		cfg.device_name = config.device_name.empty() ? nullptr : config.device_name.c_str();
		cfg.emulator_id = "armsx2";
		cfg.emulator_version = config.emulator_version.c_str();
		// No conflict screen yet: the newest version wins and the other stays in emusync's history.
		cfg.conflict_policy = ES_POLICY_NEWEST;
		cfg.log = &Log;
		cfg.log_level = ES_LOG_INFO;
		es_status st = es_client_create(&cfg, &s_client);
		if (st != ES_OK)
		{
			if (error)
				*error = fmt::format("Could not open the sync data in {}: {}", config.data_dir, es_status_str(st));
			s_client = nullptr;
			// Stop() returns early without a client, so this is the only place the copied keys can go.
			std::lock_guard lock(s_state_lock);
			s_config = {};
			return false;
		}

		const bool encrypted = !config.passphrase.empty();
		// Applies to the es_remote_add below: create the storage encrypted, or unlock it.
		if (encrypted)
			st = es_repo_create_encrypted(s_client, config.passphrase.c_str());
		const es_credential_provider creds = {nullptr, &GetCredentials};
		if (st == ES_OK)
			st = es_remote_add(s_client, config.remote_uri.c_str(), &creds);
		// Locked: encrypted storage this device has no key for, and no passphrase was given. The
		// remote is added; sync runs and snapshots the cards, and uploads once it is unlocked.
		if (st != ES_OK && st != ES_OFFLINE && st != ES_E_LOCKED)
		{
			if (error)
			{
				if (st == ES_E_AUTH)
					*error = "The storage rejected the access key ID or secret.";
				else if (st == ES_E_PASSPHRASE)
					*error = "The passphrase does not unlock this storage.";
				else if (st == ES_E_INCOMPATIBLE && encrypted)
					*error = "This storage is not encrypted, or was set up by a newer version of emusync. "
					         "Remove the passphrase to use it, or choose a new folder for encrypted saves.";
				else if (st == ES_E_INCOMPATIBLE)
					*error = "This storage was set up by a newer version of emusync, or has changed since this device joined it.";
				else if (st == ES_E_INVALID_ARG)
					*error = "The storage address is not valid, or this device already syncs with different storage.";
				else
					*error = fmt::format("Could not use the storage: {}", es_status_str(st));
			}
			es_client_destroy(s_client, 0);
			s_client = nullptr;
			std::lock_guard lock(s_state_lock);
			s_config = {};
			return false;
		}
		SetStatus(st == ES_OFFLINE ? "Storage not reachable yet: sync starts once it is." :
		          st == ES_E_LOCKED ? std::string(LOCKED_STATUS) : "Starting…");

		{
			std::lock_guard lock(s_worker_lock);
			s_worker_stop = false;
			s_worker_wake = false;
			s_suspend_requested = false;
		}
		es_event_set_notifier(s_client, &OnEventsWaiting, nullptr);
		s_worker = std::thread(&WorkerLoop);

		// A game already running, or still booting, keeps its cards: follow them without touching
		// them. Booting counts too: sync starts on its own thread, and when it is ready only after the
		// boot's FileMcd_Reopen (OnCardsReopening found no client then), nothing else would attach the
		// cards for that whole session. A stopping VM does not: its shutdown may already have called
		// OnVMShutdown(), and nothing would end a session begun now.
		const VMState state = VMManager::GetState();
		if (state != VMState::Shutdown && state != VMState::Stopping)
			FollowRunningVM();
		es_sync_now(s_client);
		Console.WriteLn("(MemcardSync) Syncing memory cards with %s", config.remote_uri.c_str());
		return true;
	}

	void Stop(u32 timeout_ms)
	{
		std::lock_guard lifecycle(s_lifecycle_lock);
		std::unique_lock client(s_client_lock);
		StopLocked(timeout_ms);
	}

	bool IsRunning()
	{
		std::shared_lock client(s_client_lock);
		return s_client != nullptr;
	}

	std::string GetStatus()
	{
		std::lock_guard lock(s_state_lock);
		return s_status;
	}

	void SyncNow()
	{
		std::shared_lock client(s_client_lock);
		if (s_client)
			es_sync_now(s_client);
	}

	void SetAppBackgrounded(bool backgrounded)
	{
		s_backgrounded.store(backgrounded, std::memory_order_release);
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return;
		if (backgrounded)
		{
			{
				std::lock_guard lock(s_worker_lock);
				s_suspend_requested = true;
			}
			s_worker_cv.notify_one();
		}
		else
		{
			es_app_resume(s_client);
		}
	}

	void OnCardsReopening()
	{
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return;

		if (VMManager::GetState() != VMState::Initializing)
		{
			// A reopen under a running game (settings change, card swap, disc change): follow a
			// change of cards, never wait for the network.
			FollowRunningVM();
			return;
		}

		EndSession(); // a session left by a boot that never reached OnVMShutdown()
		const Mounted m = RegisterSlots();
		es_session_info info;
		const es_session_id id = BeginSession(m, BOOT_SYNC_TIMEOUT_MS, &info);
		{
			std::lock_guard lock(s_state_lock);
			s_mounted = m;
			s_session = id;
			s_session_started = std::chrono::steady_clock::now();
		}
		if (id != 0 && info.units_applied > 0)
		{
			Console.WriteLn("(MemcardSync) %u save(s) updated from other devices before boot.", info.units_applied);
			SetStatus(fmt::format("Updated {} save(s) from other devices at {}.", info.units_applied, TimeOfDay()));
		}
		else if (id != 0 && !info.remote_reached)
		{
			Console.Warning("(MemcardSync) Storage not reached before boot; starting with the cards as they are.");
		}
	}

	bool IsSyncing(uint slot)
	{
		if (slot >= SYNCED_SLOTS)
			return false;
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return false;
		std::lock_guard lock(s_state_lock);
		return s_mounted.ids[slot] != 0;
	}

	void OnCardWritten(uint slot)
	{
		if (slot >= SYNCED_SLOTS)
			return;
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return;
		es_container_id id;
		{
			std::lock_guard lock(s_state_lock);
			id = s_mounted.ids[slot];
		}
		if (id != 0)
			es_container_touched(s_client, id);
	}

	void OnCardsFlushed()
	{
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return;
		Mounted m;
		{
			std::lock_guard lock(s_state_lock);
			m = s_mounted;
		}
		for (const es_container_id id : m.ids)
		{
			if (id != 0)
				es_container_flushed(s_client, id);
		}
	}

	void OnVMShutdown()
	{
		std::shared_lock client(s_client_lock);
		if (!s_client)
			return;
		EndSession();
		std::lock_guard lock(s_state_lock);
		s_mounted = {};
	}
} // namespace MemcardSync

#endif // ARMSX2_HAS_EMUSYNC
