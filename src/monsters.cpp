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

#include "monsters.h"
#include "monster.h"
#include "spells.h"
#include "combat.h"
#include "weapons.h"
#include "configmanager.h"
#include "game.h"

#include "pugicast.h"

extern Game g_game;

spellBlock_t::~spellBlock_t()
{
	if (combatSpell) {
		delete spell;
	}
}

void MonsterType::loadLoot(MonsterType* monsterType, LootBlock lootBlock)
{
	if (lootBlock.childLoot.empty()) {
		bool isContainer = Item::items[lootBlock.id].isContainer();
		if (isContainer) {
			for (LootBlock child : lootBlock.childLoot) {
				lootBlock.childLoot.push_back(child);
			}
		}
		monsterType->info.lootItems.push_back(lootBlock);
	} else {
		monsterType->info.lootItems.push_back(lootBlock);
	}
}

bool Monsters::loadRaces()
{
	races = {};
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("data/monster/races.xml");
	if (!result) {
		printXMLError("Error - Monsters::loadRaces", "data/monster/races.xml", result);
		return false;
	}

	for (auto raceNode : doc.child("races").children()) {
		pugi::xml_attribute attr;
		if (!(attr = raceNode.attribute("id"))) {
			std::cout << "[Warning - Monsters::loadRaces] Missing race id." << std::endl;
			continue;
		}

		races.emplace(raceNode.attribute("name").as_string(), pugi::cast<uint16_t>(attr.value()));
	}
	return true;
}

bool Monsters::loadFromXml(bool reloading /*= false*/)
{
	if (!loadRaces()) {
		return false;
	}

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("data/monster/monsters.xml");
	if (!result) {
		printXMLError("Error - Monsters::loadFromXml", "data/monster/monsters.xml", result);
		return false;
	}

	loaded = true;

	for (auto monsterNode : doc.child("monsters").children()) {
		std::string name = asLowerCaseString(monsterNode.attribute("name").as_string());
		std::string file = "data/monster/" + std::string(monsterNode.attribute("file").as_string());
		loadMonster(file, name, reloading);

		pugi::xml_attribute attrRaceId = monsterNode.attribute("raceid");
		if (attrRaceId) {
			uint16_t raceId = pugi::cast<uint16_t>(attrRaceId.value());
			monsterRaces[raceId][pugi::cast<uint16_t>(monsterNode.attribute("id").value())] = monsterNode.attribute("name").as_string();
		}
	}
	return true;
}

bool Monsters::reload()
{
	loaded = false;

	scriptInterface.reset();

	return loadFromXml(true);
}

ConditionDamage* Monsters::getDamageCondition(ConditionType_t conditionType,
        int32_t maxDamage, int32_t minDamage, int32_t startDamage, uint32_t tickInterval)
{
	ConditionDamage* condition = static_cast<ConditionDamage*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, 0, 0));
	condition->setParam(CONDITION_PARAM_TICKINTERVAL, tickInterval);
	condition->setParam(CONDITION_PARAM_MINVALUE, minDamage);
	condition->setParam(CONDITION_PARAM_MAXVALUE, maxDamage);
	condition->setParam(CONDITION_PARAM_STARTVALUE, startDamage);
	condition->setParam(CONDITION_PARAM_DELAYED, 1);
	return condition;
}

