/*
 * EconomyManager.cpp
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#include "module/EconomyManager.h"
#include "module/BuilderManager.h"
#include "module/FactoryManager.h"
#include "setup/SetupManager.h"
#include "resource/MetalManager.h"
#include "terrain/TerrainManager.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "CircuitAI.h"
#include "util/math/LagrangeInterPol.h"
#include "util/Scheduler.h"
#include "util/utils.h"

#include "AISCommands.h"
#include "OOAICallback.h"
#include "Unit.h"
#include "UnitDef.h"
#include "Map.h"
#include "SkirmishAIs.h"
#include "Resource.h"
#include "Economy.h"
#include "Feature.h"
#include "Team.h"  // Only for GetAllyTeams().size()

namespace circuit {

using namespace springai;

#define INCOME_SAMPLES	10

CEconomyManager::CEconomyManager(CCircuitAI* circuit) :
		IModule(circuit),
		pylonCount(0),
		indexRes(0)
{
	metalRes = circuit->GetCallback()->GetResourceByName("Metal");
	energyRes = circuit->GetCallback()->GetResourceByName("Energy");
	eco = circuit->GetCallback()->GetEconomy();

	metalIncomes.resize(INCOME_SAMPLES, .0f);
	energyIncomes.resize(INCOME_SAMPLES, .0f);

	UnitDef* def = circuit->GetUnitDefByName("armestor");
	const std::map<std::string, std::string>& customParams = def->GetCustomParams();
	auto search = customParams.find("pylonrange");
	pylonRange = (search != customParams.end()) ? utils::string_to_float(search->second) : 500;

	// TODO: Use A* ai planning... or sth... STRIPS https://ru.wikipedia.org/wiki/STRIPS
	//       https://ru.wikipedia.org/wiki/Марковский_процесс_принятия_решений

	CScheduler* scheduler = circuit->GetScheduler().get();
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateResourceIncome, this), FRAMES_PER_SEC / 2);
	scheduler->RunParallelTask(CGameTask::emptyTask, std::make_shared<CGameTask>(&CEconomyManager::Init, this));

	// TODO: Group handlers
	//       Raider:       Glaive, Bandit, Scorcher, Pyro, Panther, Scrubber, Duck
	//       Assault:      Zeus, Thug, Ravager, Hermit, Reaper
	//       Skirmisher:   Rocko, Rogue, Recluse, Scalpel, Buoy
	//       Riot:         Warrior, Outlaw, Leveler, Mace, Scallop
	//       Artillery:    Hammer, Wolverine, Impaler, Firewalker, Pillager, Tremor
	//       Scout:        Flea, Dart, Puppy
	//       Anti-Air:     Gremlin, Vandal, Crasher, Archangel, Tarantula, Copperhead, Flail, Angler
	//       Support:      Slasher, Penetrator, Felon, Moderator, (Dominatrix?)
	//       Mobile Bombs: Tick, Roach, Skuttle
	//       Shield
	//       Cloaker

	int unitDefId;

	/*
	 * factorycloak handlers
	 */
	unitDefId = circuit->GetUnitDefByName("factorycloak")->GetUnitDefId();
	finishedHandler[unitDefId] = [this](CCircuitUnit* unit) {
		// check factory's cluster
		int index = this->circuit->GetMetalManager()->FindNearestCluster(unit->GetUnit()->GetPos());
		if (index >= 0) {
			clusterInfos[index].factory = unit;
		}
	};
	destroyedHandler[unitDefId] = [this](CCircuitUnit* unit, CCircuitUnit* attacker) {
		if (unit->GetUnit()->IsBeingBuilt()) {
			return;
		}
		for (auto& info : clusterInfos) {
			if (info.factory == unit) {
				info.factory = nullptr;
			}
		}
	};

	/*
	 * armestor handlers
	 */
	unitDefId = circuit->GetUnitDefByName("armestor")->GetUnitDefId();
	createdHandler[unitDefId] = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		// check pylon's cluster
		int index = this->circuit->GetMetalManager()->FindNearestCluster(unit->GetUnit()->GetPos());
		if (index >= 0) {
			clusterInfos[index].pylon = unit;
		}
		++pylonCount;
	};
	destroyedHandler[unitDefId] = [this](CCircuitUnit* unit, CCircuitUnit* attacker) {
		for (auto& info : clusterInfos) {
			if (info.pylon == unit) {
				info.pylon = nullptr;
			}
		}
		--pylonCount;
	};
	Map* map = circuit->GetMap();
	float pylonSquare = pylonRange * 2 / SQUARE_SIZE;
	pylonSquare *= pylonSquare;
	pylonMaxCount = ((map->GetWidth() * map->GetHeight()) / pylonSquare) / 2;

	/*
	 *  Identify resource buildings
	 */
	CCircuitAI::UnitDefs& allDefs = circuit->GetUnitDefs();
	for (auto& kv : allDefs) {
		UnitDef* def = kv.second;
		if (def->GetSpeed() <= 0) {
			const std::map<std::string, std::string>& customParams = def->GetCustomParams();
			auto it = customParams.find("income_energy");
			if ((it != customParams.end()) && (utils::string_to_float(it->second) > 1)) {
				// TODO: Filter only defs that we are able to build
				allEnergyDefs.insert(def);
			} else if (((it = customParams.find("ismex")) != customParams.end()) && (utils::string_to_int(it->second) == 1)) {
				mexDef = def;  // cormex
			}
		}
	}

	// FIXME: Cost thresholds/ecoFactor should rely on alive allies
	std::vector<Team*> allyTeams = circuit->GetCallback()->GetAllyTeams();
	float allyTeamCount = allyTeams.size();
	ecoFactor = 1.0f / (allyTeamCount * 0.25f + 0.75f);
	utils::free_clear(allyTeams);

	// TODO: Make configurable
	// Using cafus, armfus, armsolar as control points
	const char* engies[] = {"cafus", "armfus", "armsolar"};
	const int limits[] = {2, 3, 8};  // TODO: range randomize
	const int size = sizeof(engies) / sizeof(engies[0]);
	CLagrangeInterPol::Vector x(size), y(size);
	for (int i = 0; i < size; ++i) {
		UnitDef* def = circuit->GetUnitDefByName(engies[i]);
		float make = utils::string_to_float(def->GetCustomParams().find("income_energy")->second);
		x[i] = def->GetCost(metalRes) / make;
		y[i] = limits[i] + 0.5;  // +0.5 to be sure precision errors will not decrease integer part
	}
	engyPol = new CLagrangeInterPol(x, y);  // Alternatively use CGaussSolver to compute polynomial - faster on reuse
}

