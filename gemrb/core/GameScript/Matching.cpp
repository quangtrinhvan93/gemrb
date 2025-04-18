/* GemRB - Infinity Engine Emulator
 * Copyright (C) 2003-2005 The GemRB Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 */

#include "GameScript/Matching.h"

#include "ie_stats.h"

#include "Game.h"
#include "Interface.h"
#include "Map.h"
#include "SymbolMgr.h"
#include "TileMap.h"

#include "GameScript/GSUtils.h"
#include "Scriptable/Container.h"
#include "Scriptable/Door.h"
#include "Scriptable/InfoPoint.h"

namespace GemRB {

/* return a Targets object with a single scriptable inside */
static inline Targets* ReturnScriptableAsTarget(Scriptable* sc)
{
	if (!sc) return NULL;
	Targets* tgts = new Targets();
	tgts->AddTarget(sc, 0, 0);
	return tgts;
}

/* do IDS filtering: [PC], [ENEMY], etc */
// at least in iwd2, it is explicitly confirmed that these respect visibility,
// but that is handled outside this function
static inline bool DoObjectIDSCheck(const Object* oC, const Actor* ac, bool* filtered)
{
	for (int j = 0; j < ObjectIDSCount; j++) {
		if (!oC->objectFields[j]) {
			continue;
		}
		*filtered = true;
		IDSFunction func = idtargets[j];
		if (!func) {
			Log(WARNING, "GameScript", "Unimplemented IDS targeting opcode: {}", j);
			continue;
		}
		if (!func(ac, oC->objectFields[j])) {
			return false;
		}
	}
	return true;
}

/* do object filtering: Myself, LastAttackerOf(Player1), etc */
static inline Targets* DoObjectFiltering(const Scriptable* Sender, Targets* tgts, const Object* oC, int ga_flags)
{
	// at least in iwd2, this ignores invisibility, except for filters that check the area (like NearestEnemyOf)
	// for simplicity we disable it for all and reenable it in XthNearestEnemyOf
	if (core->HasFeature(GFFlags::RULES_3ED)) {
		ga_flags &= ~GA_NO_HIDDEN;
	}

	targetlist::iterator m;
	const targettype* tt = tgts->GetFirstTarget(m, ST_ACTOR);
	while (tt) {
		const Actor* target = static_cast<const Actor*>(tt->actor);
		if (oC->objectName[0] || target->ValidTarget(GA_NO_DEAD)) {
			tt = tgts->GetNextTarget(m, ST_ACTOR);
		} else {
			tt = tgts->RemoveTargetAt(m);
		}
	}

	for (int i = 0; i < MaxObjectNesting; i++) {
		int filterid = oC->objectFilters[i];
		if (!filterid) break;
		if (filterid < 0) continue;

		ObjectFunction func = objects[filterid];
		if (!func) {
			Log(WARNING, "GameScript", "Unknown object filter: {} {}",
			    filterid, objectsTable->GetValue(filterid));
			continue;
		}

		tgts = func(Sender, tgts, ga_flags);
		if (!tgts->Count()) {
			delete tgts;
			return NULL;
		}
	}
	return tgts;
}

static EffectRef fx_protection_creature_ref = { "Protection:Creature", -1 };

static inline bool DoObjectChecks(const Map* map, const Scriptable* Sender, Actor* target, int& dist, bool ignoreinvis = false, const Object* oC = nullptr)
{
	dist = SquaredDistance(Sender, target); // good enough for sorting actors, but we don't use it below

	// TODO: what do we check for non-actors?
	// non-actors have a visual range (15), we should do visual range and LOS
	// see voodooconst.h for more info, currently the rest of the code uses 30 for non-actors

	if (Sender->Type != ST_ACTOR) return true;

	// Detect() ignores invisibility completely
	const Actor* source = static_cast<const Actor*>(Sender);
	if (!ignoreinvis && target->IsInvisibleTo(source)) {
		return false;
	}

	// visual range or objectRect check (if it's a valid objectRect)
	if (HasAdditionalRect && oC && oC->objectRect.size.Area() > 0) {
		if (!IsInObjectRect(target->Pos, oC->objectRect)) {
			return false;
		}
	} else if (!WithinRange(source, target->Pos, source->GetVisualRange())) {
		return false;
	}

	// line of sight check
	if (!map->IsVisibleLOS(Sender->SMPos, target->SMPos, source)) return false;

	// protection against creature
	if (target->fxqueue.HasEffect(fx_protection_creature_ref)) {
		// TODO: de-hardcode these (may not all be correct anyway)
		ieDword idsStat[] = { IE_EA, IE_GENERAL, IE_RACE, IE_CLASS, IE_SPECIFIC, IE_SEX, IE_ALIGNMENT };
		for (int i = 0; i < 7; i++) {
			ieDword statValue = source->Modified[idsStat[i]];
			if (idsStat[i] == IE_CLASS) {
				statValue = source->GetActiveClass();
			}
			if (target->fxqueue.HasEffectWithParamPair(fx_protection_creature_ref, statValue, i + 2)) {
				return false;
			}
		}
	}

	return true;
}

/* returns actors that match the [x.y.z] expression */
static Targets* EvaluateObject(const Map* map, const Scriptable* Sender, const Object* oC, int ga_flags)
{
	// if you ActionOverride a global actor, they might not have a map :(
	// TODO: don't allow this to happen?
	if (!map) {
		return NULL;
	}

	if (oC->objectName[0]) {
		//We want the object by its name...
		Scriptable* aC = map->GetActor(oC->objectNameVar, ga_flags);

		if (!aC) {
			aC = GetActorObject(map->GetTileMap(), oC->objectName);
		}

		//return here because object name/IDS targeting are mutually exclusive
		return ReturnScriptableAsTarget(aC);
	}

	if (oC->objectFields[0] == -1) {
		// this is an internal hack, allowing us to pass actor ids around as objects
		Actor* aC = map->GetActorByGlobalID((ieDword) oC->objectFields[1]);
		if (aC) {
			if (!aC->ValidTarget(ga_flags)) {
				return NULL;
			}
			return ReturnScriptableAsTarget(aC);
		}

		// meh, preserving constness
		Map* map2 = core->GetGame()->GetMap(map->GetScriptRef(), false);
		Scriptable* target = map2->GetScriptableByGlobalID(static_cast<ieDword>(oC->objectFields[1]));
		if (target != map2) {
			return ReturnScriptableAsTarget(target);
		} else {
			return nullptr;
		}
	}

	Targets* tgts = NULL;

	//we need to get a subset of actors from the large array
	//if this gets slow, we will need some index tables
	int i = map->GetActorCount(true);
	while (i--) {
		Actor* ac = map->GetActor(i, true);
		if (!ac) continue; // is this check really needed?
		// don't return Sender in IDS targeting!
		// unless it's pst, which relies on it in 3012cut2-3012cut7.bcs
		// FIXME: stop abusing old GF flags
		if (!core->HasFeature(GFFlags::AREA_OVERRIDE) && ac == Sender) continue;

		bool filtered = false;
		if (!DoObjectIDSCheck(oC, ac, &filtered)) {
			continue;
		}

		// this is needed so eg. Range trigger gets a good object
		if (!filtered) {
			// if no filters were applied..
			assert(!tgts);
			return nullptr;
		}
		int dist;
		if (DoObjectChecks(map, Sender, ac, dist, (ga_flags & GA_DETECT) != 0, oC)) {
			if (!tgts) tgts = new Targets();
			tgts->AddTarget((Scriptable*) ac, dist, ga_flags);
		}
	}

	return tgts;
}

Targets* GetAllObjects(const Map* map, Scriptable* Sender, const Action* parameters, int gaFlags)
{
	return GetAllObjects(map, Sender, parameters->objects[1], gaFlags, parameters->flags & ACF_MISSING_OBJECT);
}

Targets* GetAllObjects(const Map* map, Scriptable* Sender, const Trigger* parameters, int gaFlags)
{
	return GetAllObjects(map, Sender, parameters->objectParameter, gaFlags, parameters->flags & TF_MISSING_OBJECT);
}

Targets* GetAllObjects(const Map* map, Scriptable* Sender, const Object* oC, int ga_flags, bool anyone)
{
	// jump through hoops for [ANYONE]
	if (!oC && !anyone) {
		//return all objects
		return GetAllActors(Sender, ga_flags);
	}

	Targets* tgts;
	if (anyone) {
		tgts = GetAllActors(Sender, ga_flags);
		if (tgts) tgts->Pop(); // remove self
		return tgts;
	} else {
		tgts = EvaluateObject(map, Sender, oC, ga_flags);
	}
	//if we couldn't find an endpoint by name or object qualifiers
	//it is not an Actor, but could still be a Door or Container (scriptable)
	if (!tgts && oC->objectName[0]) {
		return NULL;
	}
	//now lets do the object filter stuff, we create Targets because
	//it is possible to start from blank sheets using endpoint filters
	//like (Myself, Protagonist etc)
	if (!tgts) {
		tgts = new Targets();
	}
	tgts = DoObjectFiltering(Sender, tgts, oC, ga_flags);
	if (tgts) {
		tgts->FilterObjectRect(oC);
	}
	return tgts;
}

Targets* GetAllActors(Scriptable* Sender, int ga_flags)
{
	const Map* map = Sender->GetCurrentArea();

	int i = map->GetActorCount(true);
	Targets* tgts = new Targets();
	//make sure that Sender is always first in the list, even if there
	//are other (e.g. dead) targets at the same location
	tgts->AddTarget(Sender, 0, ga_flags);
	while (i--) {
		Actor* ac = map->GetActor(i, true);
		if (ac != Sender) {
			int dist = Distance(Sender->Pos, ac->Pos);
			tgts->AddTarget(ac, dist, ga_flags);
		}
	}
	return tgts;
}

/* get a non-actor object from a map, by name */
Scriptable* GetActorObject(const TileMap* TMap, const ieVariable& name)
{
	Scriptable* aC = TMap->GetDoor(name);
	if (aC) {
		return aC;
	}

	//containers should have a precedence over infopoints because otherwise
	//AR1512 sanity test quest would fail
	//If this order couldn't be maintained, then 'Contains' should have a
	//unique call to get containers only

	//No... it was not an door... maybe a Container?
	aC = TMap->GetContainer(name);
	if (aC) {
		return aC;
	}

	//No... it was not a container ... maybe an InfoPoint?
	aC = TMap->GetInfoPoint(name);
	return aC;
}

// blocking actions need to store some kinds of objects between ticks
Scriptable* GetStoredActorFromObject(Scriptable* Sender, const Action* parameters, int gaFlags)
{
	return GetStoredActorFromObject(Sender, parameters->objects[1], gaFlags, parameters->flags & ACF_MISSING_OBJECT);
}

Scriptable* GetStoredActorFromObject(Scriptable* Sender, const Object* oC, int ga_flags, bool anyone)
{
	Scriptable* tar = NULL;
	const Actor* target;
	// retrieve an existing target if it still exists and is valid
	if (Sender->CurrentActionTarget) {
		tar = core->GetGame()->GetActorByGlobalID(Sender->CurrentActionTarget);
		target = Scriptable::As<Actor>(tar);
		if (target && target->ValidTarget(ga_flags, Sender)) {
			return tar;
		}
		return NULL; // target invalid/gone
	}
	tar = GetScriptableFromObject(Sender, oC, ga_flags, anyone);
	target = Scriptable::As<Actor>(tar);
	// maybe store the target if it's an actor..
	// .. but we only want objects created via objectFilters
	if (target && oC && oC->objectFilters[0]) {
		Sender->CurrentActionTarget = tar->GetGlobalID();
	}
	return tar;
}

Scriptable* GetScriptableFromObject(Scriptable* Sender, const Trigger* parameters, int gaFlags)
{
	return GetScriptableFromObject(Sender, parameters->objectParameter, gaFlags, parameters->flags & TF_MISSING_OBJECT);
}

Scriptable* GetScriptableFromObject(Scriptable* Sender, const Action* parameters, int gaFlags)
{
	return GetScriptableFromObject(Sender, parameters->objects[1], gaFlags, parameters->flags & ACF_MISSING_OBJECT);
}

Scriptable* GetScriptableFromObject2(Scriptable* Sender, const Action* parameters, int gaFlags)
{
	return GetScriptableFromObject(Sender, parameters->objects[2], gaFlags, parameters->flags & ACF_MISSING_OBJECT);
}

Scriptable* GetScriptableFromObject(Scriptable* Sender, const Object* oC, int gaFlags, bool anyone)
{
	Scriptable* aC = nullptr;

	const Game* game = core->GetGame();
	Targets* tgts = GetAllObjects(Sender->GetCurrentArea(), Sender, oC, gaFlags, anyone);
	if (tgts) {
		//now this could return other than actor objects
		aC = tgts->GetTarget(0, ST_ANY);
		delete tgts;
		if (aC || !oC || oC->objectFields[0] != -1) {
			return aC;
		}

		//global actors are always found by object ID!
		return game->GetGlobalActorByGlobalID(oC->objectFields[1]);
	}

	if (!oC) {
		return NULL;
	}

	if (oC->objectName[0]) {
		// if you ActionOverride a global actor, they might not have a map :(
		// TODO: don't allow this to happen?
		if (Sender->GetCurrentArea()) {
			aC = GetActorObject(Sender->GetCurrentArea()->GetTileMap(), oC->objectName);
			if (aC) {
				return aC;
			}
		}

		//global actors are always found by scripting name!
		aC = game->FindPC(oC->objectNameVar);
		if (aC) {
			return aC;
		}
		aC = game->FindNPC(oC->objectNameVar);
		if (aC) {
			return aC;
		}
	}
	return NULL;
}

bool MatchActor(const Scriptable* Sender, ieDword actorID, const Object* oC)
{
	if (!Sender) {
		return false;
	}
	Actor* ac = Sender->GetCurrentArea()->GetActorByGlobalID(actorID);
	if (!ac) {
		return false;
	}

	// [0]/[ANYONE] can match all actors
	if (!oC) {
		return true;
	}

	if (!IsInObjectRect(ac->Pos, oC->objectRect)) {
		return false;
	}

	bool filtered = false;

	// name matching
	if (oC->objectName[0]) {
		if (ac->GetScriptName() != oC->objectNameVar) {
			return false;
		}
		filtered = true;
	}

	// IDS targeting
	// (if we already matched by name, we don't do this)
	// TODO: check distance? area? visibility?
	if (!filtered && !DoObjectIDSCheck(oC, ac, &filtered)) return false;

	// globalID hack should never get here
	assert(oC->objectFilters[0] != -1);

	// object filters
	if (oC->objectFilters[0]) {
		// object filters insist on having a stupid targets list,
		// so we waste a lot of time here
		Targets* tgts = new Targets();
		int ga_flags = 0; // TODO: correct?

		// handle already-filtered vs not-yet-filtered cases
		// e.g. LastTalkedToBy(Myself) vs LastTalkedToBy
		if (filtered) tgts->AddTarget(ac, 0, ga_flags);

		tgts = DoObjectFiltering(Sender, tgts, oC, ga_flags);
		if (!tgts) return false;

		// and sometimes object filters are lazy and not only don't filter
		// what we give them, they clear it and return a list :(
		// so we have to search the whole list..
		bool ret = false;
		targetlist::iterator m;
		const targettype* tt = tgts->GetFirstTarget(m, ST_ACTOR);
		while (tt) {
			const Actor* actor = static_cast<const Actor*>(tt->actor);
			if (actor->GetGlobalID() == actorID) {
				ret = true;
				break;
			}
			tt = tgts->GetNextTarget(m, ST_ACTOR);
		}
		delete tgts;
		if (!ret) return false;
	}
	return true;
}

int GetObjectCount(Scriptable* Sender, const Trigger* parameters)
{
	const Object* oC = parameters->objectParameter;
	return GetObjectCount(Sender, oC, parameters->flags & TF_MISSING_OBJECT);
}

int GetObjectCount(Scriptable* Sender, const Object* oC, bool anyone)
{
	if (!oC && !anyone) {
		return 0;
	}
	// EvaluateObject will return [PC]
	// GetAllObjects will also return Myself (evaluates object filters)
	// i believe we need the latter here
	Targets* tgts = GetAllObjects(Sender->GetCurrentArea(), Sender, oC, 0, anyone);
	int count = 0; // silly fallback to avoid potential crashes
	if (tgts) {
		count = static_cast<int>(tgts->Count());
		delete tgts;
	}
	return count;
}

//TODO:
//check numcreaturesatmylevel(myself, 1)
//when the actor is alone
//it should (obviously) return true if the trigger
//evaluates object filters
//also check numcreaturesgtmylevel(myself,0) with
//actor having at high level
int GetObjectLevelCount(Scriptable* Sender, const Trigger* parameters)
{
	const Object* oC = parameters->objectParameter;
	bool anyone = parameters->flags & TF_MISSING_OBJECT;
	if (!oC && !anyone) {
		return 0;
	}
	// EvaluateObject will return [PC]
	// GetAllObjects will also return Myself (evaluates object filters)
	// i believe we need the latter here
	Targets* tgts = GetAllObjects(Sender->GetCurrentArea(), Sender, oC, 0, anyone);
	int count = 0;
	if (tgts) {
		targetlist::iterator m;
		const targettype* tt = tgts->GetFirstTarget(m, ST_ACTOR);
		while (tt) {
			count += ((Actor*) tt->actor)->GetXPLevel(true);
			tt = tgts->GetNextTarget(m, ST_ACTOR);
		}
	}
	delete tgts;
	return count;
}

Targets* GetMyTarget(const Scriptable* Sender, const Actor* actor, Targets* parameters, int ga_flags)
{
	if (!actor && Sender->Type == ST_ACTOR) {
		actor = static_cast<const Actor*>(Sender);
	}
	parameters->Clear();
	if (actor) {
		// NOTE: bgs just checked a separate variable, only set in Attack actions when
		// the target changed, so this is potentially wrong (spell actions could change LastTarget)
		// in vanilla games it's only used once, in iwd2
		Actor* target = actor->GetCurrentArea()->GetActorByGlobalID(actor->objects.LastTarget);
		if (target) {
			parameters->AddTarget(target, 0, ga_flags);
		}
	}
	return parameters;
}

Targets* XthNearestDoor(Targets* parameters, unsigned int count)
{
	//get the origin
	Scriptable* origin = parameters->GetTarget(0, ST_ANY);
	parameters->Clear();
	if (!origin) {
		return parameters;
	}
	//get the doors based on it
	const Map* map = origin->GetCurrentArea();
	unsigned int i = (unsigned int) map->TMap->GetDoorCount();
	if (count > i) {
		return parameters;
	}
	for (const auto& door : map->TMap->GetDoors()) {
		unsigned int dist = Distance(origin->Pos, door->Pos);
		parameters->AddTarget(door, dist, 0);
	}

	//now get the xth door
	origin = parameters->GetTarget(count, ST_DOOR);
	parameters->Clear();
	if (!origin) {
		return parameters;
	}
	parameters->AddTarget(origin, 0, 0);
	return parameters;
}

Targets* XthNearestOf(Targets* parameters, int count, int ga_flags)
{
	Scriptable* origin;

	if (count < 0) {
		const targettype* t = parameters->GetLastTarget(ST_ACTOR);
		if (!t) {
			parameters->Clear();
			return parameters;
		}
		origin = t->actor;
	} else {
		origin = parameters->GetTarget(count, ST_ACTOR);
	}
	parameters->Clear();
	if (!origin) {
		return parameters;
	}
	parameters->AddTarget(origin, 0, ga_flags);
	return parameters;
}

//mygroup means the same specifics as origin
Targets* XthNearestMyGroupOfType(const Scriptable* origin, Targets* parameters, unsigned int count, int ga_flags)
{
	if (origin->Type != ST_ACTOR) {
		parameters->Clear();
		return parameters;
	}

	targetlist::iterator m;
	const targettype* t = parameters->GetFirstTarget(m, ST_ACTOR);
	if (!t) {
		return parameters;
	}
	const Actor* actor = static_cast<const Actor*>(origin);
	//determining the specifics of origin
	ieDword type = actor->GetStat(IE_SPECIFIC); //my group

	while (t) {
		if (t->actor->Type != ST_ACTOR) {
			t = parameters->RemoveTargetAt(m);
			continue;
		}
		actor = static_cast<const Actor*>(t->actor);
		if (actor->GetStat(IE_SPECIFIC) != type) {
			t = parameters->RemoveTargetAt(m);
			continue;
		}
		t = parameters->GetNextTarget(m, ST_ACTOR);
	}
	return XthNearestOf(parameters, count, ga_flags);
}

Targets* ClosestEnemySummoned(const Scriptable* origin, Targets* parameters, int ga_flags)
{
	if (origin->Type != ST_ACTOR) {
		parameters->Clear();
		return parameters;
	}

	targetlist::iterator m;
	const targettype* t = parameters->GetFirstTarget(m, ST_ACTOR);
	if (!t) {
		return parameters;
	}
	const Actor* sender = static_cast<const Actor*>(origin);
	//determining the allegiance of the origin
	GroupType type = GetGroup(sender);

	if (type == GroupType::Neutral) {
		parameters->Clear();
		return parameters;
	}

	Actor* actor = nullptr;
	ieDword gametime = core->GetGame()->GameTime;
	while (t) {
		Actor* tmp = (Actor*) (t->actor);
		if (tmp->GetStat(IE_SEX) != SEX_SUMMON) {
			t = parameters->GetNextTarget(m, ST_ACTOR);
			continue;
		}
		if (!tmp->Schedule(gametime, true)) {
			t = parameters->GetNextTarget(m, ST_ACTOR);
			continue;
		}
		if (type == GroupType::PC) {
			if (tmp->GetStat(IE_EA) <= EA_GOODCUTOFF) {
				t = parameters->GetNextTarget(m, ST_ACTOR);
				continue;
			}
		} else { // GroupType::Enemy
			if (tmp->GetStat(IE_EA) >= EA_EVILCUTOFF) {
				t = parameters->GetNextTarget(m, ST_ACTOR);
				continue;
			}
		}
		actor = tmp;
		t = parameters->GetNextTarget(m, ST_ACTOR);
	}
	parameters->Clear();
	parameters->AddTarget(actor, 0, ga_flags);
	return parameters;
}

// bg2 and ee only
Targets* XthNearestEnemyOfType(const Scriptable* origin, Targets* parameters, unsigned int count, int ga_flags)
{
	if (origin->Type != ST_ACTOR) {
		parameters->Clear();
		return parameters;
	}

	targetlist::iterator m;
	const targettype* t = parameters->GetFirstTarget(m, ST_ACTOR);
	if (!t) {
		return parameters;
	}
	const Actor* actor = static_cast<const Actor*>(origin);
	//determining the allegiance of the origin
	GroupType type = GetGroup(actor);

	if (type == GroupType::Neutral) {
		parameters->Clear();
		return parameters;
	}

	ieDword gametime = core->GetGame()->GameTime;
	while (t) {
		if (t->actor->Type != ST_ACTOR) {
			t = parameters->RemoveTargetAt(m);
			continue;
		}
		actor = static_cast<const Actor*>(t->actor);
		// IDS targeting already did object checks (unless we need to override Detect?)
		if (!actor->Schedule(gametime, true)) {
			t = parameters->RemoveTargetAt(m);
			continue;
		}
		if (type == GroupType::PC) {
			if (actor->GetStat(IE_EA) <= EA_EVILCUTOFF) {
				t = parameters->RemoveTargetAt(m);
				continue;
			}
		} else {
			if (actor->GetStat(IE_EA) >= EA_GOODCUTOFF) {
				t = parameters->RemoveTargetAt(m);
				continue;
			}
		}
		t = parameters->GetNextTarget(m, ST_ACTOR);
	}
	return XthNearestOf(parameters, count, ga_flags);
}

Targets* XthNearestEnemyOf(Targets* parameters, int count, int gaFlags, bool farthest)
{
	Actor* origin = static_cast<Actor*>(parameters->GetTarget(0, ST_ACTOR));
	parameters->Clear();
	if (!origin) {
		return parameters;
	}
	// determining the allegiance of the origin
	GroupType type = GetGroup(origin);
	if (type == GroupType::Neutral) {
		return parameters;
	}

	if (core->HasFeature(GFFlags::RULES_3ED)) {
		// odd iwd2 detail for actors, turn off extra true seeing first (yes, permanently)
		// only happened for *NearestEnemyOf and FarthestEnemyOf
		if (origin->GetSafeStat(IE_MC_FLAGS) & MC_SEENPARTY && origin->GetSafeStat(IE_EA) > EA_NOTEVIL) {
			origin->SetMCFlag(MC_SEENPARTY, BitOp::NAND);
		}

		// also (re)enable visibility checks that were disabled in DoObjectFiltering
		gaFlags |= GA_NO_HIDDEN;
	}

	const Map* map = origin->GetCurrentArea();
	int i = map->GetActorCount(true);
	gaFlags |= GA_NO_UNSCHEDULED | GA_NO_DEAD;
	while (i--) {
		Actor* ac = map->GetActor(i, true);
		if (ac == origin) continue;
		// TODO: if it turns out you need to check Sender here, beware you take the right distance!
		// (n the original games, this is only used for NearestEnemyOf(Player1) in obsgolem.bcs)
		int distance;
		if (!DoObjectChecks(map, origin, ac, distance)) continue;
		if (farthest) {
			// deliberately underflow later, so we can reuse the rest of the code
			distance = -distance;
		}
		if (type == GroupType::PC) {
			if (ac->GetStat(IE_EA) >= EA_EVILCUTOFF) {
				parameters->AddTarget(ac, distance, gaFlags);
			}
		} else { // GroupType::Enemy
			if (ac->GetStat(IE_EA) <= EA_GOODCUTOFF) {
				parameters->AddTarget(ac, distance, gaFlags);
			}
		}
	}
	return XthNearestOf(parameters, count, gaFlags);
}

}
