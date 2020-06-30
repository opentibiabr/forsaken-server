/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2020  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "server.h"

#include "game.h"

#include "modules.h"
#include "iomarket.h"

#include "configmanager.h"
#include "rsa.h"
#include "protocollogin.h"
#include "protocolstatus.h"
#include "databasemanager.h"
#include "scheduler.h"
#include "databasetasks.h"
#include "scripts.h"
#include <fstream>

LuaEnvironment g_luaEnvironment;

std::mutex g_loaderLock;
std::condition_variable g_loaderSignal;
std::unique_lock<std::mutex> g_loaderUniqueLock(g_loaderLock);

void startupErrorMessage(const std::string& errorStr)
{
	spdlog::error(errorStr);
	g_loaderSignal.notify_all();
}

void mainLoader(int argc, char* argv[], ServiceManager* services);

[[noreturn]] void badAllocationHandler()
{
	// Use functions that only use stack allocation
	puts("Allocation failed, server out of memory.\nDecrease the size of your map or compile in 64 bits mode.\n");
	getchar();
	exit(-1);
}

#ifndef UNIT_TESTING
int main(int argc, char* argv[])
{
	// Setup bad allocation handler
	std::set_new_handler(badAllocationHandler);

	if (!g_database().init()) {
		return 1;
	}

	ServiceManager serviceManager;

	g_dispatcher().start();
	g_scheduler().start();

	#ifdef NDEBUG
		spdlog::set_level(spdlog::level::debug);
		spdlog::debug("You're running a debug version.");
	#else
		spdlog::info("You're running a release version.");
	#endif

	g_dispatcher().addTask(std::bind(mainLoader, argc, argv, &serviceManager));

	g_loaderSignal.wait(g_loaderUniqueLock);

	if (serviceManager.is_running()) {
    spdlog::info("{} {}", g_config().getString(ConfigManager::SERVER_NAME), "Server Online!");
		serviceManager.run();
	} else {
    spdlog::error("No services running. The server is NOT online.");
		g_scheduler().shutdown();
		g_databaseTasks().shutdown();
		g_dispatcher().shutdown();
	}

	g_scheduler().join();
	g_databaseTasks().join();
	g_dispatcher().join();
	g_database().end();
	return 0;
}
#endif