bool Monsters::deserializeSpell(const pugi::xml_node& node, spellBlock_t& sb, const std::string& description)
{
	std::string name;
	std::string scriptName;
	bool isScripted;

	pugi::xml_attribute attr;
	if ((attr = node.attribute("script"))) {
		scriptName = attr.as_string();
		isScripted = true;
	} else if ((attr = node.attribute("name"))) {
		name = attr.as_string();
		isScripted = false;
	} else {
		return false;
	}

	if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
		sb.speed = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
	}

	if ((attr = node.attribute("chance"))) {
		uint32_t chance = pugi::cast<uint32_t>(attr.value());
		if (chance > 100) {
			chance = 100;
		}
		sb.chance = chance;
	}

	if ((attr = node.attribute("range"))) {
		uint32_t range = pugi::cast<uint32_t>(attr.value());
		if (range > (Map::maxViewportX * 2)) {
			range = Map::maxViewportX * 2;
		}
		sb.range = range;
	}

	if ((attr = node.attribute("min"))) {
		sb.minCombatValue = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = node.attribute("max"))) {
		sb.maxCombatValue = pugi::cast<int32_t>(attr.value());

		//normalize values
		if (std::abs(sb.minCombatValue) > std::abs(sb.maxCombatValue)) {
			int32_t value = sb.maxCombatValue;
			sb.maxCombatValue = sb.minCombatValue;
			sb.minCombatValue = value;
		}
	}

	if (auto spell = g_spells().getSpellByName(name)) {
		sb.spell = spell;
		return true;
	}

	CombatSpell* combatSpell = nullptr;
	bool needTarget = false;
	bool needDirection = false;

	if (isScripted) {
		if ((attr = node.attribute("direction"))) {
			needDirection = attr.as_bool();
		}

		if ((attr = node.attribute("target"))) {
			needTarget = attr.as_bool();
		}

		std::unique_ptr<CombatSpell> combatSpellPtr(new CombatSpell(nullptr, needTarget, needDirection));
		if (!combatSpellPtr->loadScript("data/" + g_spells().getScriptBaseName() + "/scripts/" + scriptName)) {
			return false;
		}

		if (!combatSpellPtr->loadScriptCombat()) {
			return false;
		}

		combatSpell = combatSpellPtr.release();
		combatSpell->getCombat()->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
	} else {
		Combat* combat = new Combat;
		if ((attr = node.attribute("length"))) {
			int32_t length = pugi::cast<int32_t>(attr.value());
			if (length > 0) {
				int32_t spread = 3;

				//need direction spell
				if ((attr = node.attribute("spread"))) {
					spread = std::max<int32_t>(0, pugi::cast<int32_t>(attr.value()));
				}

				AreaCombat* area = new AreaCombat();
				area->setupArea(length, spread);
				combat->setArea(area);

				needDirection = true;
			}
		}

		if ((attr = node.attribute("radius"))) {
			int32_t radius = pugi::cast<int32_t>(attr.value());

			//target spell
			if ((attr = node.attribute("target"))) {
				needTarget = attr.as_bool();
			}

			AreaCombat* area = new AreaCombat();
			area->setupArea(radius);
			combat->setArea(area);
		}

		std::string tmpName = asLowerCaseString(name);
		if (!tfs_strcmp(tmpName.c_str(), "melee")) {
			sb.isMelee = true;

			pugi::xml_attribute attackAttribute, skillAttribute;
			if ((attackAttribute = node.attribute("attack")) && (skillAttribute = node.attribute("skill"))) {
				sb.minCombatValue = 0;
				sb.maxCombatValue = -Weapons::getMaxMeleeDamage(pugi::cast<int32_t>(skillAttribute.value()), pugi::cast<int32_t>(attackAttribute.value()));
			}

			ConditionType_t conditionType = CONDITION_NONE;
			int32_t minDamage = 0;
			int32_t maxDamage = 0;
			uint32_t tickInterval = 2000;

			if ((attr = node.attribute("fire"))) {
				conditionType = CONDITION_FIRE;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 9000;
			} else if ((attr = node.attribute("poison"))) {
				conditionType = CONDITION_POISON;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 4000;
			} else if ((attr = node.attribute("energy"))) {
				conditionType = CONDITION_ENERGY;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 10000;
			} else if ((attr = node.attribute("drown"))) {
				conditionType = CONDITION_DROWN;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 5000;
			} else if ((attr = node.attribute("freeze"))) {
				conditionType = CONDITION_FREEZING;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 8000;
			} else if ((attr = node.attribute("dazzle"))) {
				conditionType = CONDITION_DAZZLED;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 10000;
			} else if ((attr = node.attribute("curse"))) {
				conditionType = CONDITION_CURSED;

				minDamage = pugi::cast<int32_t>(attr.value());
				maxDamage = minDamage;
				tickInterval = 4000;
			} else if ((attr = node.attribute("bleed")) || (attr = node.attribute("physical"))) {
				conditionType = CONDITION_BLEEDING;
				tickInterval = 4000;
			}

			if ((attr = node.attribute("tick"))) {
				int32_t value = pugi::cast<int32_t>(attr.value());
				if (value > 0) {
					tickInterval = value;
				}
			}

			if (conditionType != CONDITION_NONE) {
				Condition* condition = getDamageCondition(conditionType, maxDamage, minDamage, 0, tickInterval);
				combat->addCondition(condition);
			}

			sb.range = 1;
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setParam(COMBAT_PARAM_BLOCKSHIELD, 1);
			combat->setOrigin(ORIGIN_MELEE);
		} else if (!tfs_strcmp(tmpName.c_str(), "physical")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setOrigin(ORIGIN_RANGED);
		} else if (!tfs_strcmp(tmpName.c_str(), "bleed")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "poison") || !tfs_strcmp(tmpName.c_str(), "earth")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_EARTHDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "fire")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_FIREDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "energy")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_ENERGYDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "drown")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_DROWNDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "ice")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_ICEDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "holy")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_HOLYDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "death")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_DEATHDAMAGE);
		} else if (!tfs_strcmp(tmpName.c_str(), "lifedrain")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_LIFEDRAIN);
		} else if (!tfs_strcmp(tmpName.c_str(), "manadrain")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_MANADRAIN);
		} else if (!tfs_strcmp(tmpName.c_str(), "healing")) {
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_HEALING);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
		} else if (!tfs_strcmp(tmpName.c_str(), "speed")) {
			int32_t minSpeedChange = 0;
			int32_t maxSpeedChange = 0;
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			if ((attr = node.attribute("speedchange"))) {
				minSpeedChange = pugi::cast<int32_t>(attr.value());
				maxSpeedChange = minSpeedChange;
			} else if ((attr = node.attribute("minspeedchange"))) {
				minSpeedChange = pugi::cast<int32_t>(attr.value());
				maxSpeedChange = pugi::cast<int32_t>(node.attribute("maxspeedchange").value());
				if (maxSpeedChange == 0) {
					maxSpeedChange = minSpeedChange; // static speedchange value
				}
			}

			//cant be slower than 100%
			if (minSpeedChange < -1000) {
				minSpeedChange = -1000;
			}
			if (maxSpeedChange < -1000) {
				maxSpeedChange = -1000;
			}

			ConditionType_t conditionType;
			if ((minSpeedChange < 0) == (maxSpeedChange < 0)) {
				if (minSpeedChange > 0) {
					conditionType = CONDITION_HASTE;
					combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
				} else {
					conditionType = CONDITION_PARALYZE;
				}
			} else {
				std::cout << "[Error - Monsters::deserializeSpell] - " << description << " - can't determine condition type because minSpeedChange/maxSpeedChange sign mismatch" << std::endl;
				delete combat;
				return false;
			}

			ConditionSpeed* condition = static_cast<ConditionSpeed*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, duration, 0));
			condition->setFormulaVars(minSpeedChange / 1000.0, 0, maxSpeedChange / 1000.0, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "outfit")) {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			if ((attr = node.attribute("monster"))) {
				MonsterType* mType = g_monsters().getMonsterType(attr.as_string());
				if (mType) {
					ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
					condition->setOutfit(mType->info.outfit);
					combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
					combat->addCondition(condition);
				}
			} else if ((attr = node.attribute("item"))) {
				Outfit_t outfit;
				outfit.lookTypeEx = pugi::cast<uint16_t>(attr.value());

				ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
				condition->setOutfit(outfit);
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
				combat->addCondition(condition);
			}
		} else if (!tfs_strcmp(tmpName.c_str(), "invisible")) {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_INVISIBLE, duration, 0);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "drunk")) {
			int32_t duration = 10000;

			if ((attr = node.attribute("duration"))) {
				duration = pugi::cast<int32_t>(attr.value());
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_DRUNK, duration, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "firefield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_FIREFIELD_PVP_FULL);
		} else if (!tfs_strcmp(tmpName.c_str(), "poisonfield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_POISONFIELD_PVP);
		} else if (!tfs_strcmp(tmpName.c_str(), "energyfield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_ENERGYFIELD_PVP);
		} else if (!tfs_strcmp(tmpName.c_str(), "firecondition") || !tfs_strcmp(tmpName.c_str(), "energycondition") ||
			!tfs_strcmp(tmpName.c_str(), "earthcondition") || !tfs_strcmp(tmpName.c_str(), "poisoncondition") ||
			!tfs_strcmp(tmpName.c_str(), "icecondition") || !tfs_strcmp(tmpName.c_str(), "freezecondition") ||
			!tfs_strcmp(tmpName.c_str(), "deathcondition") || !tfs_strcmp(tmpName.c_str(), "cursecondition") ||
			!tfs_strcmp(tmpName.c_str(), "holycondition") || !tfs_strcmp(tmpName.c_str(), "dazzlecondition") ||
			!tfs_strcmp(tmpName.c_str(), "drowncondition") || !tfs_strcmp(tmpName.c_str(), "bleedcondition") ||
			!tfs_strcmp(tmpName.c_str(), "physicalcondition")) {
			ConditionType_t conditionType = CONDITION_NONE;
			uint32_t tickInterval = 2000;
			if (!tfs_strcmp(tmpName.c_str(), "firecondition")) {
				conditionType = CONDITION_FIRE;
				tickInterval = 10000;
			} else if (!tfs_strcmp(tmpName.c_str(), "poisoncondition") || !tfs_strcmp(tmpName.c_str(), "earthcondition")) {
				conditionType = CONDITION_POISON;
				tickInterval = 4000;
			} else if (!tfs_strcmp(tmpName.c_str(), "energycondition")) {
				conditionType = CONDITION_ENERGY;
				tickInterval = 10000;
			} else if (!tfs_strcmp(tmpName.c_str(), "drowncondition")) {
				conditionType = CONDITION_DROWN;
				tickInterval = 5000;
			} else if (!tfs_strcmp(tmpName.c_str(), "freezecondition") || !tfs_strcmp(tmpName.c_str(), "icecondition")) {
				conditionType = CONDITION_FREEZING;
				tickInterval = 10000;
			} else if (!tfs_strcmp(tmpName.c_str(), "cursecondition") || !tfs_strcmp(tmpName.c_str(), "deathcondition")) {
				conditionType = CONDITION_CURSED;
				tickInterval = 4000;
			} else if (!tfs_strcmp(tmpName.c_str(), "dazzlecondition") || !tfs_strcmp(tmpName.c_str(), "holycondition")) {
				conditionType = CONDITION_DAZZLED;
				tickInterval = 10000;
			} else if (!tfs_strcmp(tmpName.c_str(), "physicalcondition") || !tfs_strcmp(tmpName.c_str(), "bleedcondition")) {
				conditionType = CONDITION_BLEEDING;
				tickInterval = 4000;
			}

			if ((attr = node.attribute("tick"))) {
				int32_t value = pugi::cast<int32_t>(attr.value());
				if (value > 0) {
					tickInterval = value;
				}
			}

			int32_t minDamage = std::abs(sb.minCombatValue);
			int32_t maxDamage = std::abs(sb.maxCombatValue);
			int32_t startDamage = 0;

			if ((attr = node.attribute("start"))) {
				int32_t value = std::abs(pugi::cast<int32_t>(attr.value()));
				if (value <= minDamage) {
					startDamage = value;
				}
			}

			Condition* condition = getDamageCondition(conditionType, maxDamage, minDamage, startDamage, tickInterval);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "strength")) {
			//
		} else if (!tfs_strcmp(tmpName.c_str(), "effect")) {
			//
		} else {
			std::cout << "[Error - Monsters::deserializeSpell] - " << description << " - Unknown spell name: " << name << std::endl;
			delete combat;
			return false;
		}

		combat->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
		combatSpell = new CombatSpell(combat, needTarget, needDirection);

		for (auto attributeNode : node.children()) {
			if ((attr = attributeNode.attribute("key"))) {
				const char* value = attr.value();
				if (strcasecmp(value, "shooteffect") == 0) {
					if ((attr = attributeNode.attribute("value"))) {
						ShootType_t shoot = getShootType(asLowerCaseString(attr.as_string()));
						if (shoot != CONST_ANI_NONE) {
							combat->setParam(COMBAT_PARAM_DISTANCEEFFECT, shoot);
						} else {
							std::cout << "[Warning - Monsters::deserializeSpell] " << description << " - Unknown shootEffect: " << attr.as_string() << std::endl;
						}
					}
				} else if (strcasecmp(value, "areaeffect") == 0) {
					if ((attr = attributeNode.attribute("value"))) {
						MagicEffectClasses effect = getMagicEffect(asLowerCaseString(attr.as_string()));
						if (effect != CONST_ME_NONE) {
							combat->setParam(COMBAT_PARAM_EFFECT, effect);
						} else {
							std::cout << "[Warning - Monsters::deserializeSpell] " << description << " - Unknown areaEffect: " << attr.as_string() << std::endl;
						}
					}
				} else {
					std::cout << "[Warning - Monsters::deserializeSpells] Effect type \"" << attr.as_string() << "\" does not exist." << std::endl;
				}
			}
		}
	}

	sb.spell = combatSpell;
	if (combatSpell) {
		sb.combatSpell = true;
	}
	return true;
}

