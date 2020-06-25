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

#include "scriptmanager.h"

#include "actions.h"
#include "chat.h"
#include "talkaction.h"
#include "spells.h"
#include "movement.h"
#include "weapons.h"
#include "globalevent.h"
#include "events.h"
#include "scripts.h"

Spells* g_spells = nullptr;
TalkActions* g_talkActions = nullptr;
MoveEvents* g_moveEvents = nullptr;
Weapons* g_weapons = nullptr;

extern LuaEnvironment g_luaEnvironment;

ScriptingManager::~ScriptingManager()
{
	delete g_weapons;
	delete g_spells;
	delete g_talkActions;
	delete g_moveEvents;
}

bool ScriptingManager::loadScriptSystems()
{
	if (g_luaEnvironment.loadFile("data/global.lua") == -1) {
		std::cout << "[Warning - ScriptingManager::loadScriptSystems] Can not load data/global.lua" << std::endl;
	}

	g_weapons = new Weapons();
	if (!g_weapons) {
		std::cout << "[ScriptingManager::loadScriptSystems] Error while loading g_weapons!" << std::endl;
		return false;
	}
	g_weapons->loadDefaults();
	g_talkActions = new TalkActions();
	if (!g_talkActions) {
		std::cout << "[ScriptingManager::loadScriptSystems] Error while loading g_talkActions!" << std::endl;
		return false;
	}
	g_moveEvents = new MoveEvents();
	if (!g_moveEvents) {
		std::cout << "[ScriptingManager::loadScriptSystems] Error while loading g_moveEvents!" << std::endl;
		return false;
	}
	g_spells = new Spells();
	if (!g_spells) {
		std::cout << "[ScriptingManager::loadScriptSystems] Error while loading g_spells!" << std::endl;
		return false;
	}

	return true;
}