CEconomyManager::~CEconomyManager()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
	delete metalRes, energyRes, eco;
	delete engyPol;
}

int CEconomyManager::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder)
{
	auto search = createdHandler.find(unit->GetDef()->GetUnitDefId());
	if (search != createdHandler.end()) {
		search->second(unit, builder);
	}

	return 0; //signaling: OK
}

int CEconomyManager::UnitFinished(CCircuitUnit* unit)
{
	auto search = finishedHandler.find(unit->GetDef()->GetUnitDefId());
	if (search != finishedHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CEconomyManager::UnitDestroyed(CCircuitUnit* unit, CCircuitUnit* attacker)
{
	auto search = destroyedHandler.find(unit->GetDef()->GetUnitDefId());
	if (search != destroyedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

IBuilderTask* CEconomyManager::CreateBuilderTask(CCircuitUnit* unit)
{
	// TODO: Add general logic here
	Unit* u = unit->GetUnit();
	const AIFloat3& pos = u->GetPos();

	IBuilderTask* task;
	task = UpdateMetalTasks(pos, unit);
	if (task != nullptr) {
		return task;
	}
	task = UpdateEnergyTasks(pos, unit);
	if (task != nullptr) {
		return task;
	}
	task = UpdateFactoryTasks(pos, unit);
	if (task != nullptr) {
		return task;
	}

	CBuilderManager* builderManager = circuit->GetBuilderManager();
	std::vector<Feature*> features = circuit->GetCallback()->GetFeaturesIn(pos, u->GetMaxSpeed() * FRAMES_PER_SEC * 60);
	if (!features.empty() && (builderManager->GetTasks(IBuilderTask::BuildType::RECLAIM).size() < 20)) {
		task = builderManager->EnqueueTask(IBuilderTask::Priority::LOW, pos,
										   IBuilderTask::BuildType::RECLAIM, FRAMES_PER_SEC * 60);
	}
	utils::free_clear(features);
	if (task != nullptr) {
		return task;
	}

	// FIXME: Eco rules. It should never get here
	float metalIncome = GetAvgMetalIncome();
	UnitDef* buildDef = circuit->GetUnitDefByName("armwin");
	if ((metalIncome < 20 * ecoFactor) && (circuit->GetCircuitDef(buildDef)->GetCount() < 50)) {
		task = builderManager->EnqueueTask(IBuilderTask::Priority::LOW, buildDef, pos,
										   IBuilderTask::BuildType::ENERGY);
	} else if (metalIncome < 40 * ecoFactor) {
		task = builderManager->EnqueueTask(IBuilderTask::Priority::LOW, unit->GetUnit()->GetPos(),
										   IBuilderTask::BuildType::PATROL, FRAMES_PER_SEC * 20);
	} else {
		const std::set<IBuilderTask*>& tasks = builderManager->GetTasks(IBuilderTask::BuildType::BIG_GUN);
		if (tasks.empty()) {
			buildDef = circuit->GetUnitDefByName("raveparty");
			if (circuit->GetCircuitDef(buildDef)->GetCount() < 2) {
				task = builderManager->EnqueueTask(IBuilderTask::Priority::HIGH, buildDef, circuit->GetSetupManager()->GetStartPos(),
												   IBuilderTask::BuildType::BIG_GUN);
			} else {
				task = builderManager->EnqueueTask(IBuilderTask::Priority::LOW, unit->GetUnit()->GetPos(),
												   IBuilderTask::BuildType::PATROL, FRAMES_PER_SEC * 20);
			}
		} else {
			task = *tasks.begin();
		}
	}
	return task;
}

CRecruitTask* CEconomyManager::CreateFactoryTask(CCircuitUnit* unit)
{
	// TODO: Add general logic here
	CRecruitTask* task = UpdateRecruitTasks();
	if (task != nullptr) {
		return task;
	}

	Unit* u = unit->GetUnit();
	UnitDef* def = unit->GetDef();
	const char* names3[] = {"armrock", "armpw", "armwar", "armsnipe", "armjeth", "armzeus"};
	const char* names2[] = {"armpw", "armrock", "armpw", "armwar", "armsnipe", "armzeus"};
	const char* names1[] = {"armpw", "armrock", "armpw", "armwar", "armpw", "armrock"};
	char** names;
	float metalIncome = GetAvgMetalIncome();
	if (metalIncome > 30 * ecoFactor) {
		names = (char**)names3;
	} else if (metalIncome > 20 * ecoFactor) {
		names = (char**)names2;
	} else {
		names = (char**)names1;
	}
	UnitDef* buildDef = circuit->GetUnitDefByName(names[rand() % 6]);
	const AIFloat3& buildPos = u->GetPos();
	float radius = std::max(def->GetXSize(), def->GetZSize()) * SQUARE_SIZE * 4;
	task = circuit->GetFactoryManager()->EnqueueTask(CRecruitTask::Priority::LOW, buildDef, buildPos, CRecruitTask::FacType::DEFAULT, 1, radius);
	return task;
}

Resource* CEconomyManager::GetMetalRes() const
{
	return metalRes;
}

Resource* CEconomyManager::GetEnergyRes() const
{
	return energyRes;
}

UnitDef* CEconomyManager::GetMexDef() const
{
	return mexDef;
}

AIFloat3 CEconomyManager::FindBuildPos(CCircuitUnit* unit)
{
	IBuilderTask* task = static_cast<IBuilderTask*>(unit->GetTask());
	Unit* u = unit->GetUnit();
	UnitDef* buildDef = task->GetBuildDef();
	AIFloat3 buildPos = -RgtVector;
	CMetalManager* metalManager = circuit->GetMetalManager();
	CTerrainManager* terrainManager = circuit->GetTerrainManager();
	switch (task->GetBuildType()) {
		case IBuilderTask::BuildType::MEX: {
			const AIFloat3& position = u->GetPos();
			const CMetalData::Metals& spots = metalManager->GetSpots();
			Map* map = circuit->GetMap();
			CMetalData::MetalPredicate predicate = [&spots, metalManager, map, buildDef, terrainManager, unit](CMetalData::MetalNode const& v) {
				int index = v.second;
				return (metalManager->IsOpenSpot(index) &&
						terrainManager->CanBuildAt(unit, spots[index].position) &&
						map->IsPossibleToBuildAt(buildDef, spots[index].position, UNIT_COMMAND_BUILD_NO_FACING));
			};
			int index = metalManager->FindNearestSpot(position, predicate);
			if (index >= 0) {
				buildPos = spots[index].position;
			}
			break;
		}
		case IBuilderTask::BuildType::PYLON: {
			CTerrainManager::TerrainPredicate predicate = [terrainManager, unit](const AIFloat3& p) {
				return terrainManager->CanBuildAt(unit, p);
			};
			const AIFloat3& position = task->GetTaskPos();
			CTerrainManager* terrain = circuit->GetTerrainManager();
			buildPos = terrain->FindBuildSite(buildDef, position, pylonRange * 8, UNIT_COMMAND_BUILD_NO_FACING, predicate);
			if (buildPos == -RgtVector) {
//				CMetalData::MetalPredicate predCl = [this](const CMetalData::MetalNode& v) {
//					return clusterInfos[v.second].pylon == nullptr;
//				};
				CMetalData::MetalIndices indices = metalManager->FindNearestClusters(position, 3/*, predCl*/);
				const CMetalData::Clusters& clusters = metalManager->GetClusters();
				for (const int idx : indices) {
					buildPos = terrain->FindBuildSite(buildDef, clusters[idx].geoCentr, pylonRange * 8, UNIT_COMMAND_BUILD_NO_FACING, predicate);
					if (buildPos != -RgtVector) {
						break;
					}
				}
			}
			break;
		}
	}
	return buildPos;
}

void CEconomyManager::AddAvailEnergy(const std::set<UnitDef*>& buildDefs)
{
	// TODO: Cache engyDefs in CCircuitDef?
	std::set<UnitDef*> engyDefs;
	std::set_intersection(allEnergyDefs.begin(), allEnergyDefs.end(),
						  buildDefs.begin(), buildDefs.end(),
						  std::inserter(engyDefs, engyDefs.begin()));
	std::set<UnitDef*> diffDefs;
	std::set_difference(engyDefs.begin(), engyDefs.end(),
						availEnergyDefs.begin(), availEnergyDefs.end(),
						std::inserter(diffDefs, diffDefs.begin()));
	if (diffDefs.empty()) {
		return;
	}
	availEnergyDefs.insert(diffDefs.begin(), diffDefs.end());

	CCircuitAI::UnitDefs& defs = circuit->GetUnitDefs();
	for (auto def : diffDefs) {
		SEnergyInfo engy;
		engy.def = def;
		engy.cost = def->GetCost(metalRes);
		engy.costDivMake = engy.cost / utils::string_to_float(def->GetCustomParams().find("income_energy")->second);
		engy.limit = engyPol->GetValueAt(engy.costDivMake);
		energyInfos.push_back(engy);
	}

	// High-tech energy first
	auto compare = [](const SEnergyInfo& e1, const SEnergyInfo& e2) {
		return e1.costDivMake < e2.costDivMake;
	};
	energyInfos.sort(compare);
}

void CEconomyManager::RemoveAvailEnergy(const std::set<UnitDef*>& buildDefs)
{
	// TODO: Cache engyDefs in CCircuitDef?
	std::set<UnitDef*> engyDefs;
	std::set_intersection(allEnergyDefs.begin(), allEnergyDefs.end(),
						  buildDefs.begin(), buildDefs.end(),
						  std::inserter(engyDefs, engyDefs.begin()));
	std::set<UnitDef*> diffDefs;
	std::set_difference(availEnergyDefs.begin(), availEnergyDefs.end(),
						engyDefs.begin(), engyDefs.end(),
						std::inserter(diffDefs, diffDefs.begin()));
	if (diffDefs.empty()) {
		return;
	}
	availEnergyDefs.erase(diffDefs.begin(), diffDefs.end());

	CCircuitAI::UnitDefs& defs = circuit->GetUnitDefs();
	auto it = energyInfos.begin();
	while (it != energyInfos.end()) {
		auto search = diffDefs.find(it->def);
		if (search != diffDefs.end()) {
			it = energyInfos.erase(it);
		}
	}
}

void CEconomyManager::UpdateResourceIncome()
{
	energyIncomes[indexRes] = eco->GetIncome(energyRes);
	metalIncomes[indexRes] = eco->GetIncome(metalRes);
	indexRes++;
	indexRes %= INCOME_SAMPLES;

	metalIncome = .0f;
	for (int i = 0; i < INCOME_SAMPLES; i++) {
		metalIncome += metalIncomes[i];
	}
	metalIncome /= INCOME_SAMPLES;

	energyIncome = .0f;
	for (int i = 0; i < INCOME_SAMPLES; i++) {
		energyIncome += energyIncomes[i];
	}
	energyIncome /= INCOME_SAMPLES;
}

float CEconomyManager::GetAvgMetalIncome()
{
	return metalIncome;
}

float CEconomyManager::GetAvgEnergyIncome()
{
	return energyIncome;
}

IBuilderTask* CEconomyManager::UpdateMetalTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	IBuilderTask* task = nullptr;
	if (!builderManager->CanEnqueueTask()) {
		return task;
	}

	// check uncolonized mexes
	float energyIncome = GetAvgEnergyIncome();
	float metalIncome = GetAvgMetalIncome();
	if ((energyIncome * 0.8 > metalIncome) && circuit->IsAvailable(mexDef)) {
		float cost = mexDef->GetCost(metalRes);
		int count = builderManager->GetBuilderPower() / cost * 4 + 2;
		if (builderManager->GetTasks(IBuilderTask::BuildType::MEX).size() < count) {
			CMetalManager* metalManager = circuit->GetMetalManager();
			const CMetalData::Metals& spots = metalManager->GetSpots();
			Map* map = circuit->GetMap();
			UnitDef* metalDef = mexDef;
			CMetalData::MetalPredicate predicate;
			if (unit != nullptr) {
				CTerrainManager* terrainManager = circuit->GetTerrainManager();
				predicate = [&spots, metalManager, map, metalDef, terrainManager, unit](CMetalData::MetalNode const& v) {
					int index = v.second;
					return (metalManager->IsOpenSpot(index) &&
							terrainManager->CanBuildAt(unit, spots[index].position) &&
							map->IsPossibleToBuildAt(metalDef, spots[index].position, UNIT_COMMAND_BUILD_NO_FACING));
				};
			} else {
				predicate = [&spots, metalManager, map, metalDef, builderManager](CMetalData::MetalNode const& v) {
					int index = v.second;
					return (metalManager->IsOpenSpot(index) &&
							builderManager->IsBuilderInArea(metalDef, spots[index].position) &&
							map->IsPossibleToBuildAt(metalDef, spots[index].position, UNIT_COMMAND_BUILD_NO_FACING));
				};
			}
			int index = metalManager->FindNearestSpot(position, predicate);
			if (index != -1) {
				metalManager->SetOpenSpot(index, false);
				const AIFloat3& pos = spots[index].position;
				task = builderManager->EnqueueTask(IBuilderTask::Priority::HIGH, mexDef, pos, IBuilderTask::BuildType::MEX, cost);
				task->SetBuildPos(pos);
			}
		}
	}

	return task;
}

IBuilderTask* CEconomyManager::UpdateEnergyTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	IBuilderTask* task = nullptr;
	if (!builderManager->CanEnqueueTask()) {
		return task;
	}

	// check energy / metal ratio
	float energyIncome = GetAvgEnergyIncome();
	float metalIncome = GetAvgMetalIncome();
	float energyUsage = eco->GetUsage(energyRes);

	if ((metalIncome > energyIncome * 0.8) || (energyUsage > energyIncome * 0.8)) {
		UnitDef* bestDef = nullptr;
		float cost;
		float buildPower = std::min(builderManager->GetBuilderPower(), metalIncome * 0.5f);
		const std::set<IBuilderTask*>& tasks = builderManager->GetTasks(IBuilderTask::BuildType::ENERGY);
		for (auto& engy : energyInfos) {  // sorted by high-tech first
			// TODO: Add geothermal powerplant support
			if (!circuit->IsAvailable(engy.def) || engy.def->IsNeedGeo()) {
				continue;
			}

			if (circuit->GetCircuitDef(engy.def)->GetCount() < engy.limit) {
				int count = buildPower / engy.cost * 4 + 1;
				if (tasks.size() < count) {
					cost = engy.cost;
					bestDef = engy.def;
					// TODO: Select proper scale/quadratic function (x*x) and smoothing coefficient (8).
					//       МЕТОД НАИМЕНЬШИХ КВАДРАТОВ ! (income|buildPower, make/cost) - points
					//       solar       geothermal    fusion         singu           ...
					//       (10, 2/70), (15, 25/500), (20, 35/1000), (30, 225/4000), ...
					if (cost / (buildPower * buildPower / 8) < MAX_BUILD_SEC / ecoFactor) {
						break;
					}
				}
			} else {
				bestDef = nullptr;
				break;
			}
		}

		if (bestDef != nullptr) {
			AIFloat3 buildPos = -RgtVector;

			CMetalManager* metalManager = circuit->GetMetalManager();
			if (cost / std::min(builderManager->GetBuilderPower(), metalIncome) < MIN_BUILD_SEC) {
				int index = metalManager->FindNearestSpot(position);
				if (index != -1) {
					const CMetalData::Metals& spots = metalManager->GetSpots();
					buildPos = spots[index].position;
				}
			} else {
				const AIFloat3& startPos = circuit->GetSetupManager()->GetStartPos();
				int index = metalManager->FindNearestCluster(startPos);
				if (index >= 0) {
					const CMetalData::Clusters& clusters = metalManager->GetClusters();
					buildPos = clusters[index].geoCentr;
				}
			}

			CTerrainManager* terrainManager = circuit->GetTerrainManager();
			if ((buildPos != -RgtVector) && terrainManager->CanBeBuiltAt(circuit->GetCircuitDef(bestDef), buildPos) &&
					((unit == nullptr) || terrainManager->CanBuildAt(unit, buildPos)))
			{
				task = builderManager->EnqueueTask(IBuilderTask::Priority::NORMAL, bestDef, buildPos, IBuilderTask::BuildType::ENERGY, cost);
			}
		}
	}

	return task;
}

IBuilderTask* CEconomyManager::UpdateFactoryTasks(const AIFloat3& position, CCircuitUnit* unit)
{
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	IBuilderTask* task = nullptr;
	if (!builderManager->CanEnqueueTask()) {
		return task;
	}

	CFactoryManager* factoryManager = circuit->GetFactoryManager();
	UnitDef* assistDef = factoryManager->GetAssistDef();
	UnitDef* facDef = circuit->GetUnitDefByName("factorycloak");

	// check buildpower
	float metalIncome = GetAvgMetalIncome();
	if ((factoryManager->GetFactoryPower() < metalIncome) &&
			builderManager->GetTasks(IBuilderTask::BuildType::FACTORY).empty() && builderManager->GetTasks(IBuilderTask::BuildType::NANO).empty())
	{
		CCircuitUnit* factory = factoryManager->NeedUpgrade();
		if ((factory != nullptr) && circuit->IsAvailable(assistDef)) {
			Unit* u = factory->GetUnit();
			UnitDef* def = factory->GetDef();
			AIFloat3 buildPos = u->GetPos();
			switch (u->GetBuildingFacing()) {
				default:
				case UNIT_FACING_SOUTH:
					buildPos.z -= def->GetZSize() * SQUARE_SIZE;
					break;
				case UNIT_FACING_EAST:
					buildPos.x -= def->GetXSize() * SQUARE_SIZE;
					break;
				case UNIT_FACING_NORTH:
					buildPos.z += def->GetZSize() * SQUARE_SIZE;
					break;
				case UNIT_FACING_WEST:
					buildPos.x += def->GetXSize() * SQUARE_SIZE;
					break;
			}

			CTerrainManager* terrainManager = circuit->GetTerrainManager();
			if (terrainManager->CanBeBuiltAt(circuit->GetCircuitDef(assistDef), buildPos) &&
					((unit == nullptr) || terrainManager->CanBuildAt(unit, buildPos)))
			{
				task = builderManager->EnqueueTask(IBuilderTask::Priority::NORMAL, assistDef, buildPos, IBuilderTask::BuildType::NANO);
			}

		} else if (circuit->IsAvailable(facDef)) {

			CMetalData::MetalPredicate predicate = [this](const CMetalData::MetalNode& v) {
				return clusterInfos[v.second].factory == nullptr;
			};
			CMetalManager* metalManager = circuit->GetMetalManager();
			int index = metalManager->FindNearestCluster(position, predicate);
			CTerrainManager* terrain = circuit->GetTerrainManager();
			AIFloat3 buildPos;
			if (index >= 0) {
				const CMetalData::Clusters& clusters = metalManager->GetClusters();
				buildPos = clusters[index].geoCentr;
				float size = std::max(facDef->GetXSize(), facDef->GetZSize()) * SQUARE_SIZE;
				buildPos.x += (buildPos.x > terrain->GetTerrainWidth() / 2) ? -size : size;
				buildPos.z += (buildPos.z > terrain->GetTerrainHeight() / 2) ? -size : size;

				CTerrainManager* terrainManager = circuit->GetTerrainManager();
				if (terrainManager->CanBeBuiltAt(circuit->GetCircuitDef(facDef), buildPos) &&
						((unit == nullptr) || terrainManager->CanBuildAt(unit, buildPos)))
				{
					task = builderManager->EnqueueTask(IBuilderTask::Priority::HIGH, facDef, buildPos, IBuilderTask::BuildType::FACTORY);
				}
			}
		}
	}

	return task;
}

CRecruitTask* CEconomyManager::UpdateRecruitTasks()
{
	CFactoryManager* factoryManager = circuit->GetFactoryManager();
	CRecruitTask* task = nullptr;
	if (!factoryManager->CanEnqueueTask()) {
		return task;
	}

	UnitDef* buildDef = circuit->GetUnitDefByName("armrectr");

	float metalIncome = GetAvgMetalIncome();
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	// TODO: Create ReclaimTask for 20% of workers, and 20% RepairTask.
	if ((builderManager->GetBuilderPower() < metalIncome * 1.5) && circuit->IsAvailable(buildDef)) {
		for (auto t : factoryManager->GetTasks()) {
			if (t->GetFacType() == CRecruitTask::FacType::BUILDPOWER) {
				return task;
			}
		}
		CCircuitUnit* factory = factoryManager->GetRandomFactory();
		const AIFloat3& buildPos = factory->GetUnit()->GetPos();
		CTerrainManager* terrain = circuit->GetTerrainManager();
		float radius = std::max(terrain->GetTerrainWidth(), terrain->GetTerrainHeight()) / 4;
		task = factoryManager->EnqueueTask(CRecruitTask::Priority::NORMAL, buildDef, buildPos, CRecruitTask::FacType::BUILDPOWER, 1, radius);
	}

	return task;
}

IBuilderTask* CEconomyManager::UpdateStorageTasks()
{
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	IBuilderTask* task = nullptr;
	if (!builderManager->CanEnqueueTask()) {
		return task;
	}

	UnitDef* storeDef = circuit->GetUnitDefByName("armmstor");
	UnitDef* pylonDef = circuit->GetUnitDefByName("armestor");

	float metalIncome = GetAvgMetalIncome();
	float storage = eco->GetStorage(metalRes);
	if (builderManager->GetTasks(IBuilderTask::BuildType::STORE).empty() && (storage / metalIncome < 25) && circuit->IsAvailable(storeDef)) {
		const AIFloat3& startPos = circuit->GetSetupManager()->GetStartPos();
		CMetalManager* metalManager = circuit->GetMetalManager();
		int index = metalManager->FindNearestSpot(startPos);
		AIFloat3 buildPos;
		if (index != -1) {
			const CMetalData::Metals& spots = metalManager->GetSpots();
			buildPos = spots[index].position;
		} else {
			CTerrainManager* terrain = circuit->GetTerrainManager();
			int terWidth = terrain->GetTerrainWidth();
			int terHeight = terrain->GetTerrainHeight();
			float x = terWidth/4 + rand() % (int)(terWidth/2 + 1);
			float z = terHeight/4 + rand() % (int)(terHeight/2 + 1);
			buildPos = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
		}
		task = builderManager->EnqueueTask(IBuilderTask::Priority::HIGH, storeDef, buildPos, IBuilderTask::BuildType::STORE);
		return task;
	}

	float energyIncome = GetAvgEnergyIncome();
	if ((metalIncome > 10 * ecoFactor) && (energyIncome > 100) && (pylonCount < pylonMaxCount) && circuit->IsAvailable(pylonDef)) {
		float cost = pylonDef->GetCost(metalRes);
		int count = builderManager->GetBuilderPower() / cost * 2 + 1;
		if (builderManager->GetTasks(IBuilderTask::BuildType::PYLON).size() < count) {
//			CMetalData::MetalPredicate predicate = [this](const CMetalData::MetalNode& v) {
//				return clusterInfos[v.second].pylon == nullptr;
//			};
			CMetalManager* metalManager = circuit->GetMetalManager();
			const AIFloat3& startPos = circuit->GetSetupManager()->GetStartPos();
			int index = metalManager->FindNearestCluster(startPos/*, predicate*/);
			if (index >= 0) {
				const CMetalData::Clusters& clusters = metalManager->GetClusters();
				task = builderManager->EnqueueTask(IBuilderTask::Priority::NORMAL, pylonDef, clusters[index].geoCentr, IBuilderTask::BuildType::PYLON);
				return task;
			}
		}
	}

	return task;
}

void CEconomyManager::Init()
{
	const CMetalData::Clusters& clusters = circuit->GetMetalManager()->GetClusters();
	clusterInfos.resize(clusters.size());

	for (int k = 0; k < clusters.size(); ++k) {
		clusterInfos[k] = {nullptr};
	}

	SkirmishAIs* ais = circuit->GetCallback()->GetSkirmishAIs();
	const int interval = ais->GetSize() * 2;
	delete ais;
	const AIFloat3& pos = circuit->GetSetupManager()->GetStartPos();
	CScheduler* scheduler = circuit->GetScheduler().get();
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateFactoryTasks, this, pos, nullptr), interval, circuit->GetSkirmishAIId() + 0 + 10 * interval);
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CEconomyManager::UpdateStorageTasks, this), interval, circuit->GetSkirmishAIId() + 1);
}

} // namespace circuit
