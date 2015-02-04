/*
 * BuilderTask.cpp
 *
 *  Created on: Sep 11, 2014
 *      Author: rlcevg
 */

#include "task/builder/BuilderTask.h"
#include "task/RetreatTask.h"
#include "task/TaskManager.h"
#include "unit/CircuitUnit.h"
#include "unit/CircuitDef.h"
#include "unit/action/UnitAction.h"
#include "static/MetalManager.h"
#include "module/EconomyManager.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/utils.h"

#include "AISCommands.h"
#include "UnitDef.h"
#include "Unit.h"
#include "Map.h"

namespace circuit {

using namespace springai;

IBuilderTask::IBuilderTask(ITaskManager* mgr, Priority priority,
		UnitDef* buildDef, const AIFloat3& position,
		BuildType type, float cost, int timeout) :
				IUnitTask(mgr, priority, Type::BUILDER),
				buildDef(buildDef),
				position(position),
				buildType(type),
				cost(cost),
				timeout(timeout),
				target(nullptr),
				buildPos(-RgtVector),
				buildPower(.0f),
				facing(UNIT_COMMAND_BUILD_NO_FACING)
{
}

IBuilderTask::~IBuilderTask()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
}

bool IBuilderTask::CanAssignTo(CCircuitUnit* unit)
{
	return (((target != nullptr) || unit->GetCircuitDef()->CanBuild(buildDef)) && (cost > buildPower * MIN_BUILD_SEC));
}

void IBuilderTask::AssignTo(CCircuitUnit* unit)
{
	IUnitTask::AssignTo(unit);

	buildPower += unit->GetDef()->GetBuildSpeed();
}

void IBuilderTask::RemoveAssignee(CCircuitUnit* unit)
{
	IUnitTask::RemoveAssignee(unit);

	buildPower -= unit->GetDef()->GetBuildSpeed();
}

void IBuilderTask::Execute(CCircuitUnit* unit)
{
	Unit* u = unit->GetUnit();

	std::vector<float> params;
	params.push_back(static_cast<float>(priority));
	u->ExecuteCustomCommand(CMD_PRIORITY, params);

	if (target != nullptr) {
		Unit* tu = target->GetUnit();
		u->Build(target->GetDef(), tu->GetPos(), tu->GetBuildingFacing(), UNIT_COMMAND_OPTION_INTERNAL_ORDER, FRAMES_PER_SEC * 60);
		return;
	}
	CCircuitAI* circuit = manager->GetCircuit();
	if (buildPos != -RgtVector) {
		facing = FindFacing(buildDef, buildPos);
		if (circuit->GetMap()->IsPossibleToBuildAt(buildDef, buildPos, facing)) {
			u->Build(buildDef, buildPos, facing, UNIT_COMMAND_OPTION_INTERNAL_ORDER, FRAMES_PER_SEC * 60);
			return;
		} else {
			circuit->GetTerrainManager()->RemoveBlocker(buildDef, buildPos, facing);
		}
	}

	float searchRadius = 100.0f * SQUARE_SIZE;
	facing = FindFacing(buildDef, position);
	CTerrainManager* terrain = circuit->GetTerrainManager();
	buildPos = terrain->FindBuildSite(buildDef, position, searchRadius, facing);
	if (buildPos == -RgtVector) {
		const CMetalData::Clusters& clusters = circuit->GetMetalManager()->GetClusters();
		const CMetalData::MetalIndices indices = circuit->GetMetalManager()->FindNearestClusters(position, 3);
		for (const int idx : indices) {
			facing = FindFacing(buildDef, clusters[idx].geoCentr);
			buildPos = terrain->FindBuildSite(buildDef, clusters[idx].geoCentr, searchRadius, facing);
			if (buildPos != -RgtVector) {
				break;
			}
		}
	}

	if (buildPos != -RgtVector) {
		circuit->GetTerrainManager()->AddBlocker(buildDef, buildPos, facing);
		u->Build(buildDef, buildPos, facing, UNIT_COMMAND_OPTION_INTERNAL_ORDER, FRAMES_PER_SEC * 60);
	} else {
		// Fallback to Guard/Assist/Patrol
		manager->FallbackTask(unit);
	}
}

void IBuilderTask::Update()
{
	// tODO: Analyze nearby situation, maybe cancel this task
	CCircuitAI* circuit = manager->GetCircuit();
	for (auto unit : units) {
		IUnitAction* action = static_cast<IUnitAction*>(unit->Begin());
		if (action->GetType() == IUnitAction::Type::PRE_BUILD) {
			Unit* u = unit->GetUnit();
			const AIFloat3& vel = u->GetVel();
			Resource* metal = circuit->GetEconomyManager()->GetMetalRes();
			if ((vel == ZeroVector) && (u->GetResourceUse(metal) <= 0)) {
				// TODO: Something is on build site, get standing units in radius and push them.
			}
		}
	}
}

void IBuilderTask::OnUnitIdle(CCircuitUnit* unit)
{
	RemoveAssignee(unit);
}

void IBuilderTask::OnUnitDamaged(CCircuitUnit* unit, CCircuitUnit* attacker)
{
	Unit* u = unit->GetUnit();
	// TODO: floating retreat coefficient
	if (u->GetHealth() >= u->GetMaxHealth() * 0.9) {
		return;
	}

	if (target == nullptr) {
		manager->AbortTask(this);
	}

	manager->AssignTask(unit, manager->GetRetreatTask());
}

void IBuilderTask::OnUnitDestroyed(CCircuitUnit* unit, CCircuitUnit* attacker)
{
	manager->AbortTask(this);
}

const AIFloat3& IBuilderTask::GetPos() const
{
	return position;
}

UnitDef* IBuilderTask::GetBuildDef()
{
	return buildDef;
}

IBuilderTask::BuildType IBuilderTask::GetBuildType()
{
	return buildType;
}

float IBuilderTask::GetBuildPower()
{
	return buildPower;
}

float IBuilderTask::GetCost()
{
	return cost;
}

int IBuilderTask::GetTimeout()
{
	return timeout;
}

void IBuilderTask::SetBuildPos(const AIFloat3& pos)
{
	buildPos = pos;
}

const AIFloat3& IBuilderTask::GetBuildPos() const
{
	return buildPos;
}

void IBuilderTask::SetTarget(CCircuitUnit* unit)
{
	target = unit;
	SetBuildPos((unit != nullptr) ? unit->GetUnit()->GetPos() : -RgtVector);
}

CCircuitUnit* IBuilderTask::GetTarget()
{
	return target;
}

bool IBuilderTask::IsStructure()
{
	return (buildType < IBuilderTask::BuildType::MEX);
}

void IBuilderTask::SetFacing(int value)
{
	facing = value;
}

int IBuilderTask::GetFacing()
{
	return facing;
}

int IBuilderTask::FindFacing(springai::UnitDef* buildDef, const springai::AIFloat3& position)
{
	int facing = UNIT_COMMAND_BUILD_NO_FACING;
	CTerrainManager* terrain = manager->GetCircuit()->GetTerrainManager();
	float terWidth = terrain->GetTerrainWidth();
	float terHeight = terrain->GetTerrainHeight();
	if (math::fabs(terWidth - 2 * position.x) > math::fabs(terHeight - 2 * position.z)) {
		facing = (2 * position.x > terWidth) ? UNIT_FACING_WEST : UNIT_FACING_EAST;
	} else {
		facing = (2 * position.z > terHeight) ? UNIT_FACING_NORTH : UNIT_FACING_SOUTH;
	}
	return facing;
}

} // namespace circuit