bool Monsters::deserializeSpell(MonsterSpell* spell, spellBlock_t& sb, const std::string& description)
{
	if (!spell->scriptName.empty()) {
		spell->isScripted = true;
	} else if (!spell->name.empty()) {
		spell->isScripted = false;
	} else {
		return false;
	}

	sb.speed = spell->interval;

	if (spell->chance > 100) {
		sb.chance = 100;
	} else {
		sb.chance = spell->chance;
	}

	if (spell->range > (Map::maxViewportX * 2)) {
		spell->range = Map::maxViewportX * 2;
	}
	sb.range = spell->range;

	sb.minCombatValue = spell->minCombatValue;
	sb.maxCombatValue = spell->maxCombatValue;
	if (std::abs(sb.minCombatValue) > std::abs(sb.maxCombatValue)) {
		int32_t value = sb.maxCombatValue;
		sb.maxCombatValue = sb.minCombatValue;
		sb.minCombatValue = value;
	}

	sb.spell = g_spells().getSpellByName(spell->name);
	if (sb.spell) {
		return true;
	}

	CombatSpell* combatSpell = nullptr;

	if (spell->isScripted) {
		std::unique_ptr<CombatSpell> combatSpellPtr(new CombatSpell(nullptr, spell->needTarget, spell->needDirection));
		if (!combatSpellPtr->loadScript("data/" + g_spells().getScriptBaseName() + "/scripts/" + spell->scriptName)) {
			std::cout << "cannot find file" << std::endl;
			return false;
		}

		if (!combatSpellPtr->loadScriptCombat()) {
			return false;
		}

		combatSpell = combatSpellPtr.release();
		combatSpell->getCombat()->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
	} else {
		std::unique_ptr<Combat> combat{ new Combat };
		sb.combatSpell = true;

		if (spell->length > 0) {
			spell->spread = std::max<int32_t>(0, spell->spread);

			AreaCombat* area = new AreaCombat();
			area->setupArea(spell->length, spell->spread);
			combat->setArea(area);

			spell->needDirection = true;
		}

		if (spell->radius > 0) {
			AreaCombat* area = new AreaCombat();
			area->setupArea(spell->radius);
			combat->setArea(area);
		}

		std::string tmpName = asLowerCaseString(spell->name);
		if (!tfs_strcmp(tmpName.c_str(), "melee")) {
			sb.isMelee = true;

			if (spell->attack > 0 && spell->skill > 0) {
				sb.minCombatValue = 0;
				sb.maxCombatValue = -Weapons::getMaxMeleeDamage(spell->skill, spell->attack);
			}

			ConditionType_t conditionType = CONDITION_NONE;
			int32_t minDamage = 0;
			int32_t maxDamage = 0;
			uint32_t tickInterval = 2000;

			if (spell->conditionType != CONDITION_NONE) {
				conditionType = spell->conditionType;

				minDamage = spell->conditionMinDamage;
				maxDamage = minDamage;
				if (spell->tickInterval != 0) {
					tickInterval = spell->tickInterval;
				}

				Condition* condition = getDamageCondition(conditionType, maxDamage, minDamage, spell->conditionStartDamage, tickInterval);
				combat->addCondition(condition);
			}

			sb.range = 1;
			combat->setParam(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE);
			combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
			combat->setParam(COMBAT_PARAM_BLOCKSHIELD, 1);
			combat->setOrigin(ORIGIN_MELEE);
		} else if (!tfs_strcmp(tmpName.c_str(), "combat")) {
			if (spell->combatType == COMBAT_PHYSICALDAMAGE) {
				combat->setParam(COMBAT_PARAM_BLOCKARMOR, 1);
				combat->setOrigin(ORIGIN_RANGED);
			} else if (spell->combatType == COMBAT_HEALING) {
				combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			}
			combat->setParam(COMBAT_PARAM_TYPE, spell->combatType);
		} else if (!tfs_strcmp(tmpName.c_str(), "speed")) {
			int32_t minSpeedChange = spell->minSpeedChange;
			int32_t maxSpeedChange = spell->maxSpeedChange;
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			if (maxSpeedChange == 0) {
				maxSpeedChange = minSpeedChange; // static speedchange value
			}

			//cant be slower than 100%
			if (minSpeedChange < -1000) {
				minSpeedChange = -1000;
			}
			if (maxSpeedChange < -1000) {
				maxSpeedChange = -1000;
			}

			ConditionType_t conditionType;
			if ((minSpeedChange < 0) == (maxSpeedChange < 0)) {
				if (minSpeedChange > 0) {
					conditionType = CONDITION_HASTE;
					combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
				} else {
					conditionType = CONDITION_PARALYZE;
				}
			} else {
				std::cout << "[Error - Monsters::deserializeSpell] - " << description << " - can't determine condition type because minSpeedChange/maxSpeedChange sign mismatch" << std::endl;
				delete spell;
				return false;
			}

			ConditionSpeed* condition = static_cast<ConditionSpeed*>(Condition::createCondition(CONDITIONID_COMBAT, conditionType, duration, 0));
			condition->setFormulaVars(minSpeedChange / 1000.0, 0, maxSpeedChange / 1000.0, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "outfit")) {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			ConditionOutfit* condition = static_cast<ConditionOutfit*>(Condition::createCondition(CONDITIONID_COMBAT, CONDITION_OUTFIT, duration, 0));
			condition->setOutfit(spell->outfit);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "invisible")) {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_INVISIBLE, duration, 0);
			combat->setParam(COMBAT_PARAM_AGGRESSIVE, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "drunk")) {
			int32_t duration = 10000;

			if (spell->duration != 0) {
				duration = spell->duration;
			}

			Condition* condition = Condition::createCondition(CONDITIONID_COMBAT, CONDITION_DRUNK, duration, 0);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "firefield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_FIREFIELD_PVP_FULL);
		} else if (!tfs_strcmp(tmpName.c_str(), "poisonfield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_POISONFIELD_PVP);
		} else if (!tfs_strcmp(tmpName.c_str(), "energyfield")) {
			combat->setParam(COMBAT_PARAM_CREATEITEM, ITEM_ENERGYFIELD_PVP);
		} else if (!tfs_strcmp(tmpName.c_str(), "condition")) {
			uint32_t tickInterval = 2000;

			if (spell->conditionType == CONDITION_NONE) {
				std::cout << "[Error - Monsters::deserializeSpell] - " << description << " - Condition is not set for: " << spell->name << std::endl;
			}

			if (spell->tickInterval != 0) {
				int32_t value = spell->tickInterval;
				if (value > 0) {
					tickInterval = value;
				}
			}

			int32_t minDamage = std::abs(spell->conditionMinDamage);
			int32_t maxDamage = std::abs(spell->conditionMaxDamage);
			int32_t startDamage = 0;

			if (spell->conditionStartDamage != 0) {
				int32_t value = std::abs(spell->conditionStartDamage);
				if (value <= minDamage) {
					startDamage = value;
				}
			}

			Condition* condition = getDamageCondition(spell->conditionType, maxDamage, minDamage, startDamage, tickInterval);
			combat->addCondition(condition);
		} else if (!tfs_strcmp(tmpName.c_str(), "strength")) {
			//
		} else if (!tfs_strcmp(tmpName.c_str(), "effect")) {
			//
		} else {
			std::cout << "[Error - Monsters::deserializeSpell] - " << description << " - Unknown spell name: " << spell->name << std::endl;
		}

		if (spell->needTarget) {
			if (spell->shoot != CONST_ANI_NONE) {
				combat->setParam(COMBAT_PARAM_DISTANCEEFFECT, spell->shoot);
			}
		}

		if (spell->effect != CONST_ME_NONE) {
			combat->setParam(COMBAT_PARAM_EFFECT, spell->effect);
		}

		combat->setPlayerCombatValues(COMBAT_FORMULA_DAMAGE, sb.minCombatValue, 0, sb.maxCombatValue, 0);
		combatSpell = new CombatSpell(combat.release(), spell->needTarget, spell->needDirection);
	}

	sb.spell = combatSpell;
	if (combatSpell) {
		sb.combatSpell = true;
	}
	return true;
}