void mainLoader(int, char*[], ServiceManager* services)
{
	//dispatcher thread
	g_game().setGameState(GAME_STATE_STARTUP);

	srand(static_cast<unsigned int>(OTSYS_TIME()));
#ifdef _WIN32
	SetConsoleTitle(STATUS_SERVER_NAME);
#endif
	spdlog::info("{} - Version {}", STATUS_SERVER_NAME, STATUS_SERVER_VERSION);
	spdlog::info("Compiled with {}", BOOST_COMPILER);

	std::string platform;
	#if defined(__amd64__) || defined(_M_X64)
		platform = "x64";
	#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
		platform = "x86";
	#elif defined(__arm__)
		platform = "ARM";
	#else
		platform = "unknown";
	#endif

	spdlog::info("Compiled on {} {} for platform {}\n", __DATE__, __TIME__, platform);

	spdlog::info("A server developed by {}", STATUS_SERVER_DEVELOPERS);
	spdlog::info("Visit our forum for updates, support, and resources: {}.", "https://github.com/opentibiabr/canary-server");
	spdlog::info("Server protocol: {}.{}\n", CLIENT_VERSION_UPPER, CLIENT_VERSION_LOWER);

	// check if config.lua or config.lua.dist exist
	std::ifstream c_test("./config.lua");
	if (!c_test.is_open()) {
		std::ifstream config_lua_dist("./config.lua.dist");
		if (config_lua_dist.is_open()) {
			spdlog::warn("Copying config.lua.dist to config.lua");
			std::ofstream config_lua("config.lua");
			config_lua << config_lua_dist.rdbuf();
			config_lua.close();
			config_lua_dist.close();
		}
	} else {
		c_test.close();
	}

	// read global config
	spdlog::info("Loading config");
	if (!g_config().load()) {
		startupErrorMessage("Unable to load config.lua!");
		return;
	}

#ifdef _WIN32
	const std::string& defaultPriority = g_config().getString(ConfigManager::DEFAULT_PRIORITY);
	if (strcasecmp(defaultPriority.c_str(), "high") == 0) {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	} else if (strcasecmp(defaultPriority.c_str(), "above-normal") == 0) {
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	}
#endif

	//set RSA key
	const char* n("109120132967399429278860960508995541528237502902798129123468757937266291492576446330739696001110603907230888610072655818825358503429057592827629436413108566029093628212635953836686562675849720620786279431090218017681061521755056710823876476444260558147179707119674283982419152118103759076030616683978566631413");
	const char* d("46730330223584118622160180015036832148732986808519344675210555262940258739805766860224610646919605860206328024326703361630109888417839241959507572247284807035235569619173792292786907845791904955103601652822519121908367187885509270025388641700821735345222087940578381210879116823013776808975766851829020659073");
	g_RSA().setKey(n, d);

	spdlog::info("Establishing database connection...");
	if (!g_database().connect()) {
		startupErrorMessage("Failed to connect to database.");
		return;
	}

	spdlog::info("Connected with MYSQL {}", Database::getClientVersion());
	if (g_database().getMaxPacketSize() < 104857600) {
		spdlog::warn("Max MYSQL Query size below 100MB might generate undefined behaviour.");
		spdlog::warn("Do you want to continue? Press enter to continue.");
		getchar();
	}

	// run database manager
	spdlog::info("Running database manager!");

	if (!DatabaseManager::isDatabaseSetup()) {
		startupErrorMessage("The database you have specified in config.lua is empty, please import the schema.sql to your database.");
		return;
	}
	g_databaseTasks().start();

	DatabaseManager::updateDatabase();
	if (g_config().getBoolean(ConfigManager::OPTIMIZE_DATABASE) && !DatabaseManager::optimizeTables()) {
		spdlog::warn("No tables were optimized.");
	}

	//load vocations
	spdlog::info("Loading vocations");
	if (!g_vocations().loadFromXml()) {
		startupErrorMessage("Unable to load vocations!");
		return;
	}

	// load item data
	spdlog::info("Loading items from OTB");
	if (!Item::items.loadFromOtb("data/items/items.otb")) {
		startupErrorMessage("Unable to load items (OTB)!");
		return;
	}

	spdlog::info("Loading items from XML");
	if (!Item::items.loadFromXml()) {
		startupErrorMessage("Unable to load items (XML)!");
		return;
	}

	spdlog::info("Loading global.lua");
	if (g_luaEnvironment.loadFile("data/global.lua") == -1) {
		spdlog::warn("Cannot load data/global.lua");
	}

	spdlog::info("Loading lua libs");
	if (!g_scripts().loadScripts("scripts/lib", true, false)) {
		startupErrorMessage("Unable to load lua libs!");
		return;
	}

	spdlog::info("Loading modules");
	if (!g_modules().load()) {
		startupErrorMessage("Failed to load modules");
		return;
	}

	spdlog::info("Loading lua scripts");
	if (!g_scripts().loadScripts("scripts", false, false)) {
		startupErrorMessage("Failed to load lua scripts");
		return;
	}

	spdlog::info("Loading monsters");
	if (!g_monsters().loadFromXml()) {
		startupErrorMessage("Unable to load monsters!");
		return;
	}

	spdlog::info("Loading lua monsters scripts");
	if (!g_scripts().loadScripts("monster", false, false)) {
		startupErrorMessage("Failed to load lua monsters scripts");
		return;
	}

	spdlog::info("Loading outfits");
	if (!Outfits::getInstance().loadFromXml()) {
		startupErrorMessage("Unable to load outfits!");
		return;
	}

	std::string worldType = asLowerCaseString(g_config().getString(ConfigManager::WORLD_TYPE));
	if (!tfs_strcmp(worldType.c_str(), "pvp")) {
		g_game().setWorldType(WORLD_TYPE_PVP);
	} else if (!tfs_strcmp(worldType.c_str(), "no-pvp")) {
		g_game().setWorldType(WORLD_TYPE_NO_PVP);
	} else if (!tfs_strcmp(worldType.c_str(), "pvp-enforced")) {
		g_game().setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		std::string str;
		str.reserve(64);
		str.append("Unknown world type: ")
			.append(g_config().getString(ConfigManager::WORLD_TYPE))
			.append(", valid world types are: pvp, no-pvp and pvp-enforced.");
		startupErrorMessage(str);
		return;
	}
	spdlog::info("World type set as {}!", asUpperCaseString(worldType));

	spdlog::info("Loading map");
	if (!g_game().loadMainMap(g_config().getString(ConfigManager::MAP_NAME))) {
		startupErrorMessage("Failed to load map");
		return;
	}

	spdlog::info("Initializing gamestate");
	g_game().setGameState(GAME_STATE_INIT);

	// Game client protocols
	services->add<ProtocolGame>(static_cast<uint16_t>(g_config().getNumber(ConfigManager::GAME_PORT)));
	services->add<ProtocolLogin>(static_cast<uint16_t>(g_config().getNumber(ConfigManager::LOGIN_PORT)));

	// OT protocols
	services->add<ProtocolStatus>(static_cast<uint16_t>(g_config().getNumber(ConfigManager::STATUS_PORT)));

	RentPeriod_t rentPeriod;
	std::string strRentPeriod = asLowerCaseString(g_config().getString(ConfigManager::HOUSE_RENT_PERIOD));
	if (!tfs_strcmp(strRentPeriod.c_str(), "yearly")) {
		rentPeriod = RENTPERIOD_YEARLY;
	} else if (!tfs_strcmp(strRentPeriod.c_str(), "weekly")) {
		rentPeriod = RENTPERIOD_WEEKLY;
	} else if (!tfs_strcmp(strRentPeriod.c_str(), "monthly")) {
		rentPeriod = RENTPERIOD_MONTHLY;
	} else if (!tfs_strcmp(strRentPeriod.c_str(), "daily")) {
		rentPeriod = RENTPERIOD_DAILY;
	} else {
		rentPeriod = RENTPERIOD_NEVER;
	}

	g_game().map.houses.payHouses(rentPeriod);

#if GAME_FEATURE_MARKET > 0
	IOMarket::checkExpiredOffers();
	IOMarket::getInstance().updateStatistics();
#endif

	spdlog::info("Loaded all modules, server starting up...");

#ifndef _WIN32
	if (getuid() == 0 || geteuid() == 0) {
		spdlog::warn("{} has been executed as root user, please consider running it as a normal user.", STATUS_SERVER_NAME);
	}
#endif

	g_game().start(services);
	g_game().setGameState(GAME_STATE_NORMAL);
	g_loaderSignal.notify_all();
}
