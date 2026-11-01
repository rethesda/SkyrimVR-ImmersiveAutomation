#include "WindsF4.h"
#include "HttpSwitch.h"
#include "PlugFinder.h"
#include "TPLinkSPHelper.h"
#include "WindSimulation.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>

namespace ImmersiveWindsF4
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using ImmersiveWinds::WindSimSettings;
		using ImmersiveWinds::WindSimulation;

		// the rest of the game state is read every WEATHER_CHECK_INTERVAL_SECONDS, the player's movement more often
		const float WEATHER_CHECK_INTERVAL_SECONDS = 3.0f;
		const float MOTION_CHECK_INTERVAL_SECONDS = 0.25f;
		const int FOCUS_CHECK_INTERVAL_MS = 250;
		const int SIMULATION_STEP_MS = 50;
		// the relay state is sent again after this time, in case a (udp) message got lost or the plug restarted
		const float RELAY_RESEND_INTERVAL_SECONDS = 15.0f;
		// a queued check that did not run after this time means the game stands still (frozen, long loading screen)
		const float GAME_STALLED_SECONDS = 10.0f;
		// faster position changes are teleports (doors, fast travel), not movement (units per second)
		const float MAX_MOVEMENT_SPEED = 4000.0f;

		// same plug name as the Skyrim plugin, so one fan works for both games
		const char * DEFAULT_PLUG_NAME = "ImmersiveWindsPlug";

		const char * INI_INTERNAL = "Internal";
		const char * INI_GENERAL = "GeneralModConfig";
		const char * INI_HTTP = "HttpSwitch";
		const char * INI_SIM = "SimulatedWind";

		// menus that pause the game
		const std::unordered_set<std::string> PAUSING_MENUS{
			"BarterMenu",
			"BookMenu",
			"Console",
			"ContainerMenu",
			"CookingMenu",
			"CreditsMenu",
			"ExamineMenu",
			"LevelUpMenu",
			"LoadingMenu",
			"LooksMenu",
			"MainMenu",
			"MessageBoxMenu",
			"PauseMenu",
			"SleepWaitMenu",
		};

		// ---- settings ----

		bool _logsEnabled = false;
		bool _noWindInMenu = false;
		bool _writeSimCsv = false;

		bool _useHttpSwitch = false;
		std::string _plugName = DEFAULT_PLUG_NAME;
		std::string _httpMethod = "GET";
		std::string _httpOnUrl;
		std::string _httpOffUrl;

		WindSimSettings _simSettings;
		float _gameWindScale = 1.0f;
		float _outsideMinWind = 0.1f;
		float _insideWind = 0.0f;
		float _motionStartSpeed = 150.0f;
		float _motionFullSpeed = 1000.0f;
		float _motionWindMax = 0.6f;

		struct FloatSetting
		{
			const char * key;
			float defaultValue;
			float * value;
		};

		// [SimulatedWind], the same keys and defaults as the Skyrim plugin, plus fInsideWind
		const FloatSetting SIM_SETTINGS[] = {
			{ "fGameWindScale", 1.0f, &_gameWindScale },
			{ "fOutsideMinWind", 0.1f, &_outsideMinWind },
			{ "fInsideWind", 0.0f, &_insideWind },
			{ "fTurbulence", 0.25f, &_simSettings.turbulence },
			{ "fTurbulenceSeconds", 4.0f, &_simSettings.turbulenceSeconds },
			{ "fGustsPerMinute", 3.0f, &_simSettings.gustsPerMinute },
			{ "fGustMinSeconds", 3.0f, &_simSettings.gustMinSeconds },
			{ "fGustMaxSeconds", 8.0f, &_simSettings.gustMaxSeconds },
			{ "fGustStrength", 0.6f, &_simSettings.gustStrength },
			{ "fLullShare", 0.25f, &_simSettings.lullShare },
			{ "fMotionStartSpeed", 150.0f, &_motionStartSpeed },
			{ "fMotionFullSpeed", 1000.0f, &_motionFullSpeed },
			{ "fMotionWindMax", 0.6f, &_motionWindMax },
			{ "fFanSpinUpSeconds", 1.5f, &_simSettings.fanSpinUpSeconds },
			{ "fFanSpinDownSeconds", 4.0f, &_simSettings.fanSpinDownSeconds },
			{ "fHysteresis", 0.05f, &_simSettings.hysteresis },
			{ "fMinOnSeconds", 1.0f, &_simSettings.minOnSeconds },
			{ "fMinOffSeconds", 1.0f, &_simSettings.minOffSeconds },
		};

		// ---- state shared between the threads ----

		std::atomic<bool> _keepRunning(true);
		std::atomic<bool> _started(false);
		std::atomic<bool> _scanFinished(false);
		std::atomic<bool> _checkPending(false);
		std::atomic<bool> _fullCheckWanted(false);
		std::atomic<Clock::rep> _checkQueuedAt(0);
		// no wind until the next game state check: menu, game in the background or standing still
		std::atomic<bool> _windPaused(false);
		std::atomic<bool> _menuOpen(false);
		// simulated wind 0-1, written by the game state check, read by the simulation thread
		std::atomic<float> _baseWind(0.0f);
		std::atomic<float> _motionWind(0.0f);
		// 0 nothing switched yet, 1 last switch worked, -1 it failed
		std::atomic<int> _switchResult(0);

		// workingIp and useUDP are written by the scan thread and read by the switch thread
		std::mutex _connectionMutex;
		std::string _workingIp;
		int _useUdp = -1;
		Clock::time_point _lastRelaySend;

		// ---- game state (main thread) ----

		RE::NiPoint3 _lastPosition;
		RE::TESObjectCELL * _lastCell = nullptr;
		Clock::time_point _lastPositionTime;
		std::uint32_t _lastWeatherId = 0;
		bool _lastInterior = false;
	}

	// ---- helpers ----

	void Log(const char * fmt, ...)
	{
		if (!_logsEnabled) {
			return;
		}
		char buffer[4096];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		spdlog::info("{}", buffer);
	}

	float SecondsSince(const Clock::time_point & start)
	{
		return std::chrono::duration<float>(Clock::now() - start).count();
	}

	std::string Trim(const std::string & text)
	{
		const size_t begin = text.find_first_not_of(" \t");
		if (begin == std::string::npos) {
			return "";
		}
		return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
	}

	std::string GetIniPath()
	{
		const auto dir = F4SE::log::log_directory();
		return dir ? (*dir / "immersiveWinds.ini").string() : "immersiveWinds.ini";
	}

	std::string ReadIniString(const char * section, const char * key, const char * defaultValue, const std::string & path)
	{
		char buffer[4096];
		GetPrivateProfileStringA(section, key, defaultValue, buffer, sizeof(buffer), path.c_str());
		return buffer;
	}

	bool ReadIniBool(const char * section, const char * key, bool defaultValue, const std::string & path)
	{
		return GetPrivateProfileIntA(section, key, defaultValue ? 1 : 0, path.c_str()) > 0;
	}

	float ReadIniFloat(const char * section, const char * key, float defaultValue, const std::string & path)
	{
		return strtof(ReadIniString(section, key, std::to_string(defaultValue).c_str(), path).c_str(), nullptr);
	}

	void WriteIniIfMissing(const char * section, const char * key, const std::string & value, const std::string & path)
	{
		const char * missing = "<missing>";
		if (ReadIniString(section, key, missing, path) == missing) {
			WritePrivateProfileStringA(section, key, value.c_str(), path.c_str());
		}
	}

	// ---- settings ----

	// Adds every missing setting with its default value, existing values are kept.
	void WriteDefaultsToIniFile()
	{
		const std::string path = GetIniPath();
		if (ReadIniString(INI_INTERNAL, "sModVersion", "", path) != MOD_VERSION) {
			WritePrivateProfileStringA(INI_INTERNAL, "sModVersion", MOD_VERSION.c_str(), path.c_str());
		}
		WriteIniIfMissing(INI_INTERNAL, "sTpLinkPlugIp", "", path);
		WriteIniIfMissing(INI_GENERAL, "sSwitchType", "tplink", path);
		WriteIniIfMissing(INI_GENERAL, "sHomeAssistantUrl", "", path);
		WriteIniIfMissing(INI_GENERAL, "sPlugName", DEFAULT_PLUG_NAME, path);
		WriteIniIfMissing(INI_GENERAL, "bUseUdp", "1", path); // read by TPLinkHelper
		WriteIniIfMissing(INI_GENERAL, "bLoggingEnabled", "0", path);
		WriteIniIfMissing(INI_GENERAL, "bNoWindInMenu", "0", path);
		WriteIniIfMissing(INI_HTTP, "sHttpMethod", "GET", path);
		WriteIniIfMissing(INI_HTTP, "sOnUrl", "", path);
		WriteIniIfMissing(INI_HTTP, "sOffUrl", "", path);
		for (const FloatSetting & setting : SIM_SETTINGS) {
			WriteIniIfMissing(INI_SIM, setting.key, std::to_string(setting.defaultValue), path);
		}
		WriteIniIfMissing(INI_SIM, "bWriteCsv", "0", path);
	}

	void LoadSettings()
	{
		const std::string path = GetIniPath();
		_logsEnabled = ReadIniBool(INI_GENERAL, "bLoggingEnabled", false, path);
		_noWindInMenu = ReadIniBool(INI_GENERAL, "bNoWindInMenu", false, path);
		_writeSimCsv = ReadIniBool(INI_SIM, "bWriteCsv", false, path);

		_useHttpSwitch = _stricmp(Trim(ReadIniString(INI_GENERAL, "sSwitchType", "tplink", path)).c_str(), "http") == 0;
		_plugName = Trim(ReadIniString(INI_GENERAL, "sPlugName", DEFAULT_PLUG_NAME, path));
		if (_plugName.empty()) {
			_plugName = DEFAULT_PLUG_NAME;
		}
		_httpMethod = Trim(ReadIniString(INI_HTTP, "sHttpMethod", "GET", path));
		_httpOnUrl = Trim(ReadIniString(INI_HTTP, "sOnUrl", "", path));
		_httpOffUrl = Trim(ReadIniString(INI_HTTP, "sOffUrl", "", path));
		const std::string homeAssistant = Trim(ReadIniString(INI_GENERAL, "sHomeAssistantUrl", "", path));
		if (!homeAssistant.empty()) {
			_useHttpSwitch = true;
			_httpMethod = "POST";
			_httpOnUrl = HttpSwitch::HomeAssistantUrl(homeAssistant, "ImmersiveWinds", true);
			_httpOffUrl = HttpSwitch::HomeAssistantUrl(homeAssistant, "ImmersiveWinds", false);
		}
		if (_useHttpSwitch) {
			spdlog::info("Using the http switch: {} {} / {}", _httpMethod, _httpOnUrl, _httpOffUrl);
		}

		for (const FloatSetting & setting : SIM_SETTINGS) {
			*setting.value = std::max(0.0f, ReadIniFloat(INI_SIM, setting.key, setting.defaultValue, path));
		}
		_motionFullSpeed = std::max(_motionFullSpeed, _motionStartSpeed + 1.0f);
	}

	// ---- plug ----

	bool LoadConnectionConfig(TPLinkHelper & tpLinkConn)
	{
		std::lock_guard<std::mutex> lock(_connectionMutex);
		return tpLinkConn.LoadIPConfigFromFile(GetIniPath(), "sTpLinkPlugIp", _useUdp, _workingIp);
	}

	void ScanThread()
	{
		// a http switch is addressed by its urls, there is nothing to search
		if (!_useHttpSwitch) {
			const std::string ip = FindTpLinkPlug(GetIniPath(), "sTpLinkPlugIp", _plugName, _logsEnabled, _keepRunning);
			if (!ip.empty()) {
				std::lock_guard<std::mutex> lock(_connectionMutex);
				_workingIp = ip;
			}
		}
		_scanFinished = true;
	}

	bool SwitchPlug(bool on)
	{
		bool worked = false;
		if (_useHttpSwitch) {
			worked = HttpSwitch::Request(_httpMethod, on ? _httpOnUrl : _httpOffUrl, _logsEnabled);
		}
		else {
			TPLinkHelper tpLinkConn(_logsEnabled);
			worked = LoadConnectionConfig(tpLinkConn) && tpLinkConn.SwitchRelayState(on ? 1 : 0);
		}
		_switchResult = worked ? 1 : -1;
		return worked;
	}

	void SendRelayState(bool on)
	{
		if (!SwitchPlug(on)) {
			Log("Switching the plug %s failed", on ? "on" : "off");
		}
		_lastRelaySend = Clock::now();
	}

	// Switches the plug so the fan follows the simulated wind.
	void SimulationThread()
	{
		while (!_scanFinished && _keepRunning) {
			Sleep(100);
		}

		WindSimulation simulation(_simSettings, std::random_device()());
		std::ofstream csv;
		if (_writeSimCsv) {
			const auto dir = F4SE::log::log_directory();
			csv.open(dir ? (*dir / "ImmersiveWindsSim.csv").string() : "ImmersiveWindsSim.csv");
			csv << "seconds,base,motion,target,fan,relay\n";
		}

		const auto start = Clock::now();
		auto lastStep = start;
		bool relayOn = false;
		SendRelayState(false);

		while (_keepRunning) {
			const auto now = Clock::now();
			const float dt = std::chrono::duration<float>(now - lastStep).count();
			lastStep = now;

			const bool paused = _windPaused;
			const float base = paused ? 0.0f : _baseWind.load();
			const float motion = paused ? 0.0f : _motionWind.load();
			const bool on = simulation.Update(dt, base, motion);

			if (on != relayOn) {
				relayOn = on;
				SendRelayState(on);
			}
			else if (SecondsSince(_lastRelaySend) >= RELAY_RESEND_INTERVAL_SECONDS) {
				SendRelayState(on);
			}

			if (csv.is_open()) {
				char line[128];
				snprintf(line, sizeof(line), "%.2f,%.3f,%.3f,%.3f,%.3f,%d\n", SecondsSince(start), base, motion, simulation.Target(), simulation.FanSpeed(), relayOn ? 1 : 0);
				csv << line << std::flush;
			}
			Sleep(SIMULATION_STEP_MS);
		}
	}

	// ---- game state ----

	// CommonLibF4 only knows the address of the original game; the second id is the one of the next gen update
	RE::Sky * GetSky()
	{
		using func_t = RE::Sky * (*)();
		static REL::Relocation<func_t> func{ REL::RelocationID(484694, 2192448) };
		return func();
	}

	// no wind until the next game state check, see _windPaused
	void PauseWind()
	{
		_windPaused = true;
	}

	// Tells the player once per game start if the switch works, so nobody has to read the log for that.
	void ShowSwitchResult()
	{
		static bool shown = false;
		const int result = _switchResult;
		if (shown || result == 0) {
			return;
		}
		shown = true;
		RE::SendHUDMessage::ShowHUDMessage(result > 0 ? "Immersive Winds: connected" : "Immersive Winds: no connection, see the log", "", false, result < 0);
	}

	// Base wind: outside the wind of the weather in the game, inside fInsideWind.
	void UpdateBaseWind(RE::PlayerCharacter * player)
	{
		const auto cell = player->GetParentCell();
		auto sky = GetSky();
		if (!cell || !sky) {
			return;
		}
		ShowSwitchResult();

		const bool interior = cell->IsInterior();
		// 0-1 (the weather records store 0-255, in case a game version keeps that scale)
		const float gameWind = sky->windSpeed > 1.0f ? sky->windSpeed / 255.0f : sky->windSpeed;
		const float wind = interior ? _insideWind : _outsideMinWind + _gameWindScale * gameWind;
		_baseWind = std::clamp(wind, 0.0f, 1.0f);
		_windPaused = false;

		const std::uint32_t weatherId = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
		if (weatherId != _lastWeatherId || interior != _lastInterior) {
			_lastWeatherId = weatherId;
			_lastInterior = interior;
			Log("%s, weather %08X, wind in the game %.2f, base wind %.2f", interior ? "inside" : "outside", weatherId, gameWind, _baseWind.load());
		}
	}

	// The wind of the player's own movement (running, vertibird, falling), from the change of the position.
	void UpdateMotion(RE::PlayerCharacter * player)
	{
		const auto now = Clock::now();
		const float seconds = SecondsSince(_lastPositionTime);
		const RE::NiPoint3 position = player->GetPosition();
		const auto cell = player->GetParentCell();

		// an interior cell change is a door; after a long pause (menu, loading) the old position says nothing
		const bool samePlace = cell == _lastCell || (cell && _lastCell && cell->IsExterior() && _lastCell->IsExterior());
		if (samePlace && seconds > 0.0f && seconds < 1.0f) {
			const RE::NiPoint3 delta = position - _lastPosition;
			const float speed = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / seconds;
			if (speed < MAX_MOVEMENT_SPEED) {
				const float share = std::clamp((speed - _motionStartSpeed) / (_motionFullSpeed - _motionStartSpeed), 0.0f, 1.0f);
				_motionWind = share * _motionWindMax;
			}
		}
		else {
			_motionWind = 0.0f;
		}
		_lastPosition = position;
		_lastCell = cell;
		_lastPositionTime = now;
	}

	// The queued check on the main thread.
	void RunGameStateCheck()
	{
		const bool full = _fullCheckWanted.exchange(false);
		if (_menuOpen && _noWindInMenu) {
			PauseWind();
			return;
		}
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return;
		}
		// after a pause the whole state is read again
		if (full || _windPaused) {
			UpdateBaseWind(player);
		}
		UpdateMotion(player);
	}

	// Queues RunGameStateCheck on the main thread. Returns false if a check is already queued.
	bool QueueGameStateCheck(bool full)
	{
		if (full) {
			_fullCheckWanted = true; // also for a check that is already queued
		}
		// only one check at a time, so tasks do not pile up while the game does not process them (loading screens)
		if (_checkPending.exchange(true)) {
			return false;
		}
		_checkQueuedAt = Clock::now().time_since_epoch().count();
		F4SE::GetTaskInterface()->AddTask([]() {
			RunGameStateCheck();
			_checkPending = false;
		});
		return true;
	}

	float SecondsSinceCheckQueued()
	{
		return SecondsSince(Clock::time_point(Clock::duration(_checkQueuedAt.load())));
	}

	// Fallout 4 VR keeps running without window focus, so it always counts as in the foreground there.
	bool IsGameInForeground()
	{
		if (REL::Module::IsVR()) {
			return true;
		}
		DWORD processId = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &processId);
		return processId == GetCurrentProcessId();
	}

	// Background timer: watches the window focus and queues the game state checks.
	void GameStateThread()
	{
		Clock::time_point lastCheck;
		Clock::time_point lastFullCheck;
		while (_keepRunning) {
			if (!IsGameInForeground()) {
				PauseWind();
				lastCheck = {}; // reads the game state right when the game is back
				lastFullCheck = {};
			}
			else if (_menuOpen && _noWindInMenu) {
				// also handled here, so the fan stops even if the game does not process tasks while the menu is open
				PauseWind();
			}
			else if (SecondsSince(lastCheck) >= MOTION_CHECK_INTERVAL_SECONDS) {
				lastCheck = Clock::now();
				const bool full = SecondsSince(lastFullCheck) >= WEATHER_CHECK_INTERVAL_SECONDS;
				if (full) {
					lastFullCheck = lastCheck;
				}
				// a check that is still queued after GAME_STALLED_SECONDS means the game stands still
				if (!QueueGameStateCheck(full) && SecondsSinceCheckQueued() >= GAME_STALLED_SECONDS && !_windPaused) {
					Log("Game stands still, stopping the fan.");
					PauseWind();
				}
			}
			Sleep(FOCUS_CHECK_INTERVAL_MS);
		}
	}

	// Tracks the open menus that pause the game.
	class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent & a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override
		{
			const std::string name = a_event.menuName.c_str();
			if (PAUSING_MENUS.contains(name)) {
				if (a_event.opening) {
					_open.insert(name);
				}
				else {
					_open.erase(name);
				}
				_menuOpen = !_open.empty();
				// the menu state counts right away, not only with the next timer check
				QueueGameStateCheck(true);
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		std::unordered_set<std::string> _open;
	};

	MenuWatcher _menuWatcher;

	void StartMod()
	{
		if (_started.exchange(true)) {
			return;
		}
		WriteDefaultsToIniFile();
		LoadSettings();
		spdlog::info("Immersive Winds {} started, {}", MOD_VERSION, REL::Module::IsVR() ? "Fallout 4 VR" : "Fallout 4");

		if (auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&_menuWatcher);
		}
		std::thread(ScanThread).detach();
		std::thread(SimulationThread).detach();
		std::thread(GameStateThread).detach();
	}

	void SwitchOffOnExit()
	{
		_windPaused = true;
		_keepRunning = false;
		if (_started) {
			SwitchPlug(false);
		}
	}

	namespace
	{
		// Switches the plug off when the game exits. Defined last in this file on purpose: objects of one
		// file are destroyed in reverse order, so the settings and connection data above still exist then.
		struct ExitHandler
		{
			~ExitHandler() { SwitchOffOnExit(); }
		} _exitHandler;
	}
}