MonsterType* Monsters::loadMonster(const std::string& file, const std::string& monsterName, bool reloading /*= false*/)
{
	MonsterType* mType = nullptr;

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(file.c_str());
	if (!result) {
		printXMLError("Error - Monsters::loadMonster", file, result);
		return nullptr;
	}

	pugi::xml_node monsterNode = doc.child("monster");
	if (!monsterNode) {
		std::cout << "[Error - Monsters::loadMonster] Missing monster node in: " << file << std::endl;
		return nullptr;
	}

	pugi::xml_attribute attr;
	if (!(attr = monsterNode.attribute("name"))) {
		std::cout << "[Error - Monsters::loadMonster] Missing name in: " << file << std::endl;
		return nullptr;
	}

	if (reloading) {
		auto it = monsters.find(monsterName);
		if (it != monsters.end()) {
			mType = &it->second;
			mType->info = {};
		}
	}

	if (!mType) {
		mType = &monsters[monsterName];
	}

	mType->name = attr.as_string();

	if ((attr = monsterNode.attribute("nameDescription"))) {
		mType->nameDescription = attr.as_string();
	} else {
		mType->nameDescription = "a " + asLowerCaseString(mType->name);
	}

	if ((attr = monsterNode.attribute("race"))) {
		std::string tmpStrValue = asLowerCaseString(attr.as_string());
		uint16_t tmpInt = pugi::cast<uint16_t>(attr.value());
		if (!tfs_strcmp(tmpStrValue.c_str(), "venom") || tmpInt == 1) {
			mType->info.race = RACE_VENOM;
		} else if (!tfs_strcmp(tmpStrValue.c_str(), "blood") || tmpInt == 2) {
			mType->info.race = RACE_BLOOD;
		} else if (!tfs_strcmp(tmpStrValue.c_str(), "undead") || tmpInt == 3) {
			mType->info.race = RACE_UNDEAD;
		} else if (!tfs_strcmp(tmpStrValue.c_str(), "fire") || tmpInt == 4) {
			mType->info.race = RACE_FIRE;
		} else if (!tfs_strcmp(tmpStrValue.c_str(), "energy") || tmpInt == 5) {
			mType->info.race = RACE_ENERGY;
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Unknown race type " << attr.as_string() << ". " << file << std::endl;
		}
	}

	if ((attr = monsterNode.attribute("experience"))) {
		mType->info.experience = pugi::cast<uint64_t>(attr.value());
	}

	if ((attr = monsterNode.attribute("speed"))) {
		mType->info.baseSpeed = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = monsterNode.attribute("manacost"))) {
		mType->info.manaCost = pugi::cast<uint32_t>(attr.value());
	}

	if ((attr = monsterNode.attribute("skull"))) {
		mType->info.skull = getSkullType(asLowerCaseString(attr.as_string()));
	}

	if ((attr = monsterNode.attribute("script"))) {
		if (!scriptInterface) {
			scriptInterface.reset(new LuaScriptInterface("Monster Interface"));
			scriptInterface->initState();
		}

		std::string script = attr.as_string();
		if (scriptInterface->loadFile("data/monster/scripts/" + script) == 0) {
			mType->info.scriptInterface = scriptInterface.get();
			mType->info.creatureAppearEvent = scriptInterface->getEvent("onCreatureAppear");
			mType->info.creatureDisappearEvent = scriptInterface->getEvent("onCreatureDisappear");
			mType->info.creatureMoveEvent = scriptInterface->getEvent("onCreatureMove");
			mType->info.creatureSayEvent = scriptInterface->getEvent("onCreatureSay");
			mType->info.thinkEvent = scriptInterface->getEvent("onThink");
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Can not load script: " << script << std::endl;
			std::cout << scriptInterface->getLastLuaError() << std::endl;
		}
	}

	pugi::xml_node node;
	if ((node = monsterNode.child("health"))) {
		if ((attr = node.attribute("now"))) {
			mType->info.health = pugi::cast<int32_t>(attr.value());
		} else {
			std::cout << "[Error - Monsters::loadMonster] Missing health now. " << file << std::endl;
		}

		if ((attr = node.attribute("max"))) {
			mType->info.healthMax = pugi::cast<int32_t>(attr.value());
		} else {
			std::cout << "[Error - Monsters::loadMonster] Missing health max. " << file << std::endl;
		}
	}

	if ((node = monsterNode.child("flags"))) {
		for (auto flagNode : node.children()) {
			attr = flagNode.first_attribute();
			const char* attrName = attr.name();
			if (strcasecmp(attrName, "summonable") == 0) {
				mType->info.isSummonable = attr.as_bool();
			} else if (strcasecmp(attrName, "attackable") == 0) {
				mType->info.isAttackable = attr.as_bool();
			} else if (strcasecmp(attrName, "hostile") == 0) {
				mType->info.isHostile = attr.as_bool();
			} else if (strcasecmp(attrName, "illusionable") == 0) {
				mType->info.isIllusionable = attr.as_bool();
			} else if (strcasecmp(attrName, "convinceable") == 0) {
				mType->info.isConvinceable = attr.as_bool();
			} else if (strcasecmp(attrName, "pushable") == 0) {
				mType->info.pushable = attr.as_bool();
			} else if (strcasecmp(attrName, "canpushitems") == 0) {
				mType->info.canPushItems = attr.as_bool();
			} else if (strcasecmp(attrName, "canpushcreatures") == 0) {
				mType->info.canPushCreatures = attr.as_bool();
			} else if (strcasecmp(attrName, "staticattack") == 0) {
				uint32_t staticAttack = pugi::cast<uint32_t>(attr.value());
				if (staticAttack > 100) {
					std::cout << "[Warning - Monsters::loadMonster] staticattack greater than 100. " << file << std::endl;
					staticAttack = 100;
				}

				mType->info.staticAttackChance = staticAttack;
			} else if (strcasecmp(attrName, "lightlevel") == 0) {
				mType->info.light.level = pugi::cast<uint16_t>(attr.value());
			} else if (strcasecmp(attrName, "lightcolor") == 0) {
				mType->info.light.color = pugi::cast<uint16_t>(attr.value());
			} else if (strcasecmp(attrName, "targetdistance") == 0) {
				mType->info.targetDistance = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
			} else if (strcasecmp(attrName, "runonhealth") == 0) {
				mType->info.runAwayHealth = pugi::cast<int32_t>(attr.value());
			} else if (strcasecmp(attrName, "hidehealth") == 0) {
				mType->info.hiddenHealth = attr.as_bool();
			} else if (strcasecmp(attrName, "isblockable") == 0) {
				mType->info.isBlockable = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonenergy") == 0) {
				mType->info.canWalkOnEnergy = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonfire") == 0) {
				mType->info.canWalkOnFire = attr.as_bool();
			} else if (strcasecmp(attrName, "canwalkonpoison") == 0) {
				mType->info.canWalkOnPoison = attr.as_bool();
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown flag attribute: " << attrName << ". " << file << std::endl;
			}
		}

		//if a monster can push creatures,
		// it should not be pushable
		if (mType->info.canPushCreatures) {
			mType->info.pushable = false;
		}
	}

	if ((node = monsterNode.child("targetchange"))) {
		if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
			mType->info.changeTargetSpeed = pugi::cast<uint32_t>(attr.value());
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing targetchange speed. " << file << std::endl;
		}

		if ((attr = node.attribute("chance"))) {
			mType->info.changeTargetChance = pugi::cast<int32_t>(attr.value());
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing targetchange chance. " << file << std::endl;
		}
	}

	if ((node = monsterNode.child("look"))) {
		if ((attr = node.attribute("type"))) {
			mType->info.outfit.lookType = pugi::cast<uint16_t>(attr.value());

			if ((attr = node.attribute("head"))) {
				mType->info.outfit.lookHead = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("body"))) {
				mType->info.outfit.lookBody = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("legs"))) {
				mType->info.outfit.lookLegs = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("feet"))) {
				mType->info.outfit.lookFeet = pugi::cast<uint16_t>(attr.value());
			}

			if ((attr = node.attribute("addons"))) {
				mType->info.outfit.lookAddons = pugi::cast<uint16_t>(attr.value());
			}
		} else if ((attr = node.attribute("typeex"))) {
			mType->info.outfit.lookTypeEx = pugi::cast<uint16_t>(attr.value());
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing look type/typeex. " << file << std::endl;
		}

		#if GAME_FEATURE_MOUNTS > 0
		if ((attr = node.attribute("mount"))) {
			mType->info.outfit.lookMount = pugi::cast<uint16_t>(attr.value());
		}
		#endif

		if ((attr = node.attribute("corpse"))) {
			mType->info.lookcorpse = pugi::cast<uint16_t>(attr.value());
		}
	}

	if ((node = monsterNode.child("attacks"))) {
		for (auto attackNode : node.children()) {
			spellBlock_t sb;
			if (deserializeSpell(attackNode, sb, monsterName)) {
				mType->info.attackSpells.emplace_back(std::move(sb));
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("defenses"))) {
		if ((attr = node.attribute("defense"))) {
			mType->info.defense = pugi::cast<int32_t>(attr.value());
		}

		if ((attr = node.attribute("armor"))) {
			mType->info.armor = pugi::cast<int32_t>(attr.value());
		}

		for (auto defenseNode : node.children()) {
			spellBlock_t sb;
			if (deserializeSpell(defenseNode, sb, monsterName)) {
				mType->info.defenseSpells.emplace_back(std::move(sb));
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("immunities"))) {
		for (auto immunityNode : node.children()) {
			if ((attr = immunityNode.attribute("name"))) {
				std::string tmpStrValue = asLowerCaseString(attr.as_string());
				if (!tfs_strcmp(tmpStrValue.c_str(), "physical")) {
					mType->info.damageImmunities |= COMBAT_PHYSICALDAMAGE;
					mType->info.conditionImmunities |= CONDITION_BLEEDING;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "energy")) {
					mType->info.damageImmunities |= COMBAT_ENERGYDAMAGE;
					mType->info.conditionImmunities |= CONDITION_ENERGY;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "fire")) {
					mType->info.damageImmunities |= COMBAT_FIREDAMAGE;
					mType->info.conditionImmunities |= CONDITION_FIRE;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "poison") || !tfs_strcmp(tmpStrValue.c_str(), "earth")) {
					mType->info.damageImmunities |= COMBAT_EARTHDAMAGE;
					mType->info.conditionImmunities |= CONDITION_POISON;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "drown")) {
					mType->info.damageImmunities |= COMBAT_DROWNDAMAGE;
					mType->info.conditionImmunities |= CONDITION_DROWN;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "ice")) {
					mType->info.damageImmunities |= COMBAT_ICEDAMAGE;
					mType->info.conditionImmunities |= CONDITION_FREEZING;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "holy")) {
					mType->info.damageImmunities |= COMBAT_HOLYDAMAGE;
					mType->info.conditionImmunities |= CONDITION_DAZZLED;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "death")) {
					mType->info.damageImmunities |= COMBAT_DEATHDAMAGE;
					mType->info.conditionImmunities |= CONDITION_CURSED;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "lifedrain")) {
					mType->info.damageImmunities |= COMBAT_LIFEDRAIN;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "manadrain")) {
					mType->info.damageImmunities |= COMBAT_MANADRAIN;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "paralyze")) {
					mType->info.conditionImmunities |= CONDITION_PARALYZE;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "outfit")) {
					mType->info.conditionImmunities |= CONDITION_OUTFIT;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "drunk")) {
					mType->info.conditionImmunities |= CONDITION_DRUNK;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "invisible") || !tfs_strcmp(tmpStrValue.c_str(), "invisibility")) {
					mType->info.conditionImmunities |= CONDITION_INVISIBLE;
				} else if (!tfs_strcmp(tmpStrValue.c_str(), "bleed")) {
					mType->info.conditionImmunities |= CONDITION_BLEEDING;
				} else {
					std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << attr.as_string() << ". " << file << std::endl;
				}
			} else if ((attr = immunityNode.attribute("physical"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_PHYSICALDAMAGE;
					mType->info.conditionImmunities |= CONDITION_BLEEDING;
				}
			} else if ((attr = immunityNode.attribute("energy"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_ENERGYDAMAGE;
					mType->info.conditionImmunities |= CONDITION_ENERGY;
				}
			} else if ((attr = immunityNode.attribute("fire"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_FIREDAMAGE;
					mType->info.conditionImmunities |= CONDITION_FIRE;
				}
			} else if ((attr = immunityNode.attribute("poison")) || (attr = immunityNode.attribute("earth"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_EARTHDAMAGE;
					mType->info.conditionImmunities |= CONDITION_POISON;
				}
			} else if ((attr = immunityNode.attribute("drown"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_DROWNDAMAGE;
					mType->info.conditionImmunities |= CONDITION_DROWN;
				}
			} else if ((attr = immunityNode.attribute("ice"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_ICEDAMAGE;
					mType->info.conditionImmunities |= CONDITION_FREEZING;
				}
			} else if ((attr = immunityNode.attribute("holy"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_HOLYDAMAGE;
					mType->info.conditionImmunities |= CONDITION_DAZZLED;
				}
			} else if ((attr = immunityNode.attribute("death"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_DEATHDAMAGE;
					mType->info.conditionImmunities |= CONDITION_CURSED;
				}
			} else if ((attr = immunityNode.attribute("lifedrain"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_LIFEDRAIN;
				}
			} else if ((attr = immunityNode.attribute("manadrain"))) {
				if (attr.as_bool()) {
					mType->info.damageImmunities |= COMBAT_MANADRAIN;
				}
			} else if ((attr = immunityNode.attribute("paralyze"))) {
				if (attr.as_bool()) {
					mType->info.conditionImmunities |= CONDITION_PARALYZE;
				}
			} else if ((attr = immunityNode.attribute("outfit"))) {
				if (attr.as_bool()) {
					mType->info.conditionImmunities |= CONDITION_OUTFIT;
				}
			} else if ((attr = immunityNode.attribute("bleed"))) {
				if (attr.as_bool()) {
					mType->info.conditionImmunities |= CONDITION_BLEEDING;
				}
			} else if ((attr = immunityNode.attribute("drunk"))) {
				if (attr.as_bool()) {
					mType->info.conditionImmunities |= CONDITION_DRUNK;
				}
			} else if ((attr = immunityNode.attribute("invisible")) || (attr = immunityNode.attribute("invisibility"))) {
				if (attr.as_bool()) {
					mType->info.conditionImmunities |= CONDITION_INVISIBLE;
				}
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("voices"))) {
		if ((attr = node.attribute("speed")) || (attr = node.attribute("interval"))) {
			mType->info.yellSpeedTicks = pugi::cast<uint32_t>(attr.value());
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing voices speed. " << file << std::endl;
		}

		if ((attr = node.attribute("chance"))) {
			mType->info.yellChance = pugi::cast<uint32_t>(attr.value());
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing voices chance. " << file << std::endl;
		}

		for (auto voiceNode : node.children()) {
			voiceBlock_t vb;
			if ((attr = voiceNode.attribute("sentence"))) {
				vb.text = attr.as_string();
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Missing voice sentence. " << file << std::endl;
			}

			if ((attr = voiceNode.attribute("yell"))) {
				vb.yellText = attr.as_bool();
			} else {
				vb.yellText = false;
			}
			mType->info.voiceVector.emplace_back(vb);
		}
	}

	if ((node = monsterNode.child("loot"))) {
		for (auto lootNode : node.children()) {
			LootBlock lootBlock;
			if (loadLootItem(lootNode, lootBlock)) {
				mType->info.lootItems.emplace_back(std::move(lootBlock));
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Cant load loot. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("elements"))) {
		for (auto elementNode : node.children()) {
			if ((attr = elementNode.attribute("physicalPercent"))) {
				mType->info.elementMap[COMBAT_PHYSICALDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("icePercent"))) {
				mType->info.elementMap[COMBAT_ICEDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("poisonPercent")) || (attr = elementNode.attribute("earthPercent"))) {
				mType->info.elementMap[COMBAT_EARTHDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("firePercent"))) {
				mType->info.elementMap[COMBAT_FIREDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("energyPercent"))) {
				mType->info.elementMap[COMBAT_ENERGYDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("holyPercent"))) {
				mType->info.elementMap[COMBAT_HOLYDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("deathPercent"))) {
				mType->info.elementMap[COMBAT_DEATHDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("drownPercent"))) {
				mType->info.elementMap[COMBAT_DROWNDAMAGE] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("lifedrainPercent"))) {
				mType->info.elementMap[COMBAT_LIFEDRAIN] = pugi::cast<int32_t>(attr.value());
			} else if ((attr = elementNode.attribute("manadrainPercent"))) {
				mType->info.elementMap[COMBAT_MANADRAIN] = pugi::cast<int32_t>(attr.value());
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown element percent. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("summons"))) {
		if ((attr = node.attribute("maxSummons"))) {
			mType->info.maxSummons = std::min<uint32_t>(pugi::cast<uint32_t>(attr.value()), 100);
		} else {
			std::cout << "[Warning - Monsters::loadMonster] Missing summons maxSummons. " << file << std::endl;
		}

		for (auto summonNode : node.children()) {
			int32_t chance = 100;
			int32_t speed = 1000;
			int32_t max = mType->info.maxSummons;
			bool force = false;

			if ((attr = summonNode.attribute("speed")) || (attr = summonNode.attribute("interval"))) {
				speed = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
			}

			if ((attr = summonNode.attribute("chance"))) {
				chance = pugi::cast<int32_t>(attr.value());
			}

			if ((attr = summonNode.attribute("max"))) {
				max = pugi::cast<uint32_t>(attr.value());
			}

			if ((attr = summonNode.attribute("force"))) {
				force = attr.as_bool();
			}

			if ((attr = summonNode.attribute("name"))) {
				summonBlock_t sb;
				sb.name = attr.as_string();
				sb.speed = speed;
				sb.chance = chance;
				sb.max = max;
				sb.force = force;
				mType->info.summons.emplace_back(sb);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Missing summon name. " << file << std::endl;
			}
		}
	}

	if ((node = monsterNode.child("script"))) {
		for (auto eventNode : node.children()) {
			if ((attr = eventNode.attribute("name"))) {
				mType->info.scripts.emplace_back(attr.as_string());
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Missing name for script event. " << file << std::endl;
			}
		}
	}

	mType->info.summons.shrink_to_fit();
	mType->info.lootItems.shrink_to_fit();
	mType->info.attackSpells.shrink_to_fit();
	mType->info.defenseSpells.shrink_to_fit();
	mType->info.voiceVector.shrink_to_fit();
	mType->info.scripts.shrink_to_fit();
	return mType;
}

bool Monsters::loadLootItem(const pugi::xml_node& node, LootBlock& lootBlock)
{
	pugi::xml_attribute attr;
	if ((attr = node.attribute("id"))) {
		lootBlock.id = pugi::cast<int32_t>(attr.value());
	} else if ((attr = node.attribute("name"))) {
		auto name = attr.as_string();

		uint16_t id = Item::items.getItemIdByName(name);
		if (id < 100) {
			std::cout << "[Warning - Monsters::loadMonster] Unknown loot item \"" << name << "\". " << std::endl;
			return false;
		}

		lootBlock.id = id;
	}

	if (lootBlock.id == 0) {
		return false;
	}

	if ((attr = node.attribute("countmax"))) {
		lootBlock.countmax = std::max<int32_t>(1, pugi::cast<int32_t>(attr.value()));
	} else {
		lootBlock.countmax = 1;
	}

	if ((attr = node.attribute("chance")) || (attr = node.attribute("chance1"))) {
		lootBlock.chance = std::min<int32_t>(MAX_LOOTCHANCE, pugi::cast<int32_t>(attr.value()));
	} else {
		lootBlock.chance = MAX_LOOTCHANCE;
	}

	if (Item::items[lootBlock.id].isContainer()) {
		loadLootContainer(node, lootBlock);
	}

	//optional
	if ((attr = node.attribute("subtype"))) {
		lootBlock.subType = pugi::cast<int32_t>(attr.value());
	} else {
		uint32_t charges = Item::items[lootBlock.id].charges;
		if (charges != 0) {
			lootBlock.subType = charges;
		}
	}

	if ((attr = node.attribute("actionId"))) {
		lootBlock.actionId = pugi::cast<int32_t>(attr.value());
	}

	if ((attr = node.attribute("text"))) {
		lootBlock.text = attr.as_string();
	}
	return true;
}

void Monsters::loadLootContainer(const pugi::xml_node& node, LootBlock& lBlock)
{
	for (auto subNode : node.children()) {
		LootBlock lootBlock;
		if (loadLootItem(subNode, lootBlock)) {
			lBlock.childLoot.emplace_back(std::move(lootBlock));
		}
	}
}

std::string Monsters::getRaceName(uint16_t raceId)
{
	for (const auto& race : races) {
		if (race.second == raceId) {
			return race.first;
		}
	}
	return std::string();
}

MonsterType* Monsters::getMonsterType(const std::string& name)
{
	std::string lowerCaseName = asLowerCaseString(name);

	auto it = monsters.find(lowerCaseName);
	if (it == monsters.end()) {
		return nullptr;
	}
	return &it->second;
}

MonsterType* Monsters::addMonsterType(const std::string& name)
{
	return &monsters[asLowerCaseString(name)];
}

bool Monsters::loadCallback(LuaScriptInterface* scriptInterface, MonsterType* mType)
{
	int32_t id = scriptInterface->getEvent();
	if (id == -1) {
		std::cout << "[Warning - MonsterType::loadCallback] Event not found. " << std::endl;
		return false;
	}

	mType->info.scriptInterface = scriptInterface;
	if (mType->info.eventType == MONSTERS_EVENT_THINK) {
		mType->info.thinkEvent = id;
	} else if (mType->info.eventType == MONSTERS_EVENT_APPEAR) {
		mType->info.creatureAppearEvent = id;
	} else if (mType->info.eventType == MONSTERS_EVENT_DISAPPEAR) {
		mType->info.creatureDisappearEvent = id;
	} else if (mType->info.eventType == MONSTERS_EVENT_MOVE) {
		mType->info.creatureMoveEvent = id;
	} else if (mType->info.eventType == MONSTERS_EVENT_SAY) {
		mType->info.creatureSayEvent = id;
	}

	return true;
}
