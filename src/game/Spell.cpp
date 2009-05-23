/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Opcodes.h"
#include "Log.h"
#include "UpdateMask.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "Spell.h"
#include "DynamicObject.h"
#include "Group.h"
#include "UpdateData.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "Policies/SingletonImp.h"
#include "SharedDefines.h"
#include "LootMgr.h"
#include "VMapFactory.h"
#include "BattleGround.h"
#include "Util.h"

#define SPELL_CHANNEL_UPDATE_INTERVAL (1*IN_MILISECONDS)

extern pEffect SpellEffects[TOTAL_SPELL_EFFECTS];

class PrioritizeManaPlayerWraper
{
    friend struct PrioritizeMana;

    public:
        explicit PrioritizeManaPlayerWraper(Player* player) : player(player)
        {
            uint32 maxmana = player->GetMaxPower(POWER_MANA);
            percentMana = maxmana ? player->GetPower(POWER_MANA) * 100 / maxmana : 101;
        }
        Player* getPlayer() const { return player; }
    private:
        Player* player;
        uint32 percentMana;
};

struct PrioritizeMana
{
    int operator()( PrioritizeManaPlayerWraper const& x, PrioritizeManaPlayerWraper const& y ) const
    {
        return x.percentMana < y.percentMana;
    }
};


bool IsQuestTameSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto) return false;

    return spellproto->Effect[0] == SPELL_EFFECT_THREAT
        && spellproto->Effect[1] == SPELL_EFFECT_APPLY_AURA && spellproto->EffectApplyAuraName[1] == SPELL_AURA_DUMMY;
}

SpellCastTargets::SpellCastTargets()
{
    m_unitTarget = NULL;
    m_itemTarget = NULL;
    m_GOTarget   = NULL;

    m_unitTargetGUID   = 0;
    m_GOTargetGUID     = 0;
    m_CorpseTargetGUID = 0;
    m_itemTargetGUID   = 0;
    m_itemTargetEntry  = 0;

    m_srcX = m_srcY = m_srcZ = m_destX = m_destY = m_destZ = 0;
    m_strTarget = "";
    m_targetMask = 0;
}

SpellCastTargets::~SpellCastTargets()
{
}

void SpellCastTargets::setUnitTarget(Unit *target)
{
    if (!target)
        return;

    m_destX = target->GetPositionX();
    m_destY = target->GetPositionY();
    m_destZ = target->GetPositionZ();
    m_unitTarget = target;
    m_unitTargetGUID = target->GetGUID();
    m_targetMask |= TARGET_FLAG_UNIT;
}

void SpellCastTargets::setDestination(float x, float y, float z)
{
    m_destX = x;
    m_destY = y;
    m_destZ = z;
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
}

void SpellCastTargets::setSource(float x, float y, float z)
{
    m_srcX = x;
    m_srcY = y;
    m_srcZ = z;
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setGOTarget(GameObject *target)
{
    m_GOTarget = target;
    m_GOTargetGUID = target->GetGUID();
    //    m_targetMask |= TARGET_FLAG_OBJECT;
}

void SpellCastTargets::setItemTarget(Item* item)
{
    if(!item)
        return;

    m_itemTarget = item;
    m_itemTargetGUID = item->GetGUID();
    m_itemTargetEntry = item->GetEntry();
    m_targetMask |= TARGET_FLAG_ITEM;
}

void SpellCastTargets::setCorpseTarget(Corpse* corpse)
{
    m_CorpseTargetGUID = corpse->GetGUID();
}

void SpellCastTargets::Update(Unit* caster)
{
    m_GOTarget   = m_GOTargetGUID ? caster->GetMap()->GetGameObject(m_GOTargetGUID) : NULL;
    m_unitTarget = m_unitTargetGUID ?
        ( m_unitTargetGUID==caster->GetGUID() ? caster : ObjectAccessor::GetUnit(*caster, m_unitTargetGUID) ) :
    NULL;

    m_itemTarget = NULL;
    if(caster->GetTypeId()==TYPEID_PLAYER)
    {
        if(m_targetMask & TARGET_FLAG_ITEM)
            m_itemTarget = ((Player*)caster)->GetItemByGuid(m_itemTargetGUID);
        else
        {
            Player* pTrader = ((Player*)caster)->GetTrader();
            if(pTrader && m_itemTargetGUID < TRADE_SLOT_COUNT)
                m_itemTarget = pTrader->GetItemByPos(pTrader->GetItemPosByTradeSlot(m_itemTargetGUID));
        }
        if(m_itemTarget)
            m_itemTargetEntry = m_itemTarget->GetEntry();
    }
}

bool SpellCastTargets::read ( WorldPacket * data, Unit *caster )
{
    if(data->rpos()+4 > data->size())
        return false;

    *data >> m_targetMask;

    if(m_targetMask == TARGET_FLAG_SELF)
    {
        m_destX = caster->GetPositionX();
        m_destY = caster->GetPositionY();
        m_destZ = caster->GetPositionZ();
        m_unitTarget = caster;
        m_unitTargetGUID = caster->GetGUID();
        return true;
    }

    // TARGET_FLAG_UNK2 is used for non-combat pets, maybe other?
    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_UNK2 ))
        if(!data->readPackGUID(m_unitTargetGUID))
            return false;

    if( m_targetMask & ( TARGET_FLAG_OBJECT ))
        if(!data->readPackGUID(m_GOTargetGUID))
            return false;

    if(( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM )) && caster->GetTypeId() == TYPEID_PLAYER)
        if(!data->readPackGUID(m_itemTargetGUID))
            return false;

    if( m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
        if(!data->readPackGUID(m_CorpseTargetGUID))
            return false;

    if( m_targetMask & TARGET_FLAG_SOURCE_LOCATION )
    {
        if(data->rpos()+4+4+4 > data->size())
            return false;

        *data >> m_srcX >> m_srcY >> m_srcZ;
        if(!MaNGOS::IsValidMapCoord(m_srcX, m_srcY, m_srcZ))
            return false;
    }

    if( m_targetMask & TARGET_FLAG_DEST_LOCATION )
    {
        if(data->rpos()+1+4+4+4 > data->size())
            return false;

        if(!data->readPackGUID(m_unitTargetGUID))
            return false;

        *data >> m_destX >> m_destY >> m_destZ;
        if(!MaNGOS::IsValidMapCoord(m_destX, m_destY, m_destZ))
            return false;
    }

    if( m_targetMask & TARGET_FLAG_STRING )
    {
        if(data->rpos()+1 > data->size())
            return false;

        *data >> m_strTarget;
    }

    // find real units/GOs
    Update(caster);
    return true;
}

void SpellCastTargets::write ( WorldPacket * data )
{
    *data << uint32(m_targetMask);

    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_PVP_CORPSE | TARGET_FLAG_OBJECT | TARGET_FLAG_CORPSE | TARGET_FLAG_UNK2 ) )
    {
        if(m_targetMask & TARGET_FLAG_UNIT)
        {
            if(m_unitTarget)
                data->append(m_unitTarget->GetPackGUID());
            else
                *data << uint8(0);
        }
        else if( m_targetMask & TARGET_FLAG_OBJECT )
        {
            if(m_GOTarget)
                data->append(m_GOTarget->GetPackGUID());
            else
                *data << uint8(0);
        }
        else if( m_targetMask & ( TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
            data->appendPackGUID(m_CorpseTargetGUID);
        else
            *data << uint8(0);
    }

    if( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM ) )
    {
        if(m_itemTarget)
            data->append(m_itemTarget->GetPackGUID());
        else
            *data << uint8(0);
    }

    if( m_targetMask & TARGET_FLAG_SOURCE_LOCATION )
        *data << m_srcX << m_srcY << m_srcZ;

    if( m_targetMask & TARGET_FLAG_DEST_LOCATION )
    {
        if(m_unitTarget)
            data->append(m_unitTarget->GetPackGUID());
        else
            *data << uint8(0);

        *data << m_destX << m_destY << m_destZ;
    }

    if( m_targetMask & TARGET_FLAG_STRING )
        *data << m_strTarget;
}

Spell::Spell( Unit* Caster, SpellEntry const *info, bool triggered, uint64 originalCasterGUID, Spell** triggeringContainer )
{
    ASSERT( Caster != NULL && info != NULL );
    ASSERT( info == sSpellStore.LookupEntry( info->Id ) && "`info` must be pointer to sSpellStore element");

    m_spellInfo = info;
    m_caster = Caster;
    m_selfContainer = NULL;
    m_triggeringContainer = triggeringContainer;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;

    m_applyMultiplierMask = 0;

    // Get data for type of attack
    switch (m_spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            if (m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_REQ_OFFHAND)
                m_attackType = OFF_ATTACK;
            else
                m_attackType = BASE_ATTACK;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            m_attackType = RANGED_ATTACK;
            break;
        default:
                                                            // Wands
            if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
                m_attackType = RANGED_ATTACK;
            else
                m_attackType = BASE_ATTACK;
            break;
    }

    m_spellSchoolMask = GetSpellSchoolMask(info);           // Can be override for some spell (wand shoot for example)

    if(m_attackType == RANGED_ATTACK)
    {
        // wand case
        if((m_caster->getClassMask() & CLASSMASK_WAND_USERS) != 0 && m_caster->GetTypeId()==TYPEID_PLAYER)
        {
            if(Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK))
                m_spellSchoolMask = SpellSchoolMask(1 << pItem->GetProto()->Damage[0].DamageType);
        }
    }
    // Set health leech amount to zero
    m_healthLeech = 0;

    if(originalCasterGUID)
        m_originalCasterGUID = originalCasterGUID;
    else
        m_originalCasterGUID = m_caster->GetGUID();

    if(m_originalCasterGUID==m_caster->GetGUID())
        m_originalCaster = m_caster;
    else
    {
        m_originalCaster = ObjectAccessor::GetUnit(*m_caster,m_originalCasterGUID);
        if(m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }

    for(int i=0; i <3; ++i)
        m_currentBasePoints[i] = m_spellInfo->EffectBasePoints[i];

    m_spellState = SPELL_STATE_NULL;

    m_castPositionX = m_castPositionY = m_castPositionZ = 0;
    m_TriggerSpells.clear();
    m_IsTriggeredSpell = triggered;
    //m_AreaAura = false;
    m_CastItem = NULL;

    unitTarget = NULL;
    itemTarget = NULL;
    gameObjTarget = NULL;
    focusObject = NULL;
    m_cast_count = 0;
    m_glyphIndex = 0;
    m_preCastSpell = 0;
    m_triggeredByAuraSpell  = NULL;

    //Auto Shot & Shoot (wand)
    m_autoRepeat = IsAutoRepeatRangedSpell(m_spellInfo);

    m_runesState = 0;
    m_powerCost = 0;                                        // setup to correct value in Spell::prepare, don't must be used before.
    m_casttime = 0;                                         // setup to correct value in Spell::prepare, don't must be used before.
    m_timer = 0;                                            // will set to castime in prepare

    m_needAliveTargetMask = 0;

    // determine reflection
    m_canReflect = false;

    if(m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !(m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_CANT_REFLECTED))
    {
        for(int j=0;j<3;j++)
        {
            if (m_spellInfo->Effect[j]==0)
                continue;

            if(!IsPositiveTarget(m_spellInfo->EffectImplicitTargetA[j],m_spellInfo->EffectImplicitTargetB[j]))
                m_canReflect = true;
            else
                m_canReflect = (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NEGATIVE) ? true : false;

            if(m_canReflect)
                continue;
            else
                break;
        }
    }

    CleanupTargetList();
}

Spell::~Spell()
{
}

template<typename T>
WorldObject* Spell::FindCorpseUsing()
{
    // non-standard target selection
    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange);

    CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    WorldObject* result = NULL;

    T u_check(m_caster, max_range);
    MaNGOS::WorldObjectSearcher<T> searcher(m_caster, result, u_check);

    TypeContainerVisitor<MaNGOS::WorldObjectSearcher<T>, GridTypeMapContainer > grid_searcher(searcher);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, grid_searcher, *m_caster->GetMap());

    if (!result)
    {
        TypeContainerVisitor<MaNGOS::WorldObjectSearcher<T>, WorldTypeMapContainer > world_searcher(searcher);
        cell_lock->Visit(cell_lock, world_searcher, *m_caster->GetMap());
    }

    return result;
}

void Spell::FillTargetMap()
{
    // TODO: ADD the correct target FILLS!!!!!!

    for(uint32 i=0;i<3;++i)
    {
        // not call for empty effect.
        // Also some spells use not used effect targets for store targets for dummy effect in triggered spells
        if(m_spellInfo->Effect[i]==0)
            continue;

        // targets for TARGET_SCRIPT_COORDINATES (A) and TARGET_SCRIPT  filled in Spell::CheckCast call
        if( m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT_COORDINATES ||
            m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT ||
            m_spellInfo->EffectImplicitTargetB[i] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[i] != TARGET_SELF )
            continue;

        // TODO: find a way so this is not needed?
        // for area auras always add caster as target (needed for totems for example)
        if(IsAreaAuraEffect(m_spellInfo->Effect[i]))
            AddUnitTarget(m_caster, i);

        std::list<Unit*> tmpUnitMap;

        // TargetA/TargetB dependent from each other, we not switch to full support this dependences
        // but need it support in some know cases
        switch(m_spellInfo->EffectImplicitTargetA[i])
        {
            case TARGET_SELF:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        break;
                    case TARGET_AREAEFFECT_INSTANT:         // use B case that not dependent from from A in fact
                        if((m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)==0)
                            m_targets.setDestination(m_caster->GetPositionX(),m_caster->GetPositionY(),m_caster->GetPositionZ());
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                    case TARGET_BEHIND_VICTIM:              // use B case that not dependent from from A in fact
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                }
                break;
            case TARGET_CASTER_COORDINATES:
                // Note: this hack with search required until GO casting not implemented
                // environment damage spells already have around enemies targeting but this not help in case not existed GO casting support
                // currently each enemy selected explicitly and self cast damage
                if(m_spellInfo->EffectImplicitTargetB[i]==TARGET_ALL_ENEMY_IN_AREA && m_spellInfo->Effect[i]==SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
                {
                    if(m_targets.getUnitTarget())
                        tmpUnitMap.push_back(m_targets.getUnitTarget());
                }
                else
                {
                    SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                    SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                }
                break;
            case TARGET_TABLE_X_Y_Z_COORDINATES:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);

                        // need some target for proccesing
                        if(m_targets.getUnitTarget())
                            tmpUnitMap.push_back(m_targets.getUnitTarget());
                        else
                            tmpUnitMap.push_back(m_caster); 
                        break;
                    case TARGET_AREAEFFECT_INSTANT:         // All 17/7 pairs used for dest teleportation, A processed in effect code
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                    break;
                }
                break;
            default:
                switch(m_spellInfo->EffectImplicitTargetB[i])
                {
                    case 0:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        break;
                    case TARGET_SCRIPT_COORDINATES:         // B case filled in CheckCast but we need fill unit list base at A case
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        break;
                    default:
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                        SetTargetMap(i,m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                        break;
                }
                break;
        }

        if( (m_spellInfo->EffectImplicitTargetA[i]==0 || m_spellInfo->EffectImplicitTargetA[i]==TARGET_EFFECT_SELECT) &&
            (m_spellInfo->EffectImplicitTargetB[i]==0 || m_spellInfo->EffectImplicitTargetB[i]==TARGET_EFFECT_SELECT) )
        {
            // add here custom effects that need default target.
            // FOR EVERY TARGET TYPE THERE IS A DIFFERENT FILL!!
            switch(m_spellInfo->Effect[i])
            {
                case SPELL_EFFECT_DUMMY:
                {
                    switch(m_spellInfo->Id)
                    {
                        case 20577:                         // Cannibalize
                        {
                            WorldObject* result = FindCorpseUsing<MaNGOS::CannibalizeObjectCheck> ();

                            if(result)
                            {
                                switch(result->GetTypeId())
                                {
                                    case TYPEID_UNIT:
                                    case TYPEID_PLAYER:
                                        tmpUnitMap.push_back((Unit*)result);
                                        break;
                                    case TYPEID_CORPSE:
                                        m_targets.setCorpseTarget((Corpse*)result);
                                        if(Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGUID()))
                                            tmpUnitMap.push_back(owner);
                                        break;
                                }
                            }
                            else
                            {
                                // clear cooldown at fail
                                if(m_caster->GetTypeId()==TYPEID_PLAYER)
                                    ((Player*)m_caster)->RemoveSpellCooldown(m_spellInfo->Id,true);
                                SendCastResult(SPELL_FAILED_NO_EDIBLE_CORPSES);
                                finish(false);
                            }
                            break;
                        }
                        default:
                            if(m_targets.getUnitTarget())
                                tmpUnitMap.push_back(m_targets.getUnitTarget());
                            break;
                    }
                    break;
                }
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_PARRY:
                case SPELL_EFFECT_BLOCK:
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_TRIGGER_SPELL:
                case SPELL_EFFECT_TRIGGER_MISSILE:
                case SPELL_EFFECT_LEARN_SPELL:
                case SPELL_EFFECT_SKILL_STEP:
                case SPELL_EFFECT_PROFICIENCY:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_SELF_RESURRECT:
                case SPELL_EFFECT_REPUTATION:
                case SPELL_EFFECT_SEND_TAXI:
                    if(m_targets.getUnitTarget())
                        tmpUnitMap.push_back(m_targets.getUnitTarget());
                    // Triggered spells have additional spell targets - cast them even if no explicit unit target is given (required for spell 50516 for example)
                    else if(m_spellInfo->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL)
                        tmpUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_SUMMON_PLAYER:
                    if(m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->GetSelection())
                    {
                        Player* target = objmgr.GetPlayer(((Player*)m_caster)->GetSelection());
                        if(target)
                            tmpUnitMap.push_back(target);
                    }
                    break;
                case SPELL_EFFECT_RESURRECT_NEW:
                    if(m_targets.getUnitTarget())
                        tmpUnitMap.push_back(m_targets.getUnitTarget());
                    if(m_targets.getCorpseTargetGUID())
                    {
                        Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster,m_targets.getCorpseTargetGUID());
                        if(corpse)
                        {
                            Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
                            if(owner)
                                tmpUnitMap.push_back(owner);
                        }
                    }
                    break;
                case SPELL_EFFECT_SUMMON:
                    if(m_spellInfo->EffectMiscValueB[i] == SUMMON_TYPE_POSESSED || m_spellInfo->EffectMiscValueB[i] == SUMMON_TYPE_POSESSED2)
                    {
                        if(m_targets.getUnitTarget())
                            tmpUnitMap.push_back(m_targets.getUnitTarget());
                    }
                    else
                        tmpUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_SUMMON_CHANGE_ITEM:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_ADD_FARSIGHT:
                case SPELL_EFFECT_APPLY_GLYPH:
                case SPELL_EFFECT_STUCK:
                case SPELL_EFFECT_FEED_PET:
                case SPELL_EFFECT_DESTROY_ALL_TOTEMS:
                case SPELL_EFFECT_SKILL:
                    tmpUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_LEARN_PET_SPELL:
                    if(Pet* pet = m_caster->GetPet())
                        tmpUnitMap.push_back(pet);
                    break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
                case SPELL_EFFECT_DISENCHANT:
                case SPELL_EFFECT_PROSPECTING:
                case SPELL_EFFECT_MILLING:
                    if(m_targets.getItemTarget())
                        AddItemTarget(m_targets.getItemTarget(), i);
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                    switch(m_spellInfo->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_ADD_FLAT_MODIFIER:  // some spell mods auras have 0 target modes instead expected TARGET_SELF(1) (and present for other ranks for same spell for example)
                        case SPELL_AURA_ADD_PCT_MODIFIER:
                            tmpUnitMap.push_back(m_caster);
                            break;
                        default:                            // apply to target in other case
                            if(m_targets.getUnitTarget())
                                tmpUnitMap.push_back(m_targets.getUnitTarget());
                            break;
                    }
                    break;
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                                                            // AreaAura
                    if(m_spellInfo->Attributes == 0x9050000 || m_spellInfo->Attributes == 0x10000)
                        SetTargetMap(i,TARGET_AREAEFFECT_PARTY,tmpUnitMap);
                    break;
                case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
                    if(m_targets.getUnitTarget())
                    {
                        tmpUnitMap.push_back(m_targets.getUnitTarget());
                    }
                    else if (m_targets.getCorpseTargetGUID())
                    {
                        Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster,m_targets.getCorpseTargetGUID());
                        if(corpse)
                        {
                            Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
                            if(owner)
                                tmpUnitMap.push_back(owner);
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        if(m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            Player *me = (Player*)m_caster;
            for (std::list<Unit*>::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                Unit *owner = (*itr)->GetOwner();
                Unit *u = owner ? owner : (*itr);
                if(u!=m_caster && u->IsPvP() && (!me->duel || me->duel->opponent != u))
                {
                    me->UpdatePvP(true);
                    me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
                    break;
                }
            }
        }

        for (std::list<Unit*>::iterator itr = tmpUnitMap.begin() ; itr != tmpUnitMap.end();)
        {
            if (!CheckTarget (*itr, i))
            {
                itr = tmpUnitMap.erase(itr);
                continue;
            }
            else
                ++itr;
        }

        for(std::list<Unit*>::const_iterator iunit= tmpUnitMap.begin();iunit != tmpUnitMap.end();++iunit)
            AddUnitTarget((*iunit), i);
    }
}

void Spell::prepareDataForTriggerSystem()
{
    //==========================================================================================
    // Now fill data for trigger system, need know:
    // an spell trigger another or not ( m_canTrigger )
    // Create base triggers flags for Attacker and Victim ( m_procAttacker and  m_procVictim)
    //==========================================================================================
    // Fill flag can spell trigger or not
    // TODO: possible exist spell attribute for this
    m_canTrigger = false;

    if (m_CastItem)
        m_canTrigger = false;         // Do not trigger from item cast spell
    else if (!m_IsTriggeredSpell)
        m_canTrigger = true;          // Normal cast - can trigger
    else if (!m_triggeredByAuraSpell)
        m_canTrigger = true;          // Triggered from SPELL_EFFECT_TRIGGER_SPELL - can trigger

    if (!m_canTrigger)                // Exceptions (some periodic triggers)
    {
        switch (m_spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_MAGE:    // Arcane Missles / Blizzard triggers need do it
                if (m_spellInfo->SpellFamilyFlags & 0x0000000000200080LL) m_canTrigger = true;
            break;
            case SPELLFAMILY_WARLOCK: // For Hellfire Effect / Rain of Fire / Seed of Corruption triggers need do it
                if (m_spellInfo->SpellFamilyFlags & 0x0000800000000060LL) m_canTrigger = true;
            break;
            case SPELLFAMILY_PRIEST:  // For Penance heal/damage triggers need do it
                if (m_spellInfo->SpellFamilyFlags & 0x0001800000000000LL) m_canTrigger = true;
            break;
            case SPELLFAMILY_ROGUE:   // For poisons need do it
                if (m_spellInfo->SpellFamilyFlags & 0x000000101001E000LL) m_canTrigger = true;
            break;
            case SPELLFAMILY_HUNTER:  // Hunter Rapid Killing/Explosive Trap Effect/Immolation Trap Effect/Frost Trap Aura/Snake Trap Effect/Explosive Shot
                if (m_spellInfo->SpellFamilyFlags & 0x0100200000000214LL ||
                    m_spellInfo->SpellFamilyFlags2 & 0x200) m_canTrigger = true;
            break;
            case SPELLFAMILY_PALADIN: // For Judgements (all) / Holy Shock triggers need do it
                if (m_spellInfo->SpellFamilyFlags & 0x0001000900B80400LL) m_canTrigger = true;
            break;
        }
    }

    // Get data for type of attack and fill base info for trigger
    switch (m_spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            m_procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_SPELL_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_MELEE_SPELL_HIT;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            // Auto attack
            if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else // Ranged spell attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;
            }
            break;
        default:
            if (IsPositiveSpell(m_spellInfo->Id))                                 // Check for positive spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL;
                m_procVictim   = PROC_FLAG_TAKEN_POSITIVE_SPELL;
            }
            else if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG) // Wands auto attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else                                           // Negative spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;
            }
            break;
    }
    // Hunter traps spells (for Entrapment trigger)
    // Gives your Immolation Trap, Frost Trap, Explosive Trap, and Snake Trap ....
    if (m_spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER && m_spellInfo->SpellFamilyFlags & 0x000020000000001CLL)
        m_procAttacker |= PROC_FLAG_ON_TRAP_ACTIVATION;
}

void Spell::CleanupTargetList()
{
    m_UniqueTargetInfo.clear();
    m_UniqueGOTargetInfo.clear();
    m_UniqueItemInfo.clear();
    m_delayMoment = 0;
}

void Spell::AddUnitTarget(Unit* pVictim, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex]==0 )
        return;

    // Check for effect immune skip if immuned
    bool immuned = pVictim->IsImmunedToSpellEffect(m_spellInfo, effIndex);

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            if (!immuned)
                ihit->effectMask |= 1<<effIndex;            // Add only effect mask if not immuned
            return;
        }
    }

    // This is new target calculate data for him

    // Get spell hit result on target
    TargetInfo target;
    target.targetGUID = targetGUID;                         // Store target GUID
    target.effectMask = immuned ? 0 : 1<<effIndex;          // Store index of effect if not immuned
    target.processed  = false;                              // Effects not apply on target

    // Calculate hit result
    target.missCondition = m_caster->SpellHitResult(pVictim, m_spellInfo, m_canReflect);

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f)
    {
        // calculate spell incoming interval
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f) dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);

        // Calculate minimum incoming time
        if (m_delayMoment==0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = 0LL;

    // If target reflect spell back to caster
    if (target.missCondition==SPELL_MISS_REFLECT)
    {
        // Calculate reflected spell result on caster
        target.reflectResult =  m_caster->SpellHitResult(m_caster, m_spellInfo, m_canReflect);

        if (target.reflectResult == SPELL_MISS_REFLECT)     // Impossible reflect again, so simply deflect spell
            target.reflectResult = SPELL_MISS_PARRY;

        // Increase time interval for reflected spells by 1.5
        target.timeDelay+=target.timeDelay>>1;
    }
    else
        target.reflectResult = SPELL_MISS_NONE;

    // Add target to list
    m_UniqueTargetInfo.push_back(target);
}

void Spell::AddUnitTarget(uint64 unitGUID, uint32 effIndex)
{
    Unit* unit = m_caster->GetGUID()==unitGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, unitGUID);
    if (unit)
        AddUnitTarget(unit, effIndex);
}

void Spell::AddGOTarget(GameObject* pVictim, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex]==0 )
        return;

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for(std::list<GOTargetInfo>::iterator ihit= m_UniqueGOTargetInfo.begin();ihit != m_UniqueGOTargetInfo.end();++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= 1<<effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    GOTargetInfo target;
    target.targetGUID = targetGUID;
    target.effectMask = 1<<effIndex;
    target.processed  = false;                              // Effects not apply on target

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f)
    {
        // calculate spell incoming interval
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f) dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);
        if (m_delayMoment==0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = 0LL;

    // Add target to list
    m_UniqueGOTargetInfo.push_back(target);
}

void Spell::AddGOTarget(uint64 goGUID, uint32 effIndex)
{
    GameObject* go = m_caster->GetMap()->GetGameObject(goGUID);
    if (go)
        AddGOTarget(go, effIndex);
}

void Spell::AddItemTarget(Item* pitem, uint32 effIndex)
{
    if( m_spellInfo->Effect[effIndex]==0 )
        return;

    // Lookup target in already in list
    for(std::list<ItemTargetInfo>::iterator ihit= m_UniqueItemInfo.begin();ihit != m_UniqueItemInfo.end();++ihit)
    {
        if (pitem == ihit->item)                            // Found in list
        {
            ihit->effectMask |= 1<<effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target add data

    ItemTargetInfo target;
    target.item       = pitem;
    target.effectMask = 1<<effIndex;
    m_UniqueItemInfo.push_back(target);
}

void Spell::DoAllEffectOnTarget(TargetInfo *target)
{
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    // Get mask of effects for target
    uint32 mask = target->effectMask;

    Unit* unit = m_caster->GetGUID()==target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster,target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *caster = m_originalCaster ? m_originalCaster : m_caster;

    // Skip if m_originalCaster not available
    if (!caster)
        return;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    m_damage = 0;
    m_healing = 0;

    // Fill base trigger info
    uint32 procAttacker = m_procAttacker;
    uint32 procVictim   = m_procVictim;
    uint32 procEx       = PROC_EX_NONE;

    if (missInfo==SPELL_MISS_NONE)                          // In case spell hit target, do all effect on that target
        DoSpellHitOnUnit(unit, mask);
    else if (missInfo == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
    {
        if (target->reflectResult == SPELL_MISS_NONE)       // If reflected spell hit caster -> do all effect on him
            DoSpellHitOnUnit(m_caster, mask);
    }

    // All calculated do it!
    // Do healing and triggers
    if (m_healing)
    {
        bool crit = caster->isSpellCrit(NULL, m_spellInfo, m_spellSchoolMask);
        uint32 addhealth = m_healing;
        if (crit)
        {
            procEx |= PROC_EX_CRITICAL_HIT;
            addhealth = caster->SpellCriticalHealingBonus(m_spellInfo, addhealth, NULL);
        }
        else
            procEx |= PROC_EX_NORMAL_HIT;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, addhealth, m_attackType, m_spellInfo);

        int32 gain = caster->DealHeal(unitTarget, addhealth, m_spellInfo, crit);
        unitTarget->getHostilRefManager().threatAssist(caster, float(gain) * 0.5f, m_spellInfo);
    }
    // Do damage and triggers
    else if (m_damage)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, m_spellSchoolMask);

        // Add bonuses and fill damageInfo struct
        caster->CalculateSpellDamage(&damageInfo, m_damage, m_spellInfo);
        caster->DealDamageMods(damageInfo.target,damageInfo.damage,&damageInfo.absorb);

        // Send log damage message to client
        caster->SendSpellNonMeleeDamageLog(&damageInfo);

        procEx = createProcExtendMask(&damageInfo, missInfo);
        procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, damageInfo.damage, m_attackType, m_spellInfo);

        caster->DealSpellDamage(&damageInfo, true);

        // Judgement of Blood
        if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN && m_spellInfo->SpellFamilyFlags & 0x0000000800000000LL && m_spellInfo->SpellIconID==153)
        {
            int32 damagePoint  = damageInfo.damage * 33 / 100;
            m_caster->CastCustomSpell(m_caster, 32220, &damagePoint, NULL, NULL, true);
        }
    }
    // Passive spell hits/misses or active spells only misses (only triggers)
    else
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, m_spellSchoolMask);
        procEx = createProcExtendMask(&damageInfo, missInfo);
        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unit, procAttacker, procVictim, procEx, 0, m_attackType, m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted upon a creature
    if(unit->GetTypeId()==TYPEID_UNIT)
    {
        // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
        // ignore pets or autorepeat/melee casts for speed (not exist quest for spells (hm... )
        if( !((Creature*)unit)->isPet() && !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
        {
            if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                p->CastedCreatureOrGO(unit->GetEntry(),unit->GetGUID(),m_spellInfo->Id);
        }

        if(((Creature*)unit)->AI())
            ((Creature*)unit)->AI()->SpellHit(m_caster ,m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted by a creature
    if(m_caster->GetTypeId()==TYPEID_UNIT && ((Creature*)m_caster)->AI())
        ((Creature*)m_caster)->AI()->SpellHitTarget(unit,m_spellInfo);
}

void Spell::DoSpellHitOnUnit(Unit *unit, const uint32 effectMask)
{
    if(!unit || !effectMask)
        return;

    // Recheck immune (only for delayed spells)
    if( m_spellInfo->speed && (
        unit->IsImmunedToDamage(GetSpellSchoolMask(m_spellInfo)) ||
        unit->IsImmunedToSpell(m_spellInfo)))
    {
        m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_IMMUNE);
        return;
    }

    if (unit->GetTypeId() == TYPEID_PLAYER)
    {
        ((Player*)unit)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, m_spellInfo->Id);
        ((Player*)unit)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET2, m_spellInfo->Id);
    }

    if(m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        ((Player*)m_caster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL2, m_spellInfo->Id, 0, unit);
    }

    if( m_caster != unit )
    {
        // Recheck  UNIT_FLAG_NON_ATTACKABLE for delayed spells
        if (m_spellInfo->speed > 0.0f &&
            unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) &&
            unit->GetCharmerOrOwnerGUID() != m_caster->GetGUID())
        {
            m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
            return;
        }

        if( !m_caster->IsFriendlyTo(unit) )
        {
            // for delayed spells ignore not visible explicit target
            if(m_spellInfo->speed > 0.0f && unit==m_targets.getUnitTarget() && !unit->isVisibleForOrDetect(m_caster,false))
            {
                m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                return;
            }

            unit->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

            if( !(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NO_INITIAL_AGGRO) )
            {
                if(!unit->IsStandState() && !unit->hasUnitState(UNIT_STAT_STUNNED))
                    unit->SetStandState(UNIT_STAND_STATE_STAND);

                if(!unit->isInCombat() && unit->GetTypeId() != TYPEID_PLAYER && ((Creature*)unit)->AI())
                    ((Creature*)unit)->AI()->AttackedBy(m_caster);

                unit->AddThreat(m_caster, 0.0f);
                unit->SetInCombatWith(m_caster);
                m_caster->SetInCombatWith(unit);

                if(Player *attackedPlayer = unit->GetCharmerOrOwnerPlayerOrPlayerItself())
                {
                    m_caster->SetContestedPvP(attackedPlayer);
                }
            }
        }
        else
        {
            // for delayed spells ignore negative spells (after duel end) for friendly targets
            if(m_spellInfo->speed > 0.0f && !IsPositiveSpell(m_spellInfo->Id))
            {
                m_caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                return;
            }

            // assisting case, healing and resurrection
            if(unit->hasUnitState(UNIT_STAT_ATTACK_PLAYER))
                m_caster->SetContestedPvP();
            if( unit->isInCombat() && !(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NO_INITIAL_AGGRO) )
            {
                m_caster->SetInCombatState(unit->GetCombatTimer() > 0);
                unit->getHostilRefManager().threatAssist(m_caster, 0.0f);
            }
        }
    }

    // Get Data Needed for Diminishing Returns, some effects may have multiple auras, so this must be done on spell hit, not aura add
    m_diminishGroup = GetDiminishingReturnsGroupForSpell(m_spellInfo,m_triggeredByAuraSpell);
    m_diminishLevel = unit->GetDiminishing(m_diminishGroup);
    // Increase Diminishing on unit, current informations for actually casts will use values above
    if((GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_PLAYER && unit->GetTypeId() == TYPEID_PLAYER) || GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_ALL)
        unit->IncrDiminishing(m_diminishGroup);

    // Apply additional spell effects to target
    if (m_preCastSpell)
        m_caster->CastSpell(unit,m_preCastSpell, true, m_CastItem);

    for(uint32 effectNumber=0;effectNumber<3;effectNumber++)
    {
        if (effectMask & (1<<effectNumber))
        {
            HandleEffects(unit,NULL,NULL,effectNumber,m_damageMultipliers[effectNumber]);
            if ( m_applyMultiplierMask & (1 << effectNumber) )
            {
                // Get multiplier
                float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
                // Apply multiplier mods
                if(Player* modOwner = m_originalCaster->GetSpellModOwner())
                    modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier,this);
                m_damageMultipliers[effectNumber] *= multiplier;
            }
        }
    }
}

void Spell::DoAllEffectOnTarget(GOTargetInfo *target)
{
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    uint32 effectMask = target->effectMask;
    if(!effectMask)
        return;

    GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
    if(!go)
        return;

    for(uint32 effectNumber=0;effectNumber<3;effectNumber++)
        if (effectMask & (1<<effectNumber))
            HandleEffects(NULL,NULL,go,effectNumber);

    // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
    // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
    if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
    {
        if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
            p->CastedCreatureOrGO(go->GetEntry(),go->GetGUID(),m_spellInfo->Id);
    }
}

void Spell::DoAllEffectOnTarget(ItemTargetInfo *target)
{
    uint32 effectMask = target->effectMask;
    if(!target->item || !effectMask)
        return;

    for(uint32 effectNumber=0;effectNumber<3;effectNumber++)
        if (effectMask & (1<<effectNumber))
            HandleEffects(NULL, target->item, NULL, effectNumber);
}

bool Spell::IsAliveUnitPresentInTargetList()
{
    // Not need check return true
    if (m_needAliveTargetMask == 0)
        return true;

    uint8 needAliveTargetMask = m_needAliveTargetMask;

    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if( ihit->missCondition == SPELL_MISS_NONE && (needAliveTargetMask & ihit->effectMask) )
        {
            Unit *unit = m_caster->GetGUID()==ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);

            if (unit && unit->isAlive())
                needAliveTargetMask &= ~ihit->effectMask;   // remove from need alive mask effect that have alive target
        }
    }

    // is all effects from m_needAliveTargetMask have alive targets
    return needAliveTargetMask==0;
}

// Helper for Chain Healing
// Spell target first
// Raidmates then descending by injury suffered (MaxHealth - Health)
// Other players/mobs then descending by injury suffered (MaxHealth - Health)
struct ChainHealingOrder : public std::binary_function<const Unit*, const Unit*, bool>
{
    const Unit* MainTarget;
    ChainHealingOrder(Unit const* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(Unit const* _Left, Unit const* _Right) const
    {
        return (ChainHealingHash(_Left) < ChainHealingHash(_Right));
    }
    int32 ChainHealingHash(Unit const* Target) const
    {
        if (Target == MainTarget)
            return 0;
        else if (Target->GetTypeId() == TYPEID_PLAYER && MainTarget->GetTypeId() == TYPEID_PLAYER &&
            ((Player const*)Target)->IsInSameRaidWith((Player const*)MainTarget))
        {
            if (Target->GetHealth() == Target->GetMaxHealth())
                return 40000;
            else
                return 20000 - Target->GetMaxHealth() + Target->GetHealth();
        }
        else
            return 40000 - Target->GetMaxHealth() + Target->GetHealth();
    }
};

class ChainHealingFullHealth: std::unary_function<const Unit*, bool>
{
    public:
        const Unit* MainTarget;
        ChainHealingFullHealth(const Unit* Target) : MainTarget(Target) {};

        bool operator()(const Unit* Target)
        {
            return (Target != MainTarget && Target->GetHealth() == Target->GetMaxHealth());
        }
};

// Helper for targets nearest to the spell target
// The spell target is always first unless there is a target at _completely_ the same position (unbelievable case)
struct TargetDistanceOrder : public std::binary_function<const Unit, const Unit, bool>
{
    const Unit* MainTarget;
    TargetDistanceOrder(const Unit* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(const Unit* _Left, const Unit* _Right) const
    {
        return MainTarget->GetDistanceOrder(_Left,_Right);
    }
};

void Spell::SetTargetMap(uint32 i,uint32 cur,UnitList& TagUnitMap)
{
    float radius;
    if (m_spellInfo->EffectRadiusIndex[i])
        radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]));
    else
        radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex));

    if(m_originalCaster)
        if(Player* modOwner = m_originalCaster->GetSpellModOwner())
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RADIUS, radius,this);

    uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[i];
    if(m_originalCaster)
        if(Player* modOwner = m_originalCaster->GetSpellModOwner())
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);

    // Get spell max affected targets
    uint32 unMaxTargets = m_spellInfo->MaxAffectedTargets;

    // custom target amount cases
    switch(m_spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_DRUID:
            // Starfall
            if (m_spellInfo->SpellFamilyFlags2 & 0x00000100LL)
                unMaxTargets = 2;
            break;
        default:
            break;
    }

    Unit::AuraList const& mod = m_caster->GetAurasByType(SPELL_AURA_MOD_MAX_AFFECTED_TARGETS);
    for(Unit::AuraList::const_iterator m = mod.begin(); m != mod.end(); ++m)
    {
        if (!(*m)->isAffectedOnSpell(m_spellInfo))
            continue;
        unMaxTargets+=(*m)->GetModifier()->m_amount;
    }

    switch(cur)
    {
        case TARGET_TOTEM_EARTH:
        case TARGET_TOTEM_WATER:
        case TARGET_TOTEM_AIR:
        case TARGET_TOTEM_FIRE:
        case TARGET_SELF:
        case TARGET_SELF2:
        case TARGET_DYNAMIC_OBJECT:
        case TARGET_AREAEFFECT_CUSTOM:
        case TARGET_AREAEFFECT_CUSTOM_2:
        case TARGET_SUMMON:
        {
            TagUnitMap.push_back(m_caster);
            break;
        }
        case TARGET_RANDOM_ENEMY_CHAIN_IN_AREA:
        {
            m_targets.m_targetMask = 0;
            unMaxTargets = EffectChainTarget;
            float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
            Cell cell(p);
            cell.data.Part.reserved = ALL_DISTRICT;
            cell.SetNoCreate();

            std::list<Unit *> tempUnitMap;

            {
                MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(m_caster, m_caster, max_range);
                MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(m_caster, tempUnitMap, u_check);

                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, WorldTypeMapContainer > world_unit_searcher(searcher);
                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, GridTypeMapContainer >  grid_unit_searcher(searcher);

                CellLock<GridReadGuard> cell_lock(cell, p);
                cell_lock->Visit(cell_lock, world_unit_searcher, *m_caster->GetMap());
                cell_lock->Visit(cell_lock, grid_unit_searcher, *m_caster->GetMap());
            }

            if(tempUnitMap.empty())
                break;

            tempUnitMap.sort(TargetDistanceOrder(m_caster));

            //Now to get us a random target that's in the initial range of the spell
            uint32 t = 0;
            std::list<Unit *>::iterator itr = tempUnitMap.begin();
            while(itr!= tempUnitMap.end() && (*itr)->IsWithinDist(m_caster,radius))
                ++t, ++itr;

            if(!t)
                break;

            itr = tempUnitMap.begin();
            std::advance(itr, rand()%t);
            Unit *pUnitTarget = *itr;
            TagUnitMap.push_back(pUnitTarget);

            tempUnitMap.erase(itr);

            tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

            t = unMaxTargets - 1;
            Unit *prev = pUnitTarget;
            std::list<Unit*>::iterator next = tempUnitMap.begin();

            while(t && next != tempUnitMap.end() )
            {
                if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if(!prev->IsWithinLOSInMap(*next))
                {
                    ++next;
                    continue;
                }

                prev = *next;
                TagUnitMap.push_back(prev);
                tempUnitMap.erase(next);
                tempUnitMap.sort(TargetDistanceOrder(prev));
                next = tempUnitMap.begin();

                --t;
            }
        }break;
        case TARGET_RANDOM_FRIEND_CHAIN_IN_AREA:
        {
            m_targets.m_targetMask = 0;
            unMaxTargets = EffectChainTarget;
            float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;
            CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
            Cell cell(p);
            cell.data.Part.reserved = ALL_DISTRICT;
            cell.SetNoCreate();
            std::list<Unit *> tempUnitMap;
            {
                MaNGOS::AnyFriendlyUnitInObjectRangeCheck u_check(m_caster, m_caster, max_range);
                MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(m_caster, tempUnitMap, u_check);

                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck>, WorldTypeMapContainer > world_unit_searcher(searcher);
                TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck>, GridTypeMapContainer >  grid_unit_searcher(searcher);

                CellLock<GridReadGuard> cell_lock(cell, p);
                cell_lock->Visit(cell_lock, world_unit_searcher, *m_caster->GetMap());
                cell_lock->Visit(cell_lock, grid_unit_searcher, *m_caster->GetMap());
            }

            if(tempUnitMap.empty())
                break;

            tempUnitMap.sort(TargetDistanceOrder(m_caster));

            //Now to get us a random target that's in the initial range of the spell
            uint32 t = 0;
            std::list<Unit *>::iterator itr = tempUnitMap.begin();
            while(itr!= tempUnitMap.end() && (*itr)->IsWithinDist(m_caster,radius))
                ++t, ++itr;

            if(!t)
                break;

            itr = tempUnitMap.begin();
            std::advance(itr, rand()%t);
            Unit *pUnitTarget = *itr;
            TagUnitMap.push_back(pUnitTarget);

            tempUnitMap.erase(itr);

            tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

            t = unMaxTargets - 1;
            Unit *prev = pUnitTarget;
            std::list<Unit*>::iterator next = tempUnitMap.begin();

            while(t && next != tempUnitMap.end() )
            {
                if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if(!prev->IsWithinLOSInMap(*next))
                {
                    ++next;
                    continue;
                }
                prev = *next;
                TagUnitMap.push_back(prev);
                tempUnitMap.erase(next);
                tempUnitMap.sort(TargetDistanceOrder(prev));
                next = tempUnitMap.begin();
                --t;
            }
        }break;
        case TARGET_PET:
        {
            Pet* tmpUnit = m_caster->GetPet();
            if (!tmpUnit) break;
            TagUnitMap.push_back(tmpUnit);
            break;
        }
        case TARGET_CHAIN_DAMAGE:
        {
            if (EffectChainTarget <= 1)
            {
                if(Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), m_spellInfo))
                {
                    m_targets.setUnitTarget(pUnitTarget);
                    TagUnitMap.push_back(pUnitTarget);
                }
            }
            else
            {
                Unit* pUnitTarget = m_targets.getUnitTarget();
                if(!pUnitTarget)
                    break;

                unMaxTargets = EffectChainTarget;

                float max_range;
                if(m_spellInfo->DmgClass==SPELL_DAMAGE_CLASS_MELEE)
                    max_range = radius;                     //
                else
                    //FIXME: This very like horrible hack and wrong for most spells
                    max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

                CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                Cell cell(p);
                cell.data.Part.reserved = ALL_DISTRICT;
                cell.SetNoCreate();

                Unit* originalCaster = GetOriginalCaster();
                if(originalCaster)
                {
                    std::list<Unit *> tempUnitMap;

                    {
                        MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(pUnitTarget, originalCaster, max_range);
                        MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(m_caster, tempUnitMap, u_check);

                        TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, WorldTypeMapContainer > world_unit_searcher(searcher);
                        TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck>, GridTypeMapContainer >  grid_unit_searcher(searcher);

                        CellLock<GridReadGuard> cell_lock(cell, p);
                        cell_lock->Visit(cell_lock, world_unit_searcher, *m_caster->GetMap());
                        cell_lock->Visit(cell_lock, grid_unit_searcher, *m_caster->GetMap());
                    }

                    tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

                    if(tempUnitMap.empty())
                        break;

                    if(*tempUnitMap.begin() == pUnitTarget)
                        tempUnitMap.erase(tempUnitMap.begin());

                    TagUnitMap.push_back(pUnitTarget);
                    uint32 t = unMaxTargets - 1;
                    Unit *prev = pUnitTarget;
                    std::list<Unit*>::iterator next = tempUnitMap.begin();

                    while(t && next != tempUnitMap.end() )
                    {
                        if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                            break;

                        if(!prev->IsWithinLOSInMap(*next))
                        {
                            ++next;
                            continue;
                        }

                        prev = *next;
                        TagUnitMap.push_back(prev);
                        tempUnitMap.erase(next);
                        tempUnitMap.sort(TargetDistanceOrder(prev));
                        next = tempUnitMap.begin();

                        --t;
                    }
                }
            }
        }break;
        case TARGET_ALL_ENEMY_IN_AREA:
        {
            FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case TARGET_AREAEFFECT_INSTANT:
        {
            SpellTargets targetB = SPELL_TARGETS_AOE_DAMAGE;
            // Select friendly targets for positive effect
            if (IsPositiveEffect(m_spellInfo->Id, i))
                targetB = SPELL_TARGETS_FRIENDLY;

            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius, PUSH_DEST_CENTER, targetB);

            // exclude caster
            TagUnitMap.remove(m_caster);
        }
        case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
        {
            // targets the ground, not the units in the area
            if (m_spellInfo->Effect[i]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
            {
                FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);

                // exclude caster (this can be important if this not original caster)
                TagUnitMap.remove(m_caster);
            }
        }break;
        case TARGET_DUELVSPLAYER_COORDINATES:
        {
            if(Unit* currentTarget = m_targets.getUnitTarget())
            {
                m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
                TagUnitMap.push_back(currentTarget);
            }
        }break;
        case TARGET_ALL_PARTY_AROUND_CASTER:
        case TARGET_ALL_PARTY_AROUND_CASTER_2:
        case TARGET_ALL_PARTY:
        {
            Player *pTarget = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
            Group *pGroup = pTarget ? pTarget->GetGroup() : NULL;

            if(pGroup)
            {
                uint8 subgroup = pTarget->GetSubGroup();

                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if( Target && Target->GetSubGroup()==subgroup && !m_caster->IsHostileTo(Target) )
                    {
                        if( m_caster->IsWithinDistInMap(Target, radius) )
                            TagUnitMap.push_back(Target);

                        if(Pet* pet = Target->GetPet())
                            if( m_caster->IsWithinDistInMap(pet, radius) )
                                TagUnitMap.push_back(pet);
                    }
                }
            }
            else
            {
                Unit* ownerOrSelf = pTarget ? pTarget : m_caster->GetCharmerOrOwnerOrSelf();
                if(ownerOrSelf==m_caster || m_caster->IsWithinDistInMap(ownerOrSelf, radius))
                    TagUnitMap.push_back(ownerOrSelf);
                if(Pet* pet = ownerOrSelf->GetPet())
                    if( m_caster->IsWithinDistInMap(pet, radius) )
                        TagUnitMap.push_back(pet);
            }
            break;
        }
        case TARGET_ALL_RAID_AROUND_CASTER:
        {
            Player *pTarget = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
            Group *pGroup = pTarget ? pTarget->GetGroup() : NULL;

            if(m_spellInfo->Id==57669)                  //Replenishment (special target selection)
            {
                if(pGroup)
                {
                    typedef std::priority_queue<PrioritizeManaPlayerWraper, std::vector<PrioritizeManaPlayerWraper>, PrioritizeMana> Top10;
                    Top10 manaUsers;

                    for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL && manaUsers.size() < 10; itr = itr->next())
                    {
                        Player* Target = itr->getSource();
                        if (m_caster->GetGUID() != Target->GetGUID() && Target->getPowerType() == POWER_MANA &&
                            !Target->isDead() && m_caster->IsWithinDistInMap(Target, radius))
                        {
                            PrioritizeManaPlayerWraper  WTarget(Target);
                            manaUsers.push(WTarget);
                        }
                    }

                    while(!manaUsers.empty())
                    {
                        TagUnitMap.push_back(manaUsers.top().getPlayer());
                        manaUsers.pop();
                    }
                }
                else
                {
                    Unit* ownerOrSelf = pTarget ? pTarget : m_caster->GetCharmerOrOwnerOrSelf();
                    if ((ownerOrSelf==m_caster || m_caster->IsWithinDistInMap(ownerOrSelf, radius)) &&
                        ownerOrSelf->getPowerType() == POWER_MANA)
                        TagUnitMap.push_back(ownerOrSelf);

                    if(Pet* pet = ownerOrSelf->GetPet())
                        if( m_caster->IsWithinDistInMap(pet, radius) && pet->getPowerType() == POWER_MANA )
                            TagUnitMap.push_back(pet);
                }
            }
            if (m_spellInfo->Id==52759)                     //Ancestral Awakening (special target selection)
            {
                float lowestPerc   = (float)m_caster->GetHealth() / (float)m_caster->GetMaxHealth();
                Unit* lowestTarget = m_caster;

                if (pGroup)
                {
                    Group::MemberSlotList const& members = pGroup->GetMemberSlots();
                    Group::MemberSlotList::const_iterator itr = members.begin();
                    for(; itr != members.end(); ++itr)
                    {
                        if (Unit* member = ObjectAccessor::GetPlayer(*m_caster, (*itr).guid))
                        {
                            if (member == m_caster || member->isDead() || m_caster->IsHostileTo(member) || !m_caster->IsWithinDistInMap(member, radius))
                                continue;

                            float perc = (float)member->GetHealth() / (float)member->GetMaxHealth();
                            if (perc <= lowestPerc)
                            {
                                lowestPerc = perc;
                                lowestTarget = member;
                            }
                        }
                    }
                }
                TagUnitMap.push_back(lowestTarget);
            }
            else
            {
                if(pGroup)
                {
                    for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                    {
                        Player* Target = itr->getSource();

                        // IsHostileTo check duel and controlled by enemy
                        if( Target && !m_caster->IsHostileTo(Target) )
                        {
                            if( m_caster->IsWithinDistInMap(Target, radius) )
                                TagUnitMap.push_back(Target);

                            if(Pet* pet = Target->GetPet())
                                if( m_caster->IsWithinDistInMap(pet, radius) )
                                    TagUnitMap.push_back(pet);
                        }
                    }
                }
                else
                {
                    Unit* ownerOrSelf = pTarget ? pTarget : m_caster->GetCharmerOrOwnerOrSelf();
                    if(ownerOrSelf==m_caster || m_caster->IsWithinDistInMap(ownerOrSelf, radius))
                        TagUnitMap.push_back(ownerOrSelf);
                    if(Pet* pet = ownerOrSelf->GetPet())
                        if( m_caster->IsWithinDistInMap(pet, radius) )
                            TagUnitMap.push_back(pet);
                }
            }
            break;
        }
        case TARGET_SINGLE_FRIEND:
        case TARGET_SINGLE_FRIEND_2:
        {
            if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
        }break;
        case TARGET_NONCOMBAT_PET:
        {
            if(Unit* target = m_targets.getUnitTarget())
                if( target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->isPet() && ((Pet*)target)->getPetType() == MINI_PET)
                    TagUnitMap.push_back(target);
        }break;
        case TARGET_CASTER_COORDINATES:
        {
            // Check original caster is GO - set its coordinates as dst cast
            WorldObject *caster = NULL;
            if (IS_GAMEOBJECT_GUID(m_originalCasterGUID))
                caster = m_caster->GetMap()->GetGameObject(m_originalCasterGUID);
            if (!caster)
                caster = m_caster;
            // Set dest for targets
            m_targets.setDestination(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ());
        }break;
        case TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER:
            FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_SELF_CENTER,SPELL_TARGETS_FRIENDLY);
            break;
        case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
            FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_FRIENDLY);
            break;
        // TARGET_SINGLE_PARTY means that the spells can only be casted on a party member and not on the caster (some seals, fire shield from imp, etc..)
        case TARGET_SINGLE_PARTY:
        {
            Unit *target = m_targets.getUnitTarget();
            // Thoses spells apparently can't be casted on the caster.
            if( target && target != m_caster)
            {
                // Can only be casted on group's members or its pets
                Group  *pGroup = NULL;

                Unit* owner = m_caster->GetCharmerOrOwner();
                Unit *targetOwner = target->GetCharmerOrOwner();
                if(owner)
                {
                    if(owner->GetTypeId() == TYPEID_PLAYER)
                    {
                        if( target == owner )
                        {
                            TagUnitMap.push_back(target);
                            break;
                        }
                        pGroup = ((Player*)owner)->GetGroup();
                    }
                }
                else if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if( targetOwner == m_caster && target->GetTypeId()==TYPEID_UNIT && ((Creature*)target)->isPet())
                    {
                        TagUnitMap.push_back(target);
                        break;
                    }
                    pGroup = ((Player*)m_caster)->GetGroup();
                }

                if(pGroup)
                {
                    // Our target can also be a player's pet who's grouped with us or our pet. But can't be controlled player
                    if(targetOwner)
                    {
                        if( targetOwner->GetTypeId() == TYPEID_PLAYER &&
                            target->GetTypeId()==TYPEID_UNIT && (((Creature*)target)->isPet()) &&
                            target->GetOwnerGUID()==targetOwner->GetGUID() &&
                            pGroup->IsMember(((Player*)targetOwner)->GetGUID()))
                        {
                            TagUnitMap.push_back(target);
                        }
                    }
                    // 1Our target can be a player who is on our group
                    else if (target->GetTypeId() == TYPEID_PLAYER && pGroup->IsMember(((Player*)target)->GetGUID()))
                    {
                        TagUnitMap.push_back(target);
                    }
                }
            }
        }break;
        case TARGET_GAMEOBJECT:
        {
            if(m_targets.getGOTarget())
                AddGOTarget(m_targets.getGOTarget(), i);
        }break;
        case TARGET_IN_FRONT_OF_CASTER:
        {
            bool inFront = m_spellInfo->SpellVisual[0] != 3879;
            FillAreaTargets(TagUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),radius,inFront ? PUSH_IN_FRONT : PUSH_IN_BACK,SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case TARGET_DUELVSPLAYER:
        {
            Unit *target = m_targets.getUnitTarget();
            if(target)
            {
                if(m_caster->IsFriendlyTo(target))
                {
                    TagUnitMap.push_back(target);
                }
                else
                {
                    if(Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), m_spellInfo))
                    {
                        m_targets.setUnitTarget(pUnitTarget);
                        TagUnitMap.push_back(pUnitTarget);
                    }
                }
            }
        }break;
        case TARGET_GAMEOBJECT_ITEM:
        {
            if(m_targets.getGOTargetGUID())
                AddGOTarget(m_targets.getGOTarget(), i);
            else if(m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), i);
            break;
        }
        case TARGET_MASTER:
        {
            if(Unit* owner = m_caster->GetCharmerOrOwner())
                TagUnitMap.push_back(owner);
            break;
        }
        case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
        {
            // targets the ground, not the units in the area
            if (m_spellInfo->Effect[i]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                FillAreaTargets(TagUnitMap,m_targets.m_destX, m_targets.m_destY,radius,PUSH_DEST_CENTER,SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case TARGET_MINION:
        {
            if(m_spellInfo->Effect[i] != SPELL_EFFECT_DUEL)
                TagUnitMap.push_back(m_caster);
        }break;
        case TARGET_SINGLE_ENEMY:
        {
            if(Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), m_spellInfo))
            {
                m_targets.setUnitTarget(pUnitTarget);
                TagUnitMap.push_back(pUnitTarget);
            }
        }break;
        case TARGET_AREAEFFECT_PARTY:
        {
            Unit* owner = m_caster->GetCharmerOrOwner();
            Player *pTarget = NULL;

            if(owner)
            {
                TagUnitMap.push_back(m_caster);
                if(owner->GetTypeId() == TYPEID_PLAYER)
                    pTarget = (Player*)owner;
            }
            else if (m_caster->GetTypeId() == TYPEID_PLAYER)
            {
                if(Unit* target = m_targets.getUnitTarget())
                {
                    if( target->GetTypeId() != TYPEID_PLAYER)
                    {
                        if(((Creature*)target)->isPet())
                        {
                            Unit *targetOwner = target->GetOwner();
                            if(targetOwner->GetTypeId() == TYPEID_PLAYER)
                                pTarget = (Player*)targetOwner;
                        }
                    }
                    else
                        pTarget = (Player*)target;
                }
            }

            Group* pGroup = pTarget ? pTarget->GetGroup() : NULL;

            if(pGroup)
            {
                uint8 subgroup = pTarget->GetSubGroup();

                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if(Target && Target->GetSubGroup()==subgroup && !m_caster->IsHostileTo(Target))
                    {
                        if( pTarget->IsWithinDistInMap(Target, radius) )
                            TagUnitMap.push_back(Target);

                        if(Pet* pet = Target->GetPet())
                            if( pTarget->IsWithinDistInMap(pet, radius) )
                                TagUnitMap.push_back(pet);
                    }
                }
            }
            else if (owner)
            {
                if(m_caster->IsWithinDistInMap(owner, radius))
                    TagUnitMap.push_back(owner);
            }
            else if(pTarget)
            {
                TagUnitMap.push_back(pTarget);

                if(Pet* pet = pTarget->GetPet())
                    if( m_caster->IsWithinDistInMap(pet, radius) )
                        TagUnitMap.push_back(pet);
            }

        }break;
        case TARGET_SCRIPT:
        {
            if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
            if(m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), i);
        }break;
        case TARGET_SELF_FISHING:
        {
            TagUnitMap.push_back(m_caster);
        }break;
        case TARGET_CHAIN_HEAL:
        {
            Unit* pUnitTarget = m_targets.getUnitTarget();
            if(!pUnitTarget)
                break;

            if (EffectChainTarget <= 1)
                TagUnitMap.push_back(pUnitTarget);
            else
            {
                unMaxTargets = EffectChainTarget;
                float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

                UnitList tempUnitMap;

                FillAreaTargets(tempUnitMap,m_caster->GetPositionX(), m_caster->GetPositionY(),max_range,PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);

                if(m_caster != pUnitTarget && std::find(tempUnitMap.begin(),tempUnitMap.end(),m_caster) == tempUnitMap.end() )
                    tempUnitMap.push_front(m_caster);

                tempUnitMap.sort(TargetDistanceOrder(pUnitTarget));

                if(tempUnitMap.empty())
                    break;

                if(*tempUnitMap.begin() == pUnitTarget)
                    tempUnitMap.erase(tempUnitMap.begin());

                TagUnitMap.push_back(pUnitTarget);
                uint32 t = unMaxTargets - 1;
                Unit *prev = pUnitTarget;
                std::list<Unit*>::iterator next = tempUnitMap.begin();

                while(t && next != tempUnitMap.end() )
                {
                    if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                        break;

                    if(!prev->IsWithinLOSInMap(*next))
                    {
                        ++next;
                        continue;
                    }

                    if((*next)->GetHealth() == (*next)->GetMaxHealth())
                    {
                        next = tempUnitMap.erase(next);
                        continue;
                    }

                    prev = *next;
                    TagUnitMap.push_back(prev);
                    tempUnitMap.erase(next);
                    tempUnitMap.sort(TargetDistanceOrder(prev));
                    next = tempUnitMap.begin();

                    --t;
                }
            }
        }break;
        case TARGET_CURRENT_ENEMY_COORDINATES:
        {
            Unit* currentTarget = m_targets.getUnitTarget();
            if(currentTarget)
            {
                TagUnitMap.push_back(currentTarget);
                m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
                if(m_spellInfo->EffectImplicitTargetB[i]==TARGET_ALL_ENEMY_IN_AREA_INSTANT)
                    FillAreaTargets(TagUnitMap, currentTarget->GetPositionX(), currentTarget->GetPositionY(), radius, PUSH_TARGET_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            }
            break;
        }
        case TARGET_AREAEFFECT_PARTY_AND_CLASS:
        {
            Player* targetPlayer = m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER
                ? (Player*)m_targets.getUnitTarget() : NULL;

            Group* pGroup = targetPlayer ? targetPlayer->GetGroup() : NULL;
            if(pGroup)
            {
                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if( Target && targetPlayer->IsWithinDistInMap(Target, radius) &&
                        targetPlayer->getClass() == Target->getClass() &&
                        !m_caster->IsHostileTo(Target) )
                    {
                        TagUnitMap.push_back(Target);
                    }
                }
            }
            else if(m_targets.getUnitTarget())
                TagUnitMap.push_back(m_targets.getUnitTarget());
            break;
        }
        case TARGET_TABLE_X_Y_Z_COORDINATES:
        {
            SpellTargetPosition const* st = spellmgr.GetSpellTargetPosition(m_spellInfo->Id);
            if(st)
            {
                if (st->target_mapId == m_caster->GetMapId())
                    m_targets.setDestination(st->target_X, st->target_Y, st->target_Z);
                else
                    sLog.outError( "SPELL: wrong map (%u instead %u) target coordinates for spell ID %u", st->target_mapId, m_caster->GetMapId(), m_spellInfo->Id );
            }
            else
                sLog.outError( "SPELL: unknown target coordinates for spell ID %u", m_spellInfo->Id );
        }break;
        case TARGET_BEHIND_VICTIM:
        {
            Unit *pTarget = NULL;

            // explicit cast data from client or server-side cast
            // some spell at client send caster
            if(m_targets.getUnitTarget() && m_targets.getUnitTarget()!=m_caster)
                pTarget = m_targets.getUnitTarget();
            else if(m_caster->getVictim())
                pTarget = m_caster->getVictim();
            else if(m_caster->GetTypeId() == TYPEID_PLAYER)
                pTarget = ObjectAccessor::GetUnit(*m_caster, ((Player*)m_caster)->GetSelection());

            if(pTarget)
            {
                float _target_x, _target_y, _target_z;
                pTarget->GetClosePoint(_target_x, _target_y, _target_z, m_caster->GetObjectSize(), CONTACT_DISTANCE, M_PI);
                if(pTarget->IsWithinLOS(_target_x,_target_y,_target_z))
                {
                    TagUnitMap.push_back(m_caster);
                    m_targets.setDestination(_target_x, _target_y, _target_z);
                }
            }
            break;
        }
        case TARGET_DYNAMIC_OBJECT_COORDINATES:
        {
            // if parent spell create dynamic object extract area from it
            if(DynamicObject* dynObj = m_caster->GetDynObject(m_triggeredByAuraSpell ? m_triggeredByAuraSpell->Id : m_spellInfo->Id))
                m_targets.setDestination(dynObj->GetPositionX(), dynObj->GetPositionY(), dynObj->GetPositionZ());
            break;
        }
        case TARGET_DIRECTLY_FORWARD:
        {
            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                SpellRangeEntry const* rEntry = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
                float minRange = GetSpellMinRange(rEntry);
                float maxRange = GetSpellMaxRange(rEntry);
                float dist = minRange+ rand_norm()*(maxRange-minRange);

                float _target_x, _target_y, _target_z;
                m_caster->GetClosePoint(_target_x, _target_y, _target_z, m_caster->GetObjectSize(), dist);
                m_targets.setDestination(_target_x, _target_y, _target_z);
            }

            TagUnitMap.push_back(m_caster);
            break;
        }
        default:
            break;
    }

    if (unMaxTargets && TagUnitMap.size() > unMaxTargets)
    {
        // make sure one unit is always removed per iteration
        uint32 removed_utarget = 0;
        for (UnitList::iterator itr = TagUnitMap.begin(), next; itr != TagUnitMap.end(); itr = next)
        {
            next = itr;
            ++next;
            if (!*itr) continue;
            if ((*itr) == m_targets.getUnitTarget())
            {
                TagUnitMap.erase(itr);
                removed_utarget = 1;
                //        break;
            }
        }
        // remove random units from the map
        while (TagUnitMap.size() > unMaxTargets - removed_utarget)
        {
            uint32 poz = urand(0, TagUnitMap.size()-1);
            for (UnitList::iterator itr = TagUnitMap.begin(); itr != TagUnitMap.end(); ++itr, --poz)
            {
                if (!*itr) continue;

                if (!poz)
                {
                    TagUnitMap.erase(itr);
                    break;
                }
            }
        }
        // the player's target will always be added to the map
        if (removed_utarget && m_targets.getUnitTarget())
            TagUnitMap.push_back(m_targets.getUnitTarget());
    }
}

void Spell::prepare(SpellCastTargets const* targets, Aura* triggeredByAura)
{
    m_targets = *targets;

    m_spellState = SPELL_STATE_PREPARING;

    m_castPositionX = m_caster->GetPositionX();
    m_castPositionY = m_caster->GetPositionY();
    m_castPositionZ = m_caster->GetPositionZ();
    m_castOrientation = m_caster->GetOrientation();

    if(triggeredByAura)
        m_triggeredByAuraSpell  = triggeredByAura->GetSpellProto();

    // create and add update event for this spell
    SpellEvent* Event = new SpellEvent(this);
    m_caster->m_Events.AddEvent(Event, m_caster->m_Events.CalculateTime(1));

    //Prevent casting at cast another spell (ServerSide check)
    if(m_caster->IsNonMeleeSpellCasted(false, true, true) && m_cast_count)
    {
        SendCastResult(SPELL_FAILED_SPELL_IN_PROGRESS);
        finish(false);
        return;
    }

    // Fill cost data
    m_powerCost = CalculatePowerCost();

    SpellCastResult result = CheckCast(true);
    if(result != SPELL_CAST_OK && !IsAutoRepeat())          //always cast autorepeat dummy for triggering
    {
        if(triggeredByAura)
        {
            SendChannelUpdate(0);
            triggeredByAura->SetAuraDuration(0);
        }
        SendCastResult(result);
        finish(false);
        return;
    }

    // Prepare data for triggers
    prepareDataForTriggerSystem();

    // calculate cast time (calculated after first CheckCast check to prevent charge counting for first CheckCast fail)
    m_casttime = GetSpellCastTime(m_spellInfo, this);

    // set timer base at cast time
    ReSetTimer();

    // stealth must be removed at cast starting (at show channel bar)
    // skip triggered spell (item equip spell casting and other not explicit character casts/item uses)
    if ( !m_IsTriggeredSpell && isSpellBreakStealth(m_spellInfo) )
    {
        m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
        m_caster->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
    }

    if(m_IsTriggeredSpell)
        cast(true);
    else
    {
        m_caster->SetCurrentCastedSpell( this );
        m_selfContainer = &(m_caster->m_currentSpells[GetCurrentContainer()]);
        SendSpellStart();
    }
}

void Spell::cancel()
{
    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    m_autoRepeat = false;
    switch (m_spellState)
    {
        case SPELL_STATE_PREPARING:
        case SPELL_STATE_DELAYED:
        {
            SendInterrupted(0);
            SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        case SPELL_STATE_CASTING:
        {
            for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            {
                if( ihit->missCondition == SPELL_MISS_NONE )
                {
                    Unit* unit = m_caster->GetGUID()==(*ihit).targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                    if( unit && unit->isAlive() )
                        unit->RemoveAurasDueToSpell(m_spellInfo->Id);
                }
            }

            m_caster->RemoveAurasDueToSpell(m_spellInfo->Id);
            SendChannelUpdate(0);
            SendInterrupted(0);
            SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        default:
        {
        } break;
    }

    finish(false);
    m_caster->RemoveDynObject(m_spellInfo->Id);
    m_caster->RemoveGameObject(m_spellInfo->Id,true);
}

void Spell::cast(bool skipCheck)
{
    SetExecutedCurrently(true);

    // update pointers base at GUIDs to prevent access to non-existed already object
    UpdatePointers();

    // cancel at lost main target unit
    if(!m_targets.getUnitTarget() && m_targets.getUnitTargetGUID() && m_targets.getUnitTargetGUID() != m_caster->GetGUID())
    {
        cancel();
        SetExecutedCurrently(false);
        return;
    }

    if(m_caster->GetTypeId() != TYPEID_PLAYER && m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
        m_caster->SetInFront(m_targets.getUnitTarget());

    SpellCastResult castResult = CheckPower();
    if(castResult != SPELL_CAST_OK)
    {
        SendCastResult(castResult);
        finish(false);
        SetExecutedCurrently(false);
        return;
    }

    // triggered cast called from Spell::prepare where it was already checked
    if(!skipCheck)
    {
        castResult = CheckCast(false);
        if(castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            SetExecutedCurrently(false);
            return;
        }
    }

    switch(m_spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            if (m_spellInfo->Mechanic == MECHANIC_BANDAGE)             // Bandages
                m_preCastSpell = 11196;                                // Recently Bandaged
            else if(m_spellInfo->SpellIconID == 1662 && m_spellInfo->AttributesEx & 0x20) // Blood Fury (Racial)
                m_preCastSpell = 23230;                                // Blood Fury - Healing Reduction
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            if (m_spellInfo->SpellFamilyFlags&0x0000008000000000LL)    // Ice Block
                m_preCastSpell = 41425;                                // Hypothermia
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            if (m_spellInfo->Mechanic == MECHANIC_SHIELD &&
                m_spellInfo->SpellIconID == 566)                       // Power Word: Shield
                m_preCastSpell = 6788;                                 // Weakened Soul
            if (m_spellInfo->Id == 47585)                              // Dispersion (transform)
                m_preCastSpell = 60069;                                // Dispersion (mana regen)
            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            if (m_spellInfo->SpellFamilyFlags&0x0000000000400080LL)    // Divine Shield, Divine Protection or Hand of Protection
                m_preCastSpell = 25771;                                // Forbearance
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            if (m_spellInfo->Id == 2825)                               // Bloodlust
                m_preCastSpell = 57724;                                // Sated
            else if (m_spellInfo->Id == 32182)                         // Heroism
                m_preCastSpell = 57723;                                // Exhaustion
            break;
        }
        default:
            break;
    }

    // Conflagrate - consumes immolate
    if ((m_spellInfo->TargetAuraState == AURA_STATE_IMMOLATE) && m_targets.getUnitTarget())
    {
        // for caster applied auras only
        Unit::AuraList const &mPeriodic = m_targets.getUnitTarget()->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE);
        for(Unit::AuraList::const_iterator i = mPeriodic.begin(); i != mPeriodic.end(); ++i)
        {
            if( (*i)->GetSpellProto()->SpellFamilyName == SPELLFAMILY_WARLOCK && ((*i)->GetSpellProto()->SpellFamilyFlags & 4) &&
                (*i)->GetCasterGUID()==m_caster->GetGUID() )
            {
                m_targets.getUnitTarget()->RemoveAura((*i)->GetId(), (*i)->GetEffIndex());
                break;
            }
        }
    }

    // traded items have trade slot instead of guid in m_itemTargetGUID
    // set to real guid to be sent later to the client
    m_targets.updateTradeSlotItem();

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (!m_IsTriggeredSpell && m_CastItem)
            ((Player*)m_caster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_ITEM, m_CastItem->GetEntry());

        ((Player*)m_caster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL, m_spellInfo->Id);
    }

    FillTargetMap();

    if(m_spellState == SPELL_STATE_FINISHED)                // stop cast if spell marked as finish somewhere in FillTargetMap
    {
        SetExecutedCurrently(false);
        return;
    }

    // CAST SPELL
    SendSpellCooldown();

    TakePower();
    TakeReagents();                                         // we must remove reagents before HandleEffects to allow place crafted item in same slot

    SendCastResult(castResult);
    SendSpellGo();                                          // we must send smsg_spell_go packet before m_castItem delete in TakeCastItem()...

    // Okay, everything is prepared. Now we need to distinguish between immediate and evented delayed spells
    if (m_spellInfo->speed > 0.0f)
    {

        // Remove used for cast item if need (it can be already NULL after TakeReagents call
        // in case delayed spell remove item at cast delay start
        TakeCastItem();

        // Okay, maps created, now prepare flags
        m_immediateHandled = false;
        m_spellState = SPELL_STATE_DELAYED;
        SetDelayStart(0);
    }
    else
    {
        // Immediate spell, no big deal
        handle_immediate();
    }

    SetExecutedCurrently(false);
}

void Spell::handle_immediate()
{
    // start channeling if applicable
    if(IsChanneledSpell(m_spellInfo))
    {
        int32 duration = GetSpellDuration(m_spellInfo);
        if (duration)
        {
            // Apply duration mod
            if(Player* modOwner = m_caster->GetSpellModOwner())
                modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_DURATION, duration);
            m_spellState = SPELL_STATE_CASTING;
            SendChannelStart(duration);
        }
    }

    // process immediate effects (items, ground, etc.) also initialize some variables
    _handle_immediate_phase();

    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    for(std::list<GOTargetInfo>::iterator ihit= m_UniqueGOTargetInfo.begin();ihit != m_UniqueGOTargetInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // spell is finished, perform some last features of the spell here
    _handle_finish_phase();

    // Remove used for cast item if need (it can be already NULL after TakeReagents call
    TakeCastItem();

    if(m_spellState != SPELL_STATE_CASTING)
        finish(true);                                       // successfully finish spell cast (not last in case autorepeat or channel spell)
}

uint64 Spell::handle_delayed(uint64 t_offset)
{
    uint64 next_time = 0;

    if (!m_immediateHandled)
    {
        _handle_immediate_phase();
        m_immediateHandled = true;
    }

    // now recheck units targeting correctness (need before any effects apply to prevent adding immunity at first effect not allow apply second spell effect and similar cases)
    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if (ihit->processed == false)
        {
            if ( ihit->timeDelay <= t_offset )
                DoAllEffectOnTarget(&(*ihit));
            else if( next_time == 0 || ihit->timeDelay < next_time )
                next_time = ihit->timeDelay;
        }
    }

    // now recheck gameobject targeting correctness
    for(std::list<GOTargetInfo>::iterator ighit= m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end();++ighit)
    {
        if (ighit->processed == false)
        {
            if ( ighit->timeDelay <= t_offset )
                DoAllEffectOnTarget(&(*ighit));
            else if( next_time == 0 || ighit->timeDelay < next_time )
                next_time = ighit->timeDelay;
        }
    }
    // All targets passed - need finish phase
    if (next_time == 0)
    {
        // spell is finished, perform some last features of the spell here
        _handle_finish_phase();

        finish(true);                                       // successfully finish spell cast

        // return zero, spell is finished now
        return 0;
    }
    else
    {
        // spell is unfinished, return next execution time
        return next_time;
    }
}

void Spell::_handle_immediate_phase()
{
    // handle some immediate features of the spell here
    HandleThreatSpells(m_spellInfo->Id);

    m_needSpellLog = IsNeedSendToClient();
    for(uint32 j = 0;j<3;j++)
    {
        if(m_spellInfo->Effect[j]==0)
            continue;

        // apply Send Event effect to ground in case empty target lists
        if( m_spellInfo->Effect[j] == SPELL_EFFECT_SEND_EVENT && !HaveTargetsForEffect(j) )
        {
            HandleEffects(NULL,NULL,NULL, j);
            continue;
        }

        // Don't do spell log, if is school damage spell
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_SCHOOL_DAMAGE || m_spellInfo->Effect[j] == 0)
            m_needSpellLog = false;

        uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[j];
        if(m_originalCaster)
            if(Player* modOwner = m_originalCaster->GetSpellModOwner())
                modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);

        // initialize multipliers
        m_damageMultipliers[j] = 1.0f;
        if( (m_spellInfo->EffectImplicitTargetA[j] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[j] == TARGET_CHAIN_HEAL) &&
            (EffectChainTarget > 1) )
            m_applyMultiplierMask |= 1 << j;
    }

    // initialize Diminishing Returns Data
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;

    // process items
    for(std::list<ItemTargetInfo>::iterator ihit= m_UniqueItemInfo.begin();ihit != m_UniqueItemInfo.end();++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // process ground
    for(uint32 j = 0;j<3;j++)
    {
        // persistent area auras target only the ground
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            HandleEffects(NULL,NULL,NULL, j);
    }
}

void Spell::_handle_finish_phase()
{
    // spell log
    if(m_needSpellLog)
        SendLogExecute();
}

void Spell::SendSpellCooldown()
{
    if(m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* _player = (Player*)m_caster;

    // mana/health/etc potions, disabled by client (until combat out as declarate)
    if (m_CastItem && m_CastItem->IsPotion())
    {
        // need in some way provided data for Spell::finish SendCooldownEvent
        _player->SetLastPotionId(m_CastItem->GetEntry());
        return;
    }

    // have infinity cooldown but set at aura apply
    if(m_spellInfo->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE)
        return;

    _player->AddSpellAndCategoryCooldowns(m_spellInfo,m_CastItem ? m_CastItem->GetEntry() : 0, this);
}

void Spell::update(uint32 difftime)
{
    // update pointers based at it's GUIDs
    UpdatePointers();

    if(m_targets.getUnitTargetGUID() && !m_targets.getUnitTarget())
    {
        cancel();
        return;
    }

    // check if the player caster has moved before the spell finished
    if ((m_caster->GetTypeId() == TYPEID_PLAYER && m_timer != 0) &&
        (m_castPositionX != m_caster->GetPositionX() || m_castPositionY != m_caster->GetPositionY() || m_castPositionZ != m_caster->GetPositionZ()) &&
        (m_spellInfo->Effect[0] != SPELL_EFFECT_STUCK || !m_caster->HasUnitMovementFlag(MOVEMENTFLAG_FALLING)))
    {
        // always cancel for channeled spells
        if( m_spellState == SPELL_STATE_CASTING )
            cancel();
        // don't cancel for melee, autorepeat, triggered and instant spells
        else if(!IsNextMeleeSwingSpell() && !IsAutoRepeat() && !m_IsTriggeredSpell && (m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
            cancel();
    }

    switch(m_spellState)
    {
        case SPELL_STATE_PREPARING:
        {
            if(m_timer)
            {
                if(difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if(m_timer == 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat())
                cast();
        } break;
        case SPELL_STATE_CASTING:
        {
            if(m_timer > 0)
            {
                if( m_caster->GetTypeId() == TYPEID_PLAYER )
                {
                    // check if player has jumped before the channeling finished
                    if(m_caster->HasUnitMovementFlag(MOVEMENTFLAG_JUMPING))
                        cancel();

                    // check for incapacitating player states
                    if( m_caster->hasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_CONFUSED))
                        cancel();

                    // check if player has turned if flag is set
                    if( m_spellInfo->ChannelInterruptFlags & CHANNEL_FLAG_TURNING && m_castOrientation != m_caster->GetOrientation() )
                        cancel();
                }

                // check if there are alive targets left
                if (!IsAliveUnitPresentInTargetList())
                {
                    SendChannelUpdate(0);
                    finish();
                }

                if(difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if(m_timer == 0)
            {
                SendChannelUpdate(0);

                // channeled spell processed independently for quest targeting
                // cast at creature (or GO) quest objectives update at successful cast channel finished
                // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
                if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() )
                {
                    if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                    {
                        for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
                        {
                            TargetInfo* target = &*ihit;
                            if(!IS_CREATURE_GUID(target->targetGUID))
                                continue;

                            Unit* unit = m_caster->GetGUID()==target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster,target->targetGUID);
                            if (unit==NULL)
                                continue;

                            p->CastedCreatureOrGO(unit->GetEntry(),unit->GetGUID(),m_spellInfo->Id);
                        }

                        for(std::list<GOTargetInfo>::iterator ihit= m_UniqueGOTargetInfo.begin();ihit != m_UniqueGOTargetInfo.end();++ihit)
                        {
                            GOTargetInfo* target = &*ihit;

                            GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
                            if(!go)
                                continue;

                            p->CastedCreatureOrGO(go->GetEntry(),go->GetGUID(),m_spellInfo->Id);
                        }
                    }
                }

                finish();
            }
        } break;
        default:
        {
        }break;
    }
}

void Spell::finish(bool ok)
{
    if(!m_caster)
        return;

    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    m_spellState = SPELL_STATE_FINISHED;

    // other code related only to successfully finished spells
    if(!ok)
        return;

    //remove spell mods
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->RemoveSpellMods(this);

    // handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
    for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
    {
        if (!(*i)->isAffectedOnSpell(m_spellInfo))
            continue;
        for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            if( ihit->missCondition == SPELL_MISS_NONE )
            {
                // check m_caster->GetGUID() let load auras at login and speedup most often case
                Unit *unit = m_caster->GetGUID()== ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                if (unit && unit->isAlive())
                {
                    SpellEntry const *auraSpellInfo = (*i)->GetSpellProto();
                    uint32 auraSpellIdx = (*i)->GetEffIndex();
                    // Calculate chance at that moment (can be depend for example from combo points)
                    int32 chance = m_caster->CalculateSpellDamage(auraSpellInfo, auraSpellIdx, (*i)->GetBasePoints(),unit);
                    if(roll_chance_i(chance))
                        m_caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i));
                }
            }
    }

    // Heal caster for all health leech from all targets
    if (m_healthLeech)
        m_caster->DealHeal(m_caster, uint32(m_healthLeech), m_spellInfo);

    if (IsMeleeAttackResetSpell())
    {
        m_caster->resetAttackTimer(BASE_ATTACK);
        if(m_caster->haveOffhandWeapon())
            m_caster->resetAttackTimer(OFF_ATTACK);
    }

    /*if (IsRangedAttackResetSpell())
        m_caster->resetAttackTimer(RANGED_ATTACK);*/

    // Clear combo at finish state
    if(m_caster->GetTypeId() == TYPEID_PLAYER && NeedsComboPoints(m_spellInfo))
    {
        // Not drop combopoints if negative spell and if any miss on enemy exist
        bool needDrop = true;
        if (!IsPositiveSpell(m_spellInfo->Id))
        for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            if (ihit->missCondition != SPELL_MISS_NONE && ihit->targetGUID!=m_caster->GetGUID())
            {
                needDrop = false;
                break;
            }
        if (needDrop)
            ((Player*)m_caster)->ClearComboPoints();
    }

    // potions disabled by client, send event "not in combat" if need
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->UpdatePotionCooldown(this);

    // call triggered spell only at successful cast (after clear combo points -> for add some if need)
    if(!m_TriggerSpells.empty())
        TriggerSpell();

    // Stop Attack for some spells
    if( m_spellInfo->Attributes & SPELL_ATTR_STOP_ATTACK_TARGET )
        m_caster->AttackStop();
}

void Spell::SendCastResult(SpellCastResult result)
{
    if(result == SPELL_CAST_OK)
        return;

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if(((Player*)m_caster)->GetSession()->PlayerLoading())  // don't send cast results at loading time
        return;

    WorldPacket data(SMSG_CAST_FAILED, (4+1+1));
    data << uint8(m_cast_count);                            // single cast or multi 2.3 (0/1)
    data << uint32(m_spellInfo->Id);
    data << uint8(result);                                  // problem
    switch (result)
    {
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            data << uint32(m_spellInfo->RequiresSpellFocus);
            break;
        case SPELL_FAILED_REQUIRES_AREA:
            // hardcode areas limitation case
            switch(m_spellInfo->Id)
            {
                case 41617:                                 // Cenarion Mana Salve
                case 41619:                                 // Cenarion Healing Salve
                    data << uint32(3905);
                    break;
                case 41618:                                 // Bottled Nethergon Energy
                case 41620:                                 // Bottled Nethergon Vapor
                    data << uint32(3842);
                    break;
                case 45373:                                 // Bloodberry Elixir
                    data << uint32(4075);
                    break;
                default:                                    // default case (don't must be)
                    data << uint32(0);
                    break;
            }
            break;
        case SPELL_FAILED_TOTEMS:
            if(m_spellInfo->Totem[0])
                data << uint32(m_spellInfo->Totem[0]);
            if(m_spellInfo->Totem[1])
                data << uint32(m_spellInfo->Totem[1]);
            break;
        case SPELL_FAILED_TOTEM_CATEGORY:
            if(m_spellInfo->TotemCategory[0])
                data << uint32(m_spellInfo->TotemCategory[0]);
            if(m_spellInfo->TotemCategory[1])
                data << uint32(m_spellInfo->TotemCategory[1]);
            break;
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
            data << uint32(m_spellInfo->EquippedItemClass);
            data << uint32(m_spellInfo->EquippedItemSubClassMask);
            //data << uint32(m_spellInfo->EquippedItemInventoryTypeMask);
            break;
        default:
            break;
    }
    ((Player*)m_caster)->GetSession()->SendPacket(&data);
}

void Spell::SendSpellStart()
{
    if(!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_START id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN1;
    if(IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;

    if(m_spellInfo->runeCostID)
        castFlags |= CAST_FLAG_UNKNOWN10;

    WorldPacket data(SMSG_SPELL_START, (8+8+4+4+2));
    if(m_CastItem)
        data.append(m_CastItem->GetPackGUID());
    else
        data.append(m_caster->GetPackGUID());

    data.append(m_caster->GetPackGUID());
    data << uint8(m_cast_count);                            // pending spell cast?
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint32(castFlags);                              // cast flags
    data << uint32(m_timer);                                // delay?

    m_targets.write(&data);

    if ( castFlags & CAST_FLAG_UNKNOWN6 )                   // predicted power?
        data << uint32(0);

    if ( castFlags & CAST_FLAG_UNKNOWN7 )                   // rune cooldowns list
    {
        uint8 v1 = 0;//m_runesState;
        uint8 v2 = 0;//((Player*)m_caster)->GetRunesState();
        data << uint8(v1);                                  // runes state before
        data << uint8(v2);                                  // runes state after
        for(uint8 i = 0; i < MAX_RUNES; ++i)
        {
            uint8 m = (1 << i);
            if(m & v1)                                      // usable before...
                if(!(m & v2))                               // ...but on cooldown now...
                    data << uint8(0);                       // some unknown byte (time?)
        }
    }

    if ( castFlags & CAST_FLAG_AMMO )
        WriteAmmoToPacket(&data);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendSpellGo()
{
    // not send invisible spell casting
    if(!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_GO id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN3;
    if(IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;                        // arrows/bullets visual

    if((m_caster->GetTypeId() == TYPEID_PLAYER) && (m_caster->getClass() == CLASS_DEATH_KNIGHT) && m_spellInfo->runeCostID)
    {
        castFlags |= CAST_FLAG_UNKNOWN10;                   // same as in SMSG_SPELL_START
        castFlags |= CAST_FLAG_UNKNOWN6;                    // makes cooldowns visible
        castFlags |= CAST_FLAG_UNKNOWN7;                    // rune cooldowns list
    }

    WorldPacket data(SMSG_SPELL_GO, 50);                    // guess size

    if(m_CastItem)
        data.append(m_CastItem->GetPackGUID());
    else
        data.append(m_caster->GetPackGUID());

    data.append(m_caster->GetPackGUID());
    data << uint8(m_cast_count);                            // pending spell cast?
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint32(castFlags);                              // cast flags
    data << uint32(getMSTime());                            // timestamp

    WriteSpellGoTargets(&data);

    m_targets.write(&data);

    if ( castFlags & CAST_FLAG_UNKNOWN6 )                   // unknown wotlk, predicted power?
        data << uint32(0);

    if ( castFlags & CAST_FLAG_UNKNOWN7 )                   // rune cooldowns list
    {
        uint8 v1 = m_runesState;
        uint8 v2 = ((Player*)m_caster)->GetRunesState();
        data << uint8(v1);                                  // runes state before
        data << uint8(v2);                                  // runes state after
        for(uint8 i = 0; i < MAX_RUNES; ++i)
        {
            uint8 m = (1 << i);
            if(m & v1)                                      // usable before...
                if(!(m & v2))                               // ...but on cooldown now...
                    data << uint8(0);                       // some unknown byte (time?)
        }
    }

    if ( castFlags & CAST_FLAG_UNKNOWN4 )                   // unknown wotlk
    {
        data << float(0);
        data << uint32(0);
    }

    if ( castFlags & CAST_FLAG_AMMO )
        WriteAmmoToPacket(&data);

    if ( castFlags & CAST_FLAG_UNKNOWN5 )                   // unknown wotlk
    {
        data << uint32(0);
        data << uint32(0);
    }

    if ( m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION )
    {
        data << uint8(0);
    }

    m_caster->SendMessageToSet(&data, true);
}

void Spell::WriteAmmoToPacket( WorldPacket * data )
{
    uint32 ammoInventoryType = 0;
    uint32 ammoDisplayID = 0;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item *pItem = ((Player*)m_caster)->GetWeaponForAttack( RANGED_ATTACK );
        if(pItem)
        {
            ammoInventoryType = pItem->GetProto()->InventoryType;
            if( ammoInventoryType == INVTYPE_THROWN )
                ammoDisplayID = pItem->GetProto()->DisplayInfoID;
            else
            {
                uint32 ammoID = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if(ammoID)
                {
                    ItemPrototype const *pProto = objmgr.GetItemPrototype( ammoID );
                    if(pProto)
                    {
                        ammoDisplayID = pProto->DisplayInfoID;
                        ammoInventoryType = pProto->InventoryType;
                    }
                }
                else if(m_caster->GetDummyAura(46699))      // Requires No Ammo
                {
                    ammoDisplayID = 5996;                   // normal arrow
                    ammoInventoryType = INVTYPE_AMMO;
                }
            }
        }
    }
    else
    {
        for (uint8 i = 0; i < 3; ++i)
        {
            if(uint32 item_id = m_caster->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + i))
            {
                if(ItemEntry const * itemEntry = sItemStore.LookupEntry(item_id))
                {
                    if(itemEntry->Class==ITEM_CLASS_WEAPON)
                    {
                        switch(itemEntry->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_THROWN:
                                ammoDisplayID = itemEntry->DisplayId;
                                ammoInventoryType = itemEntry->InventoryType;
                                break;
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                ammoDisplayID = 5996;       // is this need fixing?
                                ammoInventoryType = INVTYPE_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                ammoDisplayID = 5998;       // is this need fixing?
                                ammoInventoryType = INVTYPE_AMMO;
                                break;
                        }

                        if(ammoDisplayID)
                            break;
                    }
                }
            }
        }
    }

    *data << uint32(ammoDisplayID);
    *data << uint32(ammoInventoryType);
}

void Spell::WriteSpellGoTargets( WorldPacket * data )
{
    // This function also fill data for channeled spells:
    // m_needAliveTargetMask req for stop channelig if one target die
    uint32 hit  = m_UniqueGOTargetInfo.size(); // Always hits on GO
    uint32 miss = 0;
    for(std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if ((*ihit).effectMask == 0)                  // No effect apply - all immuned add state
        {
            // possibly SPELL_MISS_IMMUNE2 for this??
            ihit->missCondition = SPELL_MISS_IMMUNE2;
            miss++;
        }
        else if ((*ihit).missCondition == SPELL_MISS_NONE)
            hit++;
        else
            miss++;
    }

    *data << (uint8)hit;
    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
        if ((*ihit).missCondition == SPELL_MISS_NONE)       // Add only hits
        {
            *data << uint64(ihit->targetGUID);
            m_needAliveTargetMask |=ihit->effectMask;
        }

    for(std::list<GOTargetInfo>::const_iterator ighit= m_UniqueGOTargetInfo.begin();ighit != m_UniqueGOTargetInfo.end();++ighit)
        *data << uint64(ighit->targetGUID);                 // Always hits

    *data << (uint8)miss;
    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if( ihit->missCondition != SPELL_MISS_NONE )        // Add only miss
        {
            *data << uint64(ihit->targetGUID);
            *data << uint8(ihit->missCondition);
            if( ihit->missCondition == SPELL_MISS_REFLECT )
                *data << uint8(ihit->reflectResult);
        }
    }
    // Reset m_needAliveTargetMask for non channeled spell
    if(!IsChanneledSpell(m_spellInfo))
        m_needAliveTargetMask = 0;
}

void Spell::SendLogExecute()
{
    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    WorldPacket data(SMSG_SPELLLOGEXECUTE, (8+4+4+4+4+8));

    if(m_caster->GetTypeId() == TYPEID_PLAYER)
        data.append(m_caster->GetPackGUID());
    else
        data.append(target->GetPackGUID());

    data << uint32(m_spellInfo->Id);
    uint32 count1 = 1;
    data << uint32(count1);                                 // count1 (effect count?)
    for(uint32 i = 0; i < count1; ++i)
    {
        data << uint32(m_spellInfo->Effect[0]);             // spell effect
        uint32 count2 = 1;
        data << uint32(count2);                             // count2 (target count?)
        for(uint32 j = 0; j < count2; ++j)
        {
            switch(m_spellInfo->Effect[0])
            {
                case SPELL_EFFECT_POWER_DRAIN:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    data << float(0);
                    break;
                case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);                      // count?
                    break;
                case SPELL_EFFECT_INTERRUPT_CAST:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);                      // spellid
                    break;
                case SPELL_EFFECT_DURABILITY_DAMAGE:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    break;
                case SPELL_EFFECT_OPEN_LOCK:
                case SPELL_EFFECT_OPEN_LOCK_ITEM:
                    if(Item *item = m_targets.getItemTarget())
                        data.append(item->GetPackGUID());
                    else
                        data << uint8(0);
                    break;
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_CREATE_ITEM_2:
                    data << uint32(m_spellInfo->EffectItemType[0]);
                    break;
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_SUMMON_PET:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_CREATE_HOUSE:
                case SPELL_EFFECT_DUEL:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else if(m_targets.getItemTargetGUID())
                        data.appendPackGUID(m_targets.getItemTargetGUID());
                    else if(GameObject *go = m_targets.getGOTarget())
                        data.append(go->GetPackGUID());
                    else
                        data << uint8(0);                   // guid
                    break;
                case SPELL_EFFECT_FEED_PET:
                    data << uint32(m_targets.getItemTargetEntry());
                    break;
                case SPELL_EFFECT_DISMISS_PET:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    break;
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_RESURRECT_NEW:
                    if(Unit *unit = m_targets.getUnitTarget())
                        data.append(unit->GetPackGUID());
                    else
                        data << uint8(0);
                    break;
                default:
                    return;
            }
        }
    }

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendInterrupted(uint8 result)
{
    WorldPacket data(SMSG_SPELL_FAILURE, (8+4+1));
    data.append(m_caster->GetPackGUID());
    data << uint8(m_cast_count);
    data << uint32(m_spellInfo->Id);
    data << uint8(result);
    m_caster->SendMessageToSet(&data, true);

    data.Initialize(SMSG_SPELL_FAILED_OTHER, (8+4));
    data.append(m_caster->GetPackGUID());
    data << uint8(m_cast_count);
    data << uint32(m_spellInfo->Id);
    data << uint8(result);
    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendChannelUpdate(uint32 time)
{
    if(time == 0)
    {
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT,0);
        m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL,0);
    }

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data( MSG_CHANNEL_UPDATE, 8+4 );
    data.append(m_caster->GetPackGUID());
    data << uint32(time);

    ((Player*)m_caster)->GetSession()->SendPacket( &data );
}

void Spell::SendChannelStart(uint32 duration)
{
    WorldObject* target = NULL;

    // select first not resisted target from target list for _0_ effect
    if(!m_UniqueTargetInfo.empty())
    {
        for(std::list<TargetInfo>::const_iterator itr= m_UniqueTargetInfo.begin();itr != m_UniqueTargetInfo.end();++itr)
        {
            if( (itr->effectMask & (1<<0)) && itr->reflectResult==SPELL_MISS_NONE && itr->targetGUID != m_caster->GetGUID())
            {
                target = ObjectAccessor::GetUnit(*m_caster, itr->targetGUID);
                break;
            }
        }
    }
    else if(!m_UniqueGOTargetInfo.empty())
    {
        for(std::list<GOTargetInfo>::const_iterator itr= m_UniqueGOTargetInfo.begin();itr != m_UniqueGOTargetInfo.end();++itr)
        {
            if(itr->effectMask & (1<<0) )
            {
                target = m_caster->GetMap()->GetGameObject(itr->targetGUID);
                break;
            }
        }
    }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data( MSG_CHANNEL_START, (8+4+4) );
        data.append(m_caster->GetPackGUID());
        data << uint32(m_spellInfo->Id);
        data << uint32(duration);

        ((Player*)m_caster)->GetSession()->SendPacket( &data );
    }

    m_timer = duration;
    if(target)
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT, target->GetGUID());
    m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, m_spellInfo->Id);
}

void Spell::SendResurrectRequest(Player* target)
{
    // Both players and NPCs can resurrect using spells - have a look at creature 28487 for example
    // However, the packet structure differs slightly

    const char* sentName = m_caster->GetTypeId()==TYPEID_PLAYER ?"":m_caster->GetNameForLocaleIdx(target->GetSession()->GetSessionDbLocaleIndex());

    WorldPacket data(SMSG_RESURRECT_REQUEST, (8+4+strlen(sentName)+1+1+1));
    data << uint64(m_caster->GetGUID());
    data << uint32(strlen(sentName)+1);

    data << sentName;
    data << uint8(0);

    data << uint8(m_caster->GetTypeId()==TYPEID_PLAYER ?0:1);
    target->GetSession()->SendPacket(&data);
}

void Spell::SendPlaySpellVisual(uint32 SpellID)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 12);
    data << uint64(m_caster->GetGUID());
    data << uint32(SpellID);                                // spell visual id?
    ((Player*)m_caster)->GetSession()->SendPacket(&data);
}

void Spell::TakeCastItem()
{
    if(!m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    // not remove cast item at triggered spell (equipping, weapon damage, etc)
    if(m_IsTriggeredSpell)
        return;

    ItemPrototype const *proto = m_CastItem->GetProto();

    if(!proto)
    {
        // This code is to avoid a crash
        // I'm not sure, if this is really an error, but I guess every item needs a prototype
        sLog.outError("Cast item has no item prototype highId=%d, lowId=%d",m_CastItem->GetGUIDHigh(), m_CastItem->GetGUIDLow());
        return;
    }

    bool expendable = false;
    bool withoutCharges = false;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId)
        {
            // item has limited charges
            if (proto->Spells[i].SpellCharges)
            {
                if (proto->Spells[i].SpellCharges < 0)
                    expendable = true;

                int32 charges = m_CastItem->GetSpellCharges(i);

                // item has charges left
                if (charges)
                {
                    (charges > 0) ? --charges : ++charges;  // abs(charges) less at 1 after use
                    if (proto->Stackable == 1)
                        m_CastItem->SetSpellCharges(i, charges);
                    m_CastItem->SetState(ITEM_CHANGED, (Player*)m_caster);
                }

                // all charges used
                withoutCharges = (charges == 0);
            }
        }
    }

    if (expendable && withoutCharges)
    {
        uint32 count = 1;
        ((Player*)m_caster)->DestroyItemCount(m_CastItem, count, true);

        // prevent crash at access to deleted m_targets.getItemTarget
        if(m_CastItem==m_targets.getItemTarget())
            m_targets.setItemTarget(NULL);

        m_CastItem = NULL;
    }
}

void Spell::TakePower()
{
    if(m_CastItem || m_triggeredByAuraSpell)
        return;

    // health as power used
    if(m_spellInfo->powerType == POWER_HEALTH)
    {
        m_caster->ModifyHealth( -(int32)m_powerCost );
        return;
    }

    if(m_spellInfo->powerType >= MAX_POWERS)
    {
        sLog.outError("Spell::TakePower: Unknown power type '%d'", m_spellInfo->powerType);
        return;
    }

    Powers powerType = Powers(m_spellInfo->powerType);

    if(powerType == POWER_RUNE)
    {
        TakeRunePower();
        return;
    }

    m_caster->ModifyPower(powerType, -(int32)m_powerCost);

    // Set the five second timer
    if (powerType == POWER_MANA && m_powerCost > 0)
        m_caster->SetLastManaUse(getMSTime());
}

SpellCastResult Spell::CheckRuneCost(uint32 runeCostID)
{
    if(m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player *plr = (Player*)m_caster;

    if(plr->getClass() != CLASS_DEATH_KNIGHT)
        return SPELL_CAST_OK;

    SpellRuneCostEntry const *src = sSpellRuneCostStore.LookupEntry(runeCostID);

    if(!src)
        return SPELL_CAST_OK;

    if(src->NoRuneCost())
        return SPELL_CAST_OK;

    int32 runeCost[NUM_RUNE_TYPES];                         // blood, frost, unholy, death

    for(uint32 i = 0; i < RUNE_DEATH; ++i)
        runeCost[i] = src->RuneCost[i];

    runeCost[RUNE_DEATH] = MAX_RUNES;                       // calculated later

    for(uint32 i = 0; i < MAX_RUNES; ++i)
    {
        uint8 rune = plr->GetCurrentRune(i);
        if((plr->GetRuneCooldown(i) == 0) && (runeCost[rune] > 0))
            runeCost[rune]--;
    }

    for(uint32 i = 0; i < RUNE_DEATH; ++i)
        if(runeCost[i] > 0)
            runeCost[RUNE_DEATH] += runeCost[i];

    if(runeCost[RUNE_DEATH] > MAX_RUNES)
        return SPELL_FAILED_NO_POWER;                       // not sure if result code is correct

    return SPELL_CAST_OK;
}

void Spell::TakeRunePower()
{
    if(m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player *plr = (Player*)m_caster;

    if(plr->getClass() != CLASS_DEATH_KNIGHT)
        return;

    SpellRuneCostEntry const *src = sSpellRuneCostStore.LookupEntry(m_spellInfo->runeCostID);

    if(!src || (src->NoRuneCost() && src->NoRunicPowerGain()))
        return;

    m_runesState = plr->GetRunesState();                    // store previous state

    int32 runeCost[NUM_RUNE_TYPES];                         // blood, frost, unholy, death

    for(uint32 i = 0; i < RUNE_DEATH; ++i)
    {
        runeCost[i] = src->RuneCost[i];
    }

    runeCost[RUNE_DEATH] = 0;                               // calculated later

    for(uint32 i = 0; i < MAX_RUNES; ++i)
    {
        uint8 rune = plr->GetCurrentRune(i);
        if((plr->GetRuneCooldown(i) == 0) && (runeCost[rune] > 0))
        {
            plr->SetRuneCooldown(i, RUNE_COOLDOWN);         // 5*2=10 sec
            runeCost[rune]--;
        }
    }

    runeCost[RUNE_DEATH] = runeCost[RUNE_BLOOD] + runeCost[RUNE_UNHOLY] + runeCost[RUNE_FROST];

    if(runeCost[RUNE_DEATH] > 0)
    {
        for(uint32 i = 0; i < MAX_RUNES; ++i)
        {
            uint8 rune = plr->GetCurrentRune(i);
            if((plr->GetRuneCooldown(i) == 0) && (rune == RUNE_DEATH))
            {
                plr->SetRuneCooldown(i, RUNE_COOLDOWN);     // 5*2=10 sec
                runeCost[rune]--;
                plr->ConvertRune(i, plr->GetBaseRune(i));
                if(runeCost[RUNE_DEATH] == 0)
                    break;
            }
        }
    }

    // you can gain some runic power when use runes
    float rp = src->runePowerGain;;
    rp *= sWorld.getRate(RATE_POWER_RUNICPOWER_INCOME);
    plr->ModifyPower(POWER_RUNIC_POWER, (int32)rp);
}

void Spell::TakeReagents()
{
    if(m_IsTriggeredSpell)                                  // reagents used in triggered spell removed by original spell or don't must be removed.
        return;

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* p_caster = (Player*)m_caster;
    if (p_caster->CanNoReagentCast(m_spellInfo))
        return;

    for(uint32 x=0;x<8;x++)
    {
        if(m_spellInfo->Reagent[x] <= 0)
            continue;

        uint32 itemid = m_spellInfo->Reagent[x];
        uint32 itemcount = m_spellInfo->ReagentCount[x];

        // if CastItem is also spell reagent
        if (m_CastItem)
        {
            ItemPrototype const *proto = m_CastItem->GetProto();
            if( proto && proto->ItemId == itemid )
            {
                for(int s=0;s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }

                m_CastItem = NULL;
            }
        }

        // if getItemTarget is also spell reagent
        if (m_targets.getItemTargetEntry()==itemid)
            m_targets.setItemTarget(NULL);

        p_caster->DestroyItemCount(itemid, itemcount, true);
    }
}

void Spell::HandleThreatSpells(uint32 spellId)
{
    if(!m_targets.getUnitTarget() || !spellId)
        return;

    if(!m_targets.getUnitTarget()->CanHaveThreatList())
        return;

    SpellThreatEntry const *threatSpell = sSpellThreatStore.LookupEntry<SpellThreatEntry>(spellId);
    if(!threatSpell)
        return;

    m_targets.getUnitTarget()->AddThreat(m_caster, float(threatSpell->threat));

    DEBUG_LOG("Spell %u, rank %u, added an additional %i threat", spellId, spellmgr.GetSpellRank(spellId), threatSpell->threat);
}

void Spell::HandleEffects(Unit *pUnitTarget,Item *pItemTarget,GameObject *pGOTarget,uint32 i, float DamageMultiplier)
{
    unitTarget = pUnitTarget;
    itemTarget = pItemTarget;
    gameObjTarget = pGOTarget;

    uint8 eff = m_spellInfo->Effect[i];

    damage = int32(CalculateDamage((uint8)i,unitTarget)*DamageMultiplier);

    sLog.outDebug( "Spell: Effect : %u", eff);

    if(eff<TOTAL_SPELL_EFFECTS)
    {
        //sLog.outDebug( "WORLD: Spell FX %d < TOTAL_SPELL_EFFECTS ", eff);
        (*this.*SpellEffects[eff])(i);
    }
    /*
    else
    {
        sLog.outDebug( "WORLD: Spell FX %d > TOTAL_SPELL_EFFECTS ", eff);
        if (m_CastItem)
            EffectEnchantItemTmp(i);
        else
        {
            sLog.outError("SPELL: unknown effect %u spell id %u",
                eff, m_spellInfo->Id);
        }
    }
    */
}

void Spell::TriggerSpell()
{
    for(TriggerSpells::const_iterator si=m_TriggerSpells.begin(); si!=m_TriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), true, m_originalCasterGUID, m_selfContainer);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}

SpellCastResult Spell::CheckCast(bool strict)
{
    // check cooldowns to prevent cheating
    if(m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
    {
        if(m_triggeredByAuraSpell)
            return SPELL_FAILED_DONT_REPORT;
        else
            return SPELL_FAILED_NOT_READY;
    }

    // only allow triggered spells if at an ended battleground
    if( !m_IsTriggeredSpell && m_caster->GetTypeId() == TYPEID_PLAYER)
        if(BattleGround * bg = ((Player*)m_caster)->GetBattleGround())
            if(bg->GetStatus() == STATUS_WAIT_LEAVE)
                return SPELL_FAILED_DONT_REPORT;

    // only check at first call, Stealth auras are already removed at second call
    // for now, ignore triggered spells
    if( strict && !m_IsTriggeredSpell)
    {
        bool checkForm = true;
        // Ignore form req aura
        Unit::AuraList const& ignore = m_caster->GetAurasByType(SPELL_AURA_MOD_IGNORE_SHAPESHIFT);
        for(Unit::AuraList::const_iterator i = ignore.begin(); i != ignore.end(); ++i)
        {
            if (!(*i)->isAffectedOnSpell(m_spellInfo))
                continue;
            checkForm = false;
            break;
        }
        if (checkForm)
        {
            // Cannot be used in this stance/form
            SpellCastResult shapeError = GetErrorAtShapeshiftedCast(m_spellInfo, m_caster->m_form);
            if(shapeError != SPELL_CAST_OK)
                return shapeError;

            if ((m_spellInfo->Attributes & SPELL_ATTR_ONLY_STEALTHED) && !(m_caster->HasStealthAura()))
                return SPELL_FAILED_ONLY_STEALTHED;
        }
    }

    // caster state requirements
    if(m_spellInfo->CasterAuraState && !m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraState)))
        return SPELL_FAILED_CASTER_AURASTATE;
    if(m_spellInfo->CasterAuraStateNot && m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraStateNot)))
        return SPELL_FAILED_CASTER_AURASTATE;

    // Caster aura req check if need
    if(m_spellInfo->casterAuraSpell && !m_caster->HasAura(m_spellInfo->casterAuraSpell))
        return SPELL_FAILED_CASTER_AURASTATE;
    if(m_spellInfo->excludeCasterAuraSpell && m_caster->HasAura(m_spellInfo->excludeCasterAuraSpell))
        return SPELL_FAILED_CASTER_AURASTATE;

    // cancel autorepeat spells if cast start when moving
    // (not wand currently autorepeat cast delayed to moving stop anyway in spell update code)
    if( m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->isMoving() )
    {
        // skip stuck spell to allow use it in falling case and apply spell limitations at movement
        if( (!m_caster->HasUnitMovementFlag(MOVEMENTFLAG_FALLING) || m_spellInfo->Effect[0] != SPELL_EFFECT_STUCK) &&
            (IsAutoRepeat() || (m_spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED) != 0) )
            return SPELL_FAILED_MOVING;
    }

    if(Unit *target = m_targets.getUnitTarget())
    {
        // target state requirements (not allowed state), apply to self also
        if(m_spellInfo->TargetAuraStateNot && target->HasAuraState(AuraState(m_spellInfo->TargetAuraStateNot)))
            return SPELL_FAILED_TARGET_AURASTATE;

        // Target aura req check if need
        if(m_spellInfo->targetAuraSpell && !target->HasAura(m_spellInfo->targetAuraSpell))
            return SPELL_FAILED_CASTER_AURASTATE;
        if(m_spellInfo->excludeTargetAuraSpell && target->HasAura(m_spellInfo->excludeTargetAuraSpell))
            return SPELL_FAILED_CASTER_AURASTATE;

        if(target != m_caster)
        {
            // target state requirements (apply to non-self only), to allow cast affects to self like Dirty Deeds
            if(m_spellInfo->TargetAuraState && !target->HasAuraState(AuraState(m_spellInfo->TargetAuraState)))
                return SPELL_FAILED_TARGET_AURASTATE;

            // Not allow casting on flying player
            if (target->isInFlight())
                return SPELL_FAILED_BAD_TARGETS;

            if(!m_IsTriggeredSpell && VMAP::VMapFactory::checkSpellForLoS(m_spellInfo->Id) && !m_caster->IsWithinLOSInMap(target))
                return SPELL_FAILED_LINE_OF_SIGHT;

            // auto selection spell rank implemented in WorldSession::HandleCastSpellOpcode
            // this case can be triggered if rank not found (too low-level target for first rank)
            if(m_caster->GetTypeId() == TYPEID_PLAYER && !IsPassiveSpell(m_spellInfo->Id) && !m_CastItem)
                for(int i=0;i<3;++i)
                    if(IsPositiveEffect(m_spellInfo->Id, i) && m_spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA)
                        if(target->getLevel() + 10 < m_spellInfo->spellLevel)
                            return SPELL_FAILED_LOWLEVEL;
        }
        else if (m_caster->GetTypeId()==TYPEID_PLAYER) // Target - is player caster
        {
            // Additional check for some spells
            // If 0 spell effect empty - client not send target data (need use selection)
            // TODO: check it on next client version
            if (m_targets.m_targetMask == TARGET_FLAG_SELF &&
                m_spellInfo->EffectImplicitTargetA[1] == TARGET_CHAIN_DAMAGE)
            {
                if (target = m_caster->GetUnit(*m_caster, ((Player *)m_caster)->GetSelection()))
                    m_targets.setUnitTarget(target);
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        // check pet presents
        for(int j=0;j<3;j++)
        {
            if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_PET)
            {
                target = m_caster->GetPet();
                if(!target)
                {
                    if(m_triggeredByAuraSpell)              // not report pet not existence for triggered spells
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_NO_PET;
                }
                break;
            }
        }

        //check creature type
        //ignore self casts (including area casts when caster selected as target)
        if(target != m_caster)
        {
            if(!CheckTargetCreatureType(target))
            {
                if(target->GetTypeId()==TYPEID_PLAYER)
                    return SPELL_FAILED_TARGET_IS_PLAYER;
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        // TODO: this check can be applied and for player to prevent cheating when IsPositiveSpell will return always correct result.
        // check target for pet/charmed casts (not self targeted), self targeted cast used for area effects and etc
        if(m_caster != target && m_caster->GetTypeId()==TYPEID_UNIT && m_caster->GetCharmerOrOwnerGUID())
        {
            // check correctness positive/negative cast target (pet cast real check and cheating check)
            if(IsPositiveSpell(m_spellInfo->Id))
            {
                if(m_caster->IsHostileTo(target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                if(m_caster->IsFriendlyTo(target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        if(IsPositiveSpell(m_spellInfo->Id))
            if(target->IsImmunedToSpell(m_spellInfo))
                return SPELL_FAILED_TARGET_AURASTATE;

        //Must be behind the target.
        if( m_spellInfo->AttributesEx2 == 0x100000 && (m_spellInfo->AttributesEx & 0x200) == 0x200 && target->HasInArc(M_PI, m_caster) )
        {
            //Exclusion for Pounce: Facing Limitation was removed in 2.0.1, but it still uses the same, old Ex-Flags
            if( m_spellInfo->SpellFamilyName != SPELLFAMILY_DRUID || m_spellInfo->SpellFamilyFlags != 0x0000000000020000LL )
            {
                SendInterrupted(2);
                return SPELL_FAILED_NOT_BEHIND;
            }
        }

        //Target must be facing you.
        if((m_spellInfo->Attributes == 0x150010) && !target->HasInArc(M_PI, m_caster) )
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_INFRONT;
        }

        // check if target is in combat
        if (target != m_caster && (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_IN_COMBAT_TARGET) && target->isInCombat())
            return SPELL_FAILED_TARGET_AFFECTING_COMBAT;
    }

    // Spell casted only on battleground
    if((m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_BATTLEGROUND) &&  m_caster->GetTypeId()==TYPEID_PLAYER)
        if(!((Player*)m_caster)->InBattleGround())
            return SPELL_FAILED_ONLY_BATTLEGROUNDS;

    // do not allow spells to be cast in arenas
    // - with greater than 15 min CD without SPELL_ATTR_EX4_USABLE_IN_ARENA flag
    // - with SPELL_ATTR_EX4_NOT_USABLE_IN_ARENA flag
    if( (m_spellInfo->AttributesEx4 & SPELL_ATTR_EX4_NOT_USABLE_IN_ARENA) ||
        GetSpellRecoveryTime(m_spellInfo) > 15 * MINUTE * IN_MILISECONDS && !(m_spellInfo->AttributesEx4 & SPELL_ATTR_EX4_USABLE_IN_ARENA) )
        if(MapEntry const* mapEntry = sMapStore.LookupEntry(m_caster->GetMapId()))
            if(mapEntry->IsBattleArena())
                return SPELL_FAILED_NOT_IN_ARENA;

    // zone check
    uint32 zone, area;
    m_caster->GetZoneAndAreaId(zone,area);

    SpellCastResult locRes= spellmgr.GetSpellAllowedInLocationError(m_spellInfo,m_caster->GetMapId(),zone,area,
        m_caster->GetTypeId()==TYPEID_PLAYER ? ((Player*)m_caster) : NULL);
    if(locRes != SPELL_CAST_OK)
        return locRes;

    // not let players cast spells at mount (and let do it to creatures)
    if( m_caster->IsMounted() && m_caster->GetTypeId()==TYPEID_PLAYER && !m_IsTriggeredSpell &&
        !IsPassiveSpell(m_spellInfo->Id) && !(m_spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_MOUNTED) )
    {
        if(m_caster->isInFlight())
            return SPELL_FAILED_NOT_FLYING;
        else
            return SPELL_FAILED_NOT_MOUNTED;
    }

    // always (except passive spells) check items (focus object can be required for any type casts)
    if(!IsPassiveSpell(m_spellInfo->Id))
    {
        SpellCastResult castResult = CheckItems();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    //ImpliciteTargetA-B = 38, If fact there is 0 Spell with  ImpliciteTargetB=38
    if(m_UniqueTargetInfo.empty())                          // skip second CheckCast apply (for delayed spells for example)
    {
        for(uint8 j = 0; j < 3; j++)
        {
            if( m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[j] != TARGET_SELF ||
                m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
            {
                SpellScriptTarget::const_iterator lower = spellmgr.GetBeginSpellScriptTarget(m_spellInfo->Id);
                SpellScriptTarget::const_iterator upper = spellmgr.GetEndSpellScriptTarget(m_spellInfo->Id);
                if(lower==upper)
                    sLog.outErrorDb("Spell (ID: %u) has effect EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT or TARGET_SCRIPT_COORDINATES, but does not have record in `spell_script_target`",m_spellInfo->Id);

                SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
                float range = GetSpellMaxRange(srange);

                Creature* creatureScriptTarget = NULL;
                GameObject* goScriptTarget = NULL;

                for(SpellScriptTarget::const_iterator i_spellST = lower; i_spellST != upper; ++i_spellST)
                {
                    switch(i_spellST->second.type)
                    {
                        case SPELL_TARGET_TYPE_GAMEOBJECT:
                        {
                            GameObject* p_GameObject = NULL;

                            if(i_spellST->second.targetEntry)
                            {
                                CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                                Cell cell(p);
                                cell.data.Part.reserved = ALL_DISTRICT;

                                MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster,i_spellST->second.targetEntry,range);
                                MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(m_caster, p_GameObject,go_check);

                                TypeContainerVisitor<MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck>, GridTypeMapContainer > object_checker(checker);
                                CellLock<GridReadGuard> cell_lock(cell, p);
                                cell_lock->Visit(cell_lock, object_checker, *m_caster->GetMap());

                                if(p_GameObject)
                                {
                                    // remember found target and range, next attempt will find more near target with another entry
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = p_GameObject;
                                    range = go_check.GetLastRange();
                                }
                            }
                            else if( focusObject )          //Focus Object
                            {
                                float frange = m_caster->GetDistance(focusObject);
                                if(range >= frange)
                                {
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = focusObject;
                                    range = frange;
                                }
                            }
                            break;
                        }
                        case SPELL_TARGET_TYPE_CREATURE:
                        case SPELL_TARGET_TYPE_DEAD:
                        default:
                        {
                            Creature *p_Creature = NULL;

                            CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
                            Cell cell(p);
                            cell.data.Part.reserved = ALL_DISTRICT;
                            cell.SetNoCreate();             // Really don't know what is that???

                            MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*m_caster,i_spellST->second.targetEntry,i_spellST->second.type!=SPELL_TARGET_TYPE_DEAD,range);
                            MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(m_caster, p_Creature, u_check);

                            TypeContainerVisitor<MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck>, GridTypeMapContainer >  grid_creature_searcher(searcher);

                            CellLock<GridReadGuard> cell_lock(cell, p);
                            cell_lock->Visit(cell_lock, grid_creature_searcher, *m_caster->GetMap());

                            if(p_Creature )
                            {
                                creatureScriptTarget = p_Creature;
                                goScriptTarget = NULL;
                                range = u_check.GetLastRange();
                            }
                            break;
                        }
                    }
                }

                if(creatureScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
                    {
                        m_targets.setDestination(creatureScriptTarget->GetPositionX(),creatureScriptTarget->GetPositionY(),creatureScriptTarget->GetPositionZ());

                        if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->EffectImplicitTargetB[j] == 0 && m_spellInfo->Effect[j]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddUnitTarget(creatureScriptTarget, j);
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                        AddUnitTarget(creatureScriptTarget, j);
                }
                else if(goScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES )
                    {
                        m_targets.setDestination(goScriptTarget->GetPositionX(),goScriptTarget->GetPositionY(),goScriptTarget->GetPositionZ());

                        if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->EffectImplicitTargetB[j] == 0 && m_spellInfo->Effect[j]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddGOTarget(goScriptTarget, j);
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                        AddGOTarget(goScriptTarget, j);
                }
                //Missing DB Entry or targets for this spellEffect.
                else
                {
                    // not report target not existence for triggered spells
                    if(m_triggeredByAuraSpell || m_IsTriggeredSpell)
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
    }

    if(!m_IsTriggeredSpell)
    {
        SpellCastResult castResult = CheckRange(strict);
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    {
        SpellCastResult castResult = CheckPower();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    if(!m_IsTriggeredSpell)                             // triggered spell not affected by stun/etc
    {
        SpellCastResult castResult = CheckCasterAuras();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    for (int i = 0; i < 3; ++i)
    {
        // for effects of spells that have only one target
        switch(m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_DUMMY:
            {
                if(m_spellInfo->SpellIconID == 1648)        // Execute
                {
                    if(!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if (m_spellInfo->Id == 51582)          // Rocket Boots Engaged
                {
                    if(m_caster->IsInWater())
                        return SPELL_FAILED_ONLY_ABOVEWATER;
                }
                else if(m_spellInfo->SpellIconID==156)      // Holy Shock
                {
                    // spell different for friends and enemies
                    // hart version required facing
                    if(m_targets.getUnitTarget() && !m_caster->IsFriendlyTo(m_targets.getUnitTarget()) && !m_caster->HasInArc( M_PI, m_targets.getUnitTarget() ))
                        return SPELL_FAILED_UNIT_NOT_INFRONT;
                }
                break;
            }
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                // Hammer of Wrath
                if(m_spellInfo->SpellVisual[0] == 7250)
                {
                    if (!m_targets.getUnitTarget())
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                    if(m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_EFFECT_TAMECREATURE:
            {
                if (!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_targets.getUnitTarget()->getLevel() > m_caster->getLevel())
                    return SPELL_FAILED_HIGHLEVEL;

                // use SMSG_PET_TAME_FAILURE?
                if (!((Creature*)m_targets.getUnitTarget())->GetCreatureInfo()->isTameable ())
                    return SPELL_FAILED_BAD_TARGETS;

                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                if(m_spellInfo->EffectImplicitTargetA[i] != TARGET_PET)
                    break;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if(m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                break;
            }
            case SPELL_EFFECT_LEARN_PET_SPELL:
            {
                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if(m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                break;
            }
            case SPELL_EFFECT_FEED_PET:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                Item* foodItem = m_targets.getItemTarget();
                if(!foodItem)
                    return SPELL_FAILED_BAD_TARGETS;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(!pet->HaveInDiet(foodItem->GetProto()))
                    return SPELL_FAILED_WRONG_PET_FOOD;

                if(!pet->GetCurrentFoodBenefitLevel(foodItem->GetProto()->ItemLevel))
                    return SPELL_FAILED_FOOD_LOWLEVEL;

                if(m_caster->isInCombat() || pet->isInCombat())
                    return SPELL_FAILED_AFFECTING_COMBAT;

                break;
            }
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_POWER_DRAIN:
            {
                // Can be area effect, Check only for players and not check if target - caster (spell can have multiply drain/burn effects)
                if(m_caster->GetTypeId() == TYPEID_PLAYER)
                    if(Unit* target = m_targets.getUnitTarget())
                        if(target!=m_caster && target->getPowerType()!=m_spellInfo->EffectMiscValue[i])
                            return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_CHARGE:
            {
                if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                    return SPELL_FAILED_ROOTED;

                break;
            }
            case SPELL_EFFECT_SKINNING:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() != TYPEID_UNIT)
                    return SPELL_FAILED_BAD_TARGETS;

                if( !(m_targets.getUnitTarget()->GetUInt32Value(UNIT_FIELD_FLAGS) & UNIT_FLAG_SKINNABLE) )
                    return SPELL_FAILED_TARGET_UNSKINNABLE;

                Creature* creature = (Creature*)m_targets.getUnitTarget();
                if ( creature->GetCreatureType() != CREATURE_TYPE_CRITTER && ( !creature->lootForBody || !creature->loot.empty() ) )
                {
                    return SPELL_FAILED_TARGET_NOT_LOOTED;
                }

                uint32 skill = creature->GetCreatureInfo()->GetRequiredLootSkill();

                int32 skillValue = ((Player*)m_caster)->GetSkillValue(skill);
                int32 TargetLevel = m_targets.getUnitTarget()->getLevel();
                int32 ReqValue = (skillValue < 100 ? (TargetLevel-10)*10 : TargetLevel*5);
                if (ReqValue > skillValue)
                    return SPELL_FAILED_LOW_CASTLEVEL;

                // chance for fail at orange skinning attempt
                if( (m_selfContainer && (*m_selfContainer) == this) &&
                    skillValue < sWorld.GetConfigMaxSkillValue() &&
                    (ReqValue < 0 ? 0 : ReqValue) > irand(skillValue-25, skillValue+37) )
                    return SPELL_FAILED_TRY_AGAIN;

                break;
            }
            case SPELL_EFFECT_OPEN_LOCK_ITEM:
            case SPELL_EFFECT_OPEN_LOCK:
            {
                if( m_spellInfo->EffectImplicitTargetA[i] != TARGET_GAMEOBJECT &&
                    m_spellInfo->EffectImplicitTargetA[i] != TARGET_GAMEOBJECT_ITEM )
                    break;

                if( m_caster->GetTypeId() != TYPEID_PLAYER  // only players can open locks, gather etc.
                    // we need a go target in case of TARGET_GAMEOBJECT
                    || m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT && !m_targets.getGOTarget()
                    // we need a go target, or an openable item target in case of TARGET_GAMEOBJECT_ITEM
                    || m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT_ITEM && !m_targets.getGOTarget() &&
                    (!m_targets.getItemTarget() || !m_targets.getItemTarget()->GetProto()->LockID || m_targets.getItemTarget()->GetOwner() != m_caster ) )
                    return SPELL_FAILED_BAD_TARGETS;

                // In BattleGround players can use only flags and banners
                if( ((Player*)m_caster)->InBattleGround() &&
                    !((Player*)m_caster)->CanUseBattleGroundObject() )
                    return SPELL_FAILED_TRY_AGAIN;

                // get the lock entry
                uint32 lockId = 0;
                if (GameObject* go=m_targets.getGOTarget())
                    lockId = go->GetLockId();
                else if(Item* itm=m_targets.getItemTarget())
                    lockId = itm->GetProto()->LockID;

                SkillType skillId =SKILL_NONE;
                int32 reqSkillValue = 0;
                int32 skillValue = 0;

                // check lock compatibility
                SpellCastResult res = CanOpenLock(i,lockId,skillId,reqSkillValue,skillValue);
                if(res != SPELL_CAST_OK)
                    return res;

                // chance for fail at orange mining/herb/LockPicking gathering attempt
                // second check prevent fail at rechecks
                if(skillId != SKILL_NONE && (!m_selfContainer || ((*m_selfContainer) != this)))
                {
                    bool canFailAtMax = skillId != SKILL_HERBALISM && skillId != SKILL_MINING;

                    // chance for failure in orange gather / lockpick (gathering skill can't fail at maxskill)
                    if((canFailAtMax || skillValue < sWorld.GetConfigMaxSkillValue()) && reqSkillValue > irand(skillValue-25, skillValue+37))
                        return SPELL_FAILED_TRY_AGAIN;
                }
                break;
            }
            case SPELL_EFFECT_SUMMON_DEAD_PET:
            {
                Creature *pet = m_caster->GetPet();
                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(pet->isAlive())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                break;
            }
            // This is generic summon effect
            case SPELL_EFFECT_SUMMON:
            {
                switch(m_spellInfo->EffectMiscValueB[i])
                {
                    case SUMMON_TYPE_POSESSED:
                    case SUMMON_TYPE_POSESSED2:
                    case SUMMON_TYPE_DEMON:
                    case SUMMON_TYPE_SUMMON:
                    {
                        if(m_caster->GetPetGUID())
                            return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                        if(m_caster->GetCharmGUID())
                            return SPELL_FAILED_ALREADY_HAVE_CHARM;
                        break;
                    }
                }
                break;
            }
            // Not used for summon?
            case SPELL_EFFECT_SUMMON_PHANTASM:
            {
                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PET:
            {
                if(m_caster->GetPetGUID())                  //let warlock do a replacement summon
                {

                    Pet* pet = ((Player*)m_caster)->GetPet();

                    if (m_caster->GetTypeId()==TYPEID_PLAYER && m_caster->getClass()==CLASS_WARLOCK)
                    {
                        if (strict)                         //starting cast, trigger pet stun (cast by pet so it doesn't attack player)
                            pet->CastSpell(pet, 32752, true, NULL, NULL, pet->GetGUID());
                    }
                    else
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
                }

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PLAYER:
            {
                if(m_caster->GetTypeId()!=TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;
                if(!((Player*)m_caster)->GetSelection())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = objmgr.GetPlayer(((Player*)m_caster)->GetSelection());
                if( !target || ((Player*)m_caster)==target || !target->IsInSameRaidWith((Player*)m_caster) )
                    return SPELL_FAILED_BAD_TARGETS;

                // check if our map is dungeon
                if( sMapStore.LookupEntry(m_caster->GetMapId())->IsDungeon() )
                {
                    InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(m_caster->GetMapId());
                    if(!instance)
                        return SPELL_FAILED_TARGET_NOT_IN_INSTANCE;
                    if ( instance->levelMin > target->getLevel() )
                        return SPELL_FAILED_LOWLEVEL;
                    if ( instance->levelMax && instance->levelMax < target->getLevel() )
                        return SPELL_FAILED_HIGHLEVEL;
                }
                break;
            }
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            {
                float dis = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]));
                float fx = m_caster->GetPositionX() + dis * cos(m_caster->GetOrientation());
                float fy = m_caster->GetPositionY() + dis * sin(m_caster->GetOrientation());
                // teleport a bit above terrain level to avoid falling below it
                float fz = MapManager::Instance().GetBaseMap(m_caster->GetMapId())->GetHeight(fx,fy,m_caster->GetPositionZ(),true);
                if(fz <= INVALID_HEIGHT)                    // note: this also will prevent use effect in instances without vmaps height enabled
                    return SPELL_FAILED_TRY_AGAIN;

                float caster_pos_z = m_caster->GetPositionZ();
                // Control the caster to not climb or drop when +-fz > 8
                if(!(fz<=caster_pos_z+8 && fz>=caster_pos_z-8))
                    return SPELL_FAILED_TRY_AGAIN;

                // not allow use this effect at battleground until battleground start
                if(m_caster->GetTypeId()==TYPEID_PLAYER)
                    if(BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                        if(bg->GetStatus() != STATUS_IN_PROGRESS)
                            return SPELL_FAILED_TRY_AGAIN;
                break;
            }
            case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
            {
                if (m_targets.getUnitTarget()==m_caster)
                    return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            default:break;
        }
    }

    for (int i = 0; i < 3; ++i)
    {
        switch(m_spellInfo->EffectApplyAuraName[i])
        {
            case SPELL_AURA_DUMMY:
            {
                //custom check
                switch(m_spellInfo->Id)
                {
                    case 61336:
                        if(m_caster->GetTypeId()!=TYPEID_PLAYER || !((Player*)m_caster)->IsInFeralForm())
                            return SPELL_FAILED_ONLY_SHAPESHIFT;
                        break;
                    default:
                        break;
                }
                break;
            }
            case SPELL_AURA_MOD_POSSESS:
            case SPELL_AURA_MOD_CHARM:
            {
                if(m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if(m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if(m_caster->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if(m_targets.getUnitTarget()->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if(int32(m_targets.getUnitTarget()->getLevel()) > CalculateDamage(i,m_targets.getUnitTarget()))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOUNTED:
            {
                if (m_caster->IsInWater())
                    return SPELL_FAILED_ONLY_ABOVEWATER;

                if (m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->GetTransport())
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                // Ignore map check if spell have AreaId. AreaId already checked and this prevent special mount spells
                if (m_caster->GetTypeId()==TYPEID_PLAYER && !sMapStore.LookupEntry(m_caster->GetMapId())->IsMountAllowed() && !m_IsTriggeredSpell && !m_spellInfo->AreaGroupId)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                ShapeshiftForm form = m_caster->m_form;
                if( form == FORM_CAT          || form == FORM_TREE      || form == FORM_TRAVEL   ||
                    form == FORM_AQUA         || form == FORM_BEAR      || form == FORM_DIREBEAR ||
                    form == FORM_CREATUREBEAR || form == FORM_GHOSTWOLF || form == FORM_FLIGHT   ||
                    form == FORM_FLIGHT_EPIC  || form == FORM_MOONKIN   || form == FORM_METAMORPHOSIS )
                    return SPELL_FAILED_NOT_SHAPESHIFT;

                break;
            }
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
            {
                if(!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                // can be casted at non-friendly unit or own pet/charm
                if(m_caster->IsFriendlyTo(m_targets.getUnitTarget()))
                    return SPELL_FAILED_TARGET_FRIENDLY;

                break;
            }
            case SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED:
            case SPELL_AURA_FLY:
            {
                // not allow cast fly spells at old maps by players (all spells is self target)
                if(m_caster->GetTypeId()==TYPEID_PLAYER)
                {
                    if( !((Player*)m_caster)->IsAllowUseFlyMountsHere() )
                        return SPELL_FAILED_NOT_HERE;
                }

                break;
            }
            case SPELL_AURA_PERIODIC_MANA_LEECH:
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_caster->GetTypeId()!=TYPEID_PLAYER || m_CastItem)
                    break;

                if(m_targets.getUnitTarget()->getPowerType()!=POWER_MANA)
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            default:
                break;
        }
    }

    // all ok
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPetCast(Unit* target)
{
    if(!m_caster->isAlive())
        return SPELL_FAILED_CASTER_DEAD;

    if(m_caster->IsNonMeleeSpellCasted(false))              //prevent spellcast interruption by another spellcast
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    if(m_caster->isInCombat() && IsNonCombatSpell(m_spellInfo))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if(m_caster->GetTypeId()==TYPEID_UNIT && (((Creature*)m_caster)->isPet() || m_caster->isCharmed()))
    {
                                                            //dead owner (pets still alive when owners ressed?)
        if(m_caster->GetCharmerOrOwner() && !m_caster->GetCharmerOrOwner()->isAlive())
            return SPELL_FAILED_CASTER_DEAD;

        if(!target && m_targets.getUnitTarget())
            target = m_targets.getUnitTarget();

        bool need = false;
        for(uint32 i = 0;i<3;++i)
        {
            if(m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND || m_spellInfo->EffectImplicitTargetA[i] == TARGET_DUELVSPLAYER || m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_PARTY || m_spellInfo->EffectImplicitTargetA[i] == TARGET_CURRENT_ENEMY_COORDINATES)
            {
                need = true;
                if(!target)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
                break;
            }
        }
        if(need)
            m_targets.setUnitTarget(target);

        Unit* _target = m_targets.getUnitTarget();

        if(_target)                                         //for target dead/target not valid
        {
            if(!_target->isAlive())
                return SPELL_FAILED_BAD_TARGETS;

            if(IsPositiveSpell(m_spellInfo->Id))
            {
                if(m_caster->IsHostileTo(_target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                bool duelvsplayertar = false;
                for(int j=0;j<3;j++)
                {
                                                            //TARGET_DUELVSPLAYER is positive AND negative
                    duelvsplayertar |= (m_spellInfo->EffectImplicitTargetA[j] == TARGET_DUELVSPLAYER);
                }
                if(m_caster->IsFriendlyTo(target) && !duelvsplayertar)
                {
                    return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
                                                            //cooldown
        if(((Creature*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
            return SPELL_FAILED_NOT_READY;
    }

    return CheckCast(true);
}

SpellCastResult Spell::CheckCasterAuras() const
{
    // Flag drop spells totally immuned to caster auras
    // FIXME: find more nice check for all totally immuned spells
    // AttributesEx3 & 0x10000000?
    if(m_spellInfo->Id==23336 || m_spellInfo->Id==23334 || m_spellInfo->Id==34991)
        return SPELL_CAST_OK;

    uint8 school_immune = 0;
    uint32 mechanic_immune = 0;
    uint32 dispel_immune = 0;

    //Check if the spell grants school or mechanic immunity.
    //We use bitmasks so the loop is done only once and not on every aura check below.
    if ( m_spellInfo->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY )
    {
        for(int i = 0;i < 3; i ++)
        {
            if(m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_IMMUNITY)
                school_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if(m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY)
                mechanic_immune |= 1 << uint32(m_spellInfo->EffectMiscValue[i]);
            else if(m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_DISPEL_IMMUNITY)
                dispel_immune |= GetDispellMask(DispelType(m_spellInfo->EffectMiscValue[i]));
        }
        //immune movement impairment and loss of control
        if(m_spellInfo->Id==(uint32)42292)
            mechanic_immune = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
    }

    //Check whether the cast should be prevented by any state you might have.
    SpellCastResult prevented_reason = SPELL_CAST_OK;
    // Have to check if there is a stun aura. Otherwise will have problems with ghost aura apply while logging out
    uint32 unitflag = m_caster->GetUInt32Value(UNIT_FIELD_FLAGS);     // Get unit state
    if(unitflag & UNIT_FLAG_STUNNED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
        prevented_reason = SPELL_FAILED_STUNNED;
    else if(unitflag & UNIT_FLAG_CONFUSED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
        prevented_reason = SPELL_FAILED_CONFUSED;
    else if(unitflag & UNIT_FLAG_FLEEING && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
        prevented_reason = SPELL_FAILED_FLEEING;
    else if(unitflag & UNIT_FLAG_SILENCED && m_spellInfo->PreventionType==SPELL_PREVENTION_TYPE_SILENCE)
        prevented_reason = SPELL_FAILED_SILENCED;
    else if(unitflag & UNIT_FLAG_PACIFIED && m_spellInfo->PreventionType==SPELL_PREVENTION_TYPE_PACIFY)
        prevented_reason = SPELL_FAILED_PACIFIED;

    // Attr must make flag drop spell totally immune from all effects
    if(prevented_reason != SPELL_CAST_OK)
    {
        if(school_immune || mechanic_immune || dispel_immune)
        {
            //Checking auras is needed now, because you are prevented by some state but the spell grants immunity.
            Unit::AuraMap const& auras = m_caster->GetAuras();
            for(Unit::AuraMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
            {
                if(itr->second)
                {
                    if( GetSpellMechanicMask(itr->second->GetSpellProto(), itr->second->GetEffIndex()) & mechanic_immune )
                        continue;
                    if( GetSpellSchoolMask(itr->second->GetSpellProto()) & school_immune )
                        continue;
                    if( (1<<(itr->second->GetSpellProto()->Dispel)) & dispel_immune)
                        continue;

                    //Make a second check for spell failed so the right SPELL_FAILED message is returned.
                    //That is needed when your casting is prevented by multiple states and you are only immune to some of them.
                    switch(itr->second->GetModifier()->m_auraname)
                    {
                        case SPELL_AURA_MOD_STUN:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
                                return SPELL_FAILED_STUNNED;
                            break;
                        case SPELL_AURA_MOD_CONFUSE:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
                                return SPELL_FAILED_CONFUSED;
                            break;
                        case SPELL_AURA_MOD_FEAR:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
                                return SPELL_FAILED_FLEEING;
                            break;
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_PACIFY:
                        case SPELL_AURA_MOD_PACIFY_SILENCE:
                            if( m_spellInfo->PreventionType==SPELL_PREVENTION_TYPE_PACIFY)
                                return SPELL_FAILED_PACIFIED;
                            else if ( m_spellInfo->PreventionType==SPELL_PREVENTION_TYPE_SILENCE)
                                return SPELL_FAILED_SILENCED;
                            break;
                    }
                }
            }
        }
        //You are prevented from casting and the spell casted does not grant immunity. Return a failed error.
        else
            return prevented_reason;
    }
    return SPELL_CAST_OK;
}

bool Spell::CanAutoCast(Unit* target)
{
    uint64 targetguid = target->GetGUID();

    for(uint32 j = 0;j<3;j++)
    {
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_APPLY_AURA)
        {
            if( m_spellInfo->StackAmount <= 1)
            {
                if( target->HasAura(m_spellInfo->Id, j) )
                    return false;
            }
            else
            {
                if( target->GetAuras().count(Unit::spellEffectPair(m_spellInfo->Id, j)) >= m_spellInfo->StackAmount)
                    return false;
            }
        }
        else if ( IsAreaAuraEffect( m_spellInfo->Effect[j] ))
        {
                if( target->HasAura(m_spellInfo->Id, j) )
                    return false;
        }
    }

    SpellCastResult result = CheckPetCast(target);

    if(result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
    {
        FillTargetMap();
        //check if among target units, our WANTED target is as well (->only self cast spells return false)
        for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
            if( ihit->targetGUID == targetguid )
                return true;
    }
    return false;                                           //target invalid
}

SpellCastResult Spell::CheckRange(bool strict)
{
    float range_mod;

    // self cast doesn't need range checking -- also for Starshards fix
    if (m_spellInfo->rangeIndex == 1)
        return SPELL_CAST_OK;

    if (strict)                                             //add radius of caster
        range_mod = 1.25;
    else                                                    //add radius of caster and ~5 yds "give"
        range_mod = 6.25;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange) + range_mod;
    float min_range = GetSpellMinRange(srange);

    if(Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, max_range, this);

    Unit *target = m_targets.getUnitTarget();

    if(target && target != m_caster)
    {
        // distance from target in checks
        float dist = m_caster->GetCombatDistance(target);

        if(dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;               //0x5A;
        if(dist < min_range)
            return SPELL_FAILED_TOO_CLOSE;
        if( m_caster->GetTypeId() == TYPEID_PLAYER &&
            (m_spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc( M_PI, target ) )
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    if(m_targets.m_targetMask == TARGET_FLAG_DEST_LOCATION && m_targets.m_destX != 0 && m_targets.m_destY != 0 && m_targets.m_destZ != 0)
    {
        if(!m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ,max_range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if(m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ,min_range))
            return SPELL_FAILED_TOO_CLOSE;
    }

    return SPELL_CAST_OK;
}

int32 Spell::CalculatePowerCost()
{
    // item cast not used power
    if(m_CastItem)
        return 0;

    // Spell drain all exist power on cast (Only paladin lay of Hands)
    if (m_spellInfo->AttributesEx & SPELL_ATTR_EX_DRAIN_ALL_POWER)
    {
        // If power type - health drain all
        if (m_spellInfo->powerType == POWER_HEALTH)
            return m_caster->GetHealth();
        // Else drain all power
        if (m_spellInfo->powerType < MAX_POWERS)
            return m_caster->GetPower(Powers(m_spellInfo->powerType));
        sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", m_spellInfo->powerType, m_spellInfo->Id);
        return 0;
    }

    // Base powerCost
    int32 powerCost = m_spellInfo->manaCost;
    // PCT cost from total amount
    if (m_spellInfo->ManaCostPercentage)
    {
        switch (m_spellInfo->powerType)
        {
            // health as power used
            case POWER_HEALTH:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetCreateHealth() / 100;
                break;
            case POWER_MANA:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetCreateMana() / 100;
                break;
            case POWER_RAGE:
            case POWER_FOCUS:
            case POWER_ENERGY:
            case POWER_HAPPINESS:
                powerCost += m_spellInfo->ManaCostPercentage * m_caster->GetMaxPower(Powers(m_spellInfo->powerType)) / 100;
                break;
            case POWER_RUNE:
            case POWER_RUNIC_POWER:
                sLog.outDebug("Spell::CalculateManaCost: Not implemented yet!");
                break;
            default:
                sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", m_spellInfo->powerType, m_spellInfo->Id);
                return 0;
        }
    }
    SpellSchools school = GetFirstSchoolInMask(m_spellSchoolMask);
    // Flat mod from caster auras by spell school
    powerCost += m_caster->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + school);
    // Shiv - costs 20 + weaponSpeed*10 energy (apply only to non-triggered spell with energy cost)
    if ( m_spellInfo->AttributesEx4 & SPELL_ATTR_EX4_SPELL_VS_EXTEND_COST )
        powerCost += m_caster->GetAttackTime(OFF_ATTACK)/100;
    // Apply cost mod by spell
    if(Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_COST, powerCost, this);

    if(m_spellInfo->Attributes & SPELL_ATTR_LEVEL_DAMAGE_CALCULATION)
        powerCost = int32(powerCost/ (1.117f* m_spellInfo->spellLevel / m_caster->getLevel() -0.1327f));

    // PCT mod from user auras by school
    powerCost = int32(powerCost * (1.0f+m_caster->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER+school)));
    if (powerCost < 0)
        powerCost = 0;
    return powerCost;
}

SpellCastResult Spell::CheckPower()
{
    // item cast not used power
    if(m_CastItem)
        return SPELL_CAST_OK;

    // health as power used - need check health amount
    if(m_spellInfo->powerType == POWER_HEALTH)
    {
        if(m_caster->GetHealth() <= m_powerCost)
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }
    // Check valid power type
    if( m_spellInfo->powerType >= MAX_POWERS )
    {
        sLog.outError("Spell::CheckMana: Unknown power type '%d'", m_spellInfo->powerType);
        return SPELL_FAILED_UNKNOWN;
    }

    SpellCastResult failReason = CheckRuneCost(m_spellInfo->runeCostID);
    if(failReason != SPELL_CAST_OK)
        return failReason;

    // Check power amount
    Powers powerType = Powers(m_spellInfo->powerType);
    if(m_caster->GetPower(powerType) < m_powerCost)
        return SPELL_FAILED_NO_POWER;
    else
        return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckItems()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player* p_caster = (Player*)m_caster;

    // cast item checks
    if(m_CastItem)
    {
        uint32 itemid = m_CastItem->GetEntry();
        if( !p_caster->HasItemCount(itemid,1) )
            return SPELL_FAILED_ITEM_NOT_READY;

        ItemPrototype const *proto = m_CastItem->GetProto();
        if(!proto)
            return SPELL_FAILED_ITEM_NOT_READY;

        for (int i = 0; i<5; ++i)
            if (proto->Spells[i].SpellCharges)
                if(m_CastItem->GetSpellCharges(i)==0)
                    return SPELL_FAILED_NO_CHARGES_REMAIN;

        // consumable cast item checks
        if (proto->Class == ITEM_CLASS_CONSUMABLE && m_targets.getUnitTarget())
        {
            // such items should only fail if there is no suitable effect at all - see Rejuvenation Potions for example
            SpellCastResult failReason = SPELL_CAST_OK;
            for (int i = 0; i < 3; ++i)
            {
                // skip check, pet not required like checks, and for TARGET_PET m_targets.getUnitTarget() is not the real target but the caster
                if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_PET)
                    continue;

                if (m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                {
                    if (m_targets.getUnitTarget()->GetHealth() == m_targets.getUnitTarget()->GetMaxHealth())
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_HEALTH;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }

                // Mana Potion, Rage Potion, Thistle Tea(Rogue), ...
                if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENERGIZE)
                {
                    if(m_spellInfo->EffectMiscValue[i] < 0 || m_spellInfo->EffectMiscValue[i] >= MAX_POWERS)
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }

                    Powers power = Powers(m_spellInfo->EffectMiscValue[i]);
                    if (m_targets.getUnitTarget()->GetPower(power) == m_targets.getUnitTarget()->GetMaxPower(power))
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }
            }
            if (failReason != SPELL_CAST_OK)
                return failReason;
        }
    }

    // check target item
    if(m_targets.getItemTargetGUID())
    {
        if(m_caster->GetTypeId() != TYPEID_PLAYER)
            return SPELL_FAILED_BAD_TARGETS;

        if(!m_targets.getItemTarget())
            return SPELL_FAILED_ITEM_GONE;

        if(!m_targets.getItemTarget()->IsFitToSpellRequirements(m_spellInfo))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }
    // if not item target then required item must be equipped
    else
    {
        if(m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->HasItemFitToSpellReqirements(m_spellInfo))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }

    // check spell focus object
    if(m_spellInfo->RequiresSpellFocus)
    {
        CellPair p(MaNGOS::ComputeCellPair(m_caster->GetPositionX(), m_caster->GetPositionY()));
        Cell cell(p);
        cell.data.Part.reserved = ALL_DISTRICT;

        GameObject* ok = NULL;
        MaNGOS::GameObjectFocusCheck go_check(m_caster,m_spellInfo->RequiresSpellFocus);
        MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> checker(m_caster,ok,go_check);

        TypeContainerVisitor<MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck>, GridTypeMapContainer > object_checker(checker);
        CellLock<GridReadGuard> cell_lock(cell, p);
        cell_lock->Visit(cell_lock, object_checker, *m_caster->GetMap());

        if(!ok)
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;

        focusObject = ok;                                   // game object found in range
    }

    // check reagents (ignore triggered spells with reagents processed by original spell) and special reagent ignore case.
    if (!m_IsTriggeredSpell && !p_caster->CanNoReagentCast(m_spellInfo))
    {
        for(uint32 i=0;i<8;++i)
        {
            if(m_spellInfo->Reagent[i] <= 0)
                continue;

            uint32 itemid    = m_spellInfo->Reagent[i];
            uint32 itemcount = m_spellInfo->ReagentCount[i];

            // if CastItem is also spell reagent
            if( m_CastItem && m_CastItem->GetEntry() == itemid )
            {
                ItemPrototype const *proto = m_CastItem->GetProto();
                if(!proto)
                    return SPELL_FAILED_ITEM_NOT_READY;
                for(int s=0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }
            }
            if( !p_caster->HasItemCount(itemid,itemcount) )
                return SPELL_FAILED_ITEM_NOT_READY;         //0x54
        }
    }

    // check totem-item requirements (items presence in inventory)
    uint32 totems = 2;
    for(int i=0;i<2;++i)
    {
        if(m_spellInfo->Totem[i] != 0)
        {
            if( p_caster->HasItemCount(m_spellInfo->Totem[i],1) )
            {
                totems -= 1;
                continue;
            }
        }else
        totems -= 1;
    }
    if(totems != 0)
        return SPELL_FAILED_TOTEMS;                         //0x7C

    // Check items for TotemCategory  (items presence in inventory)
    uint32 TotemCategory = 2;
    for(int i=0;i<2;++i)
    {
        if(m_spellInfo->TotemCategory[i] != 0)
        {
            if( p_caster->HasItemTotemCategory(m_spellInfo->TotemCategory[i]) )
            {
                TotemCategory -= 1;
                continue;
            }
        }
        else
            TotemCategory -= 1;
    }
    if(TotemCategory != 0)
        return SPELL_FAILED_TOTEM_CATEGORY;                 //0x7B

    // special checks for spell effects
    for(int i = 0; i < 3; ++i)
    {
        switch (m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_CREATE_ITEM:
            {
                if (!m_IsTriggeredSpell && m_spellInfo->EffectItemType[i])
                {
                    ItemPosCountVec dest;
                    uint8 msg = p_caster->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m_spellInfo->EffectItemType[i], 1 );
                    if (msg != EQUIP_ERR_OK )
                    {
                        p_caster->SendEquipError( msg, NULL, NULL );
                        return SPELL_FAILED_DONT_REPORT;
                    }
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
            {
                Item* targetItem = m_targets.getItemTarget();
                if(!targetItem)
                    return SPELL_FAILED_ITEM_NOT_FOUND;

                if( targetItem->GetProto()->ItemLevel < m_spellInfo->baseLevel )
                    return SPELL_FAILED_LOWLEVEL;
                // Not allow enchant in trade slot for some enchant type
                if( targetItem->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            {
                Item *item = m_targets.getItemTarget();
                if(!item)
                    return SPELL_FAILED_ITEM_NOT_FOUND;
                // Not allow enchant in trade slot for some enchant type
                if( item->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
                // check item existence in effect code (not output errors at offhand hold item effect to main hand for example
                break;
            case SPELL_EFFECT_DISENCHANT:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // prevent disenchanting in trade slot
                if( m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID() )
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                ItemPrototype const* itemProto = m_targets.getItemTarget()->GetProto();
                if(!itemProto)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                uint32 item_quality = itemProto->Quality;
                // 2.0.x addon: Check player enchanting level against the item disenchanting requirements
                uint32 item_disenchantskilllevel = itemProto->RequiredDisenchantSkill;
                if (item_disenchantskilllevel == uint32(-1))
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (item_disenchantskilllevel > p_caster->GetSkillValue(SKILL_ENCHANTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                if(item_quality > 4 || item_quality < 2)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if(itemProto->Class != ITEM_CLASS_WEAPON && itemProto->Class != ITEM_CLASS_ARMOR)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (!itemProto->DisenchantID)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                break;
            }
            case SPELL_EFFECT_PROSPECTING:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //ensure item is a prospectable ore
                if(!(m_targets.getItemTarget()->GetProto()->BagFamily & BAG_FAMILY_MASK_MINING_SUPP) || m_targets.getItemTarget()->GetProto()->Class != ITEM_CLASS_TRADE_GOODS)
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //prevent prospecting in trade slot
                if( m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID() )
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //Check for enough skill in jewelcrafting
                uint32 item_prospectingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if(item_prospectingskilllevel >p_caster->GetSkillValue(SKILL_JEWELCRAFTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                //make sure the player has the required ores in inventory
                if(m_targets.getItemTarget()->GetCount() < 5)
                    return SPELL_FAILED_NEED_MORE_ITEMS;

                if(!LootTemplates_Prospecting.HaveLootFor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_PROSPECTED;

                break;
            }
            case SPELL_EFFECT_MILLING:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_MILLED;
                //ensure item is a millable herb
                if(!(m_targets.getItemTarget()->GetProto()->BagFamily & BAG_FAMILY_MASK_HERBS) || m_targets.getItemTarget()->GetProto()->Class != ITEM_CLASS_TRADE_GOODS)
                    return SPELL_FAILED_CANT_BE_MILLED;
                //prevent milling in trade slot
                if( m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID() )
                    return SPELL_FAILED_CANT_BE_MILLED;
                //Check for enough skill in inscription
                uint32 item_millingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if(item_millingskilllevel >p_caster->GetSkillValue(SKILL_INSCRIPTION))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                //make sure the player has the required herbs in inventory
                if(m_targets.getItemTarget()->GetCount() < 5)
                    return SPELL_FAILED_NEED_MORE_ITEMS;

                if(!LootTemplates_Milling.HaveLootFor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_MILLED;

                break;
            }
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            {
                if(m_caster->GetTypeId() != TYPEID_PLAYER) return SPELL_FAILED_TARGET_NOT_PLAYER;
                if( m_attackType != RANGED_ATTACK )
                    break;
                Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(m_attackType);
                if(!pItem || pItem->IsBroken())
                    return SPELL_FAILED_EQUIPPED_ITEM;

                switch(pItem->GetProto()->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                    {
                        uint32 ammo = pItem->GetEntry();
                        if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_GUN:
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    {
                        uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                        if(!ammo)
                        {
                            // Requires No Ammo
                            if(m_caster->GetDummyAura(46699))
                                break;                      // skip other checks

                            return SPELL_FAILED_NO_AMMO;
                        }

                        ItemPrototype const *ammoProto = objmgr.GetItemPrototype( ammo );
                        if(!ammoProto)
                            return SPELL_FAILED_NO_AMMO;

                        if(ammoProto->Class != ITEM_CLASS_PROJECTILE)
                            return SPELL_FAILED_NO_AMMO;

                        // check ammo ws. weapon compatibility
                        switch(pItem->GetProto()->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                if(ammoProto->SubClass!=ITEM_SUBCLASS_ARROW)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                if(ammoProto->SubClass!=ITEM_SUBCLASS_BULLET)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            default:
                                return SPELL_FAILED_NO_AMMO;
                        }

                        if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_WAND:
                    default:
                        break;
                }
                break;
            }
            default:break;
        }
    }

    return SPELL_CAST_OK;
}

void Spell::Delayed()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (m_spellState == SPELL_STATE_DELAYED)
        return;                                             // spell is active and can't be time-backed

    if(isDelayableNoMore())                                 // Spells may only be delayed twice
        return;

    // spells not loosing casting time ( slam, dynamites, bombs.. )
    if(!(m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE))
        return;

    //check pushback reduce
    int32 delaytime = 500;                                  // spellcasting delay is normally 500ms
    int32 delayReduce = 100;                                // must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id,SPELLMOD_NOT_LOSE_CASTING_TIME, delayReduce, this);
    delayReduce += m_caster->GetTotalAuraModifier(SPELL_AURA_REDUCE_PUSHBACK) - 100;
    if(delayReduce >= 100)
        return;

    delaytime = delaytime * (100 - delayReduce) / 100;

    if(int32(m_timer) + delaytime > m_casttime)
    {
        delaytime = m_casttime - m_timer;
        m_timer = m_casttime;
    }
    else
        m_timer += delaytime;

    sLog.outDetail("Spell %u partially interrupted for (%d) ms at damage",m_spellInfo->Id,delaytime);

    WorldPacket data(SMSG_SPELL_DELAYED, 8+4);
    data.append(m_caster->GetPackGUID());
    data << uint32(delaytime);

    m_caster->SendMessageToSet(&data,true);
}

void Spell::DelayedChannel()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER || getState() != SPELL_STATE_CASTING)
        return;

    if(isDelayableNoMore())                                 // Spells may only be delayed twice
        return;

    //check pushback reduce
    int32 delaytime = GetSpellDuration(m_spellInfo) * 25 / 100; // channeling delay is normally 25% of its time per hit
    int32 delayReduce = 100;                               // must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id,SPELLMOD_NOT_LOSE_CASTING_TIME,delayReduce, this);
    delayReduce += m_caster->GetTotalAuraModifier(SPELL_AURA_REDUCE_PUSHBACK) - 100;
    if(delayReduce >= 100)
        return;

    delaytime = delaytime * (100 - delayReduce) / 100;

    if(int32(m_timer) < delaytime)
    {
        delaytime = m_timer;
        m_timer = 0;
    }
    else
        m_timer -= delaytime;

    sLog.outDebug("Spell %u partially interrupted for %i ms, new duration: %u ms", m_spellInfo->Id, delaytime, m_timer);

    for(std::list<TargetInfo>::const_iterator ihit= m_UniqueTargetInfo.begin();ihit != m_UniqueTargetInfo.end();++ihit)
    {
        if ((*ihit).missCondition == SPELL_MISS_NONE)
        {
            Unit* unit = m_caster->GetGUID()==ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
            if (unit)
            {
                for (int j=0;j<3;j++)
                    if( ihit->effectMask & (1<<j) )
                        unit->DelayAura(m_spellInfo->Id, j, delaytime);
            }

        }
    }

    for(int j = 0; j < 3; j++)
    {
        // partially interrupt persistent area auras
        DynamicObject* dynObj = m_caster->GetDynObject(m_spellInfo->Id, j);
        if(dynObj)
            dynObj->Delay(delaytime);
    }

    SendChannelUpdate(m_timer);
}

void Spell::UpdatePointers()
{
    if(m_originalCasterGUID==m_caster->GetGUID())
        m_originalCaster = m_caster;
    else
    {
        m_originalCaster = ObjectAccessor::GetUnit(*m_caster,m_originalCasterGUID);
        if(m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }

    m_targets.Update(m_caster);
}

bool Spell::IsAffectedByAura(Aura *aura)
{
    return spellmgr.IsAffectedByMod(m_spellInfo, aura->getAuraSpellMod());
}

bool Spell::CheckTargetCreatureType(Unit* target) const
{
    uint32 spellCreatureTargetMask = m_spellInfo->TargetCreatureType;

    // Curse of Doom : not find another way to fix spell target check :/
    if(m_spellInfo->SpellFamilyName==SPELLFAMILY_WARLOCK && m_spellInfo->SpellFamilyFlags == 0x0200000000LL)
    {
        // not allow cast at player
        if(target->GetTypeId()==TYPEID_PLAYER)
            return false;

        spellCreatureTargetMask = 0x7FF;
    }

    // Dismiss Pet and Taming Lesson skipped
    if(m_spellInfo->Id == 2641 || m_spellInfo->Id == 23356)
        spellCreatureTargetMask =  0;

    if (spellCreatureTargetMask)
    {
        uint32 TargetCreatureType = target->GetCreatureTypeMask();

        return !TargetCreatureType || (spellCreatureTargetMask & TargetCreatureType);
    }
    return true;
}

CurrentSpellTypes Spell::GetCurrentContainer()
{
    if (IsNextMeleeSwingSpell())
        return(CURRENT_MELEE_SPELL);
    else if (IsAutoRepeat())
        return(CURRENT_AUTOREPEAT_SPELL);
    else if (IsChanneledSpell(m_spellInfo))
        return(CURRENT_CHANNELED_SPELL);
    else
        return(CURRENT_GENERIC_SPELL);
}

bool Spell::CheckTarget( Unit* target, uint32 eff )
{
    // Check targets for creature type mask and remove not appropriate (skip explicit self target case, maybe need other explicit targets)
    if(m_spellInfo->EffectImplicitTargetA[eff]!=TARGET_SELF )
    {
        if (!CheckTargetCreatureType(target))
            return false;
    }

    // Check Aura spell req (need for AoE spells)
    if(m_spellInfo->targetAuraSpell && !target->HasAura(m_spellInfo->targetAuraSpell))
        return false;
    if (m_spellInfo->excludeTargetAuraSpell && target->HasAura(m_spellInfo->excludeTargetAuraSpell))
        return false;

    // Check targets for not_selectable unit flag and remove
    // A player can cast spells on his pet (or other controlled unit) though in any state
    if (target != m_caster && target->GetCharmerOrOwnerGUID() != m_caster->GetGUID())
    {
        // any unattackable target skipped
        if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE))
            return false;

        // unselectable targets skipped in all cases except TARGET_SCRIPT targeting
        // in case TARGET_SCRIPT target selected by server always and can't be cheated
        if( target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE) &&
            m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT &&
            m_spellInfo->EffectImplicitTargetB[eff] != TARGET_SCRIPT )
            return false;
    }

    //Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if( target != m_caster && target->GetTypeId()==TYPEID_PLAYER)
    {
        if(((Player*)target)->GetVisibility()==VISIBILITY_OFF)
            return false;

        if(((Player*)target)->isGameMaster() && !IsPositiveSpell(m_spellInfo->Id))
            return false;
    }

    //Check targets for LOS visibility (except spells without range limitations )
    switch(m_spellInfo->Effect[eff])
    {
        case SPELL_EFFECT_SUMMON_PLAYER:                    // from anywhere
            break;
        case SPELL_EFFECT_DUMMY:
            if(m_spellInfo->Id!=20577)                      // Cannibalize
                break;
            //fall through
        case SPELL_EFFECT_RESURRECT_NEW:
            // player far away, maybe his corpse near?
            if(target!=m_caster && !target->IsWithinLOSInMap(m_caster))
            {
                if(!m_targets.getCorpseTargetGUID())
                    return false;

                Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster,m_targets.getCorpseTargetGUID());
                if(!corpse)
                    return false;

                if(target->GetGUID()!=corpse->GetOwnerGUID())
                    return false;

                if(!corpse->IsWithinLOSInMap(m_caster))
                    return false;
            }

            // all ok by some way or another, skip normal check
            break;
        default:                                            // normal case
            // Get GO cast coordinates if original caster -> GO
            WorldObject *caster = NULL;
            if (IS_GAMEOBJECT_GUID(m_originalCasterGUID))
                caster = m_caster->GetMap()->GetGameObject(m_originalCasterGUID);
            if (!caster)
                caster = m_caster;
            if(target!=m_caster && !target->IsWithinLOSInMap(caster))
                return false;
            break;
    }

    return true;
}

bool Spell::IsNeedSendToClient() const
{
    return m_spellInfo->SpellVisual[0] || m_spellInfo->SpellVisual[1] || IsChanneledSpell(m_spellInfo) ||
        m_spellInfo->speed > 0.0f || !m_triggeredByAuraSpell && !m_IsTriggeredSpell;
}

bool Spell::HaveTargetsForEffect( uint8 effect ) const
{
    for(std::list<TargetInfo>::const_iterator itr= m_UniqueTargetInfo.begin();itr != m_UniqueTargetInfo.end();++itr)
        if(itr->effectMask & (1<<effect))
            return true;

    for(std::list<GOTargetInfo>::const_iterator itr= m_UniqueGOTargetInfo.begin();itr != m_UniqueGOTargetInfo.end();++itr)
        if(itr->effectMask & (1<<effect))
            return true;

    for(std::list<ItemTargetInfo>::const_iterator itr= m_UniqueItemInfo.begin();itr != m_UniqueItemInfo.end();++itr)
        if(itr->effectMask & (1<<effect))
            return true;

    return false;
}

SpellEvent::SpellEvent(Spell* spell) : BasicEvent()
{
    m_Spell = spell;
}

SpellEvent::~SpellEvent()
{
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();

    if (m_Spell->IsDeletable())
    {
        delete m_Spell;
    }
    else
    {
        sLog.outError("~SpellEvent: %s %u tried to delete non-deletable spell %u. Was not deleted, causes memory leak.",
            (m_Spell->GetCaster()->GetTypeId()==TYPEID_PLAYER?"Player":"Creature"), m_Spell->GetCaster()->GetGUIDLow(),m_Spell->m_spellInfo->Id);
    }
}

bool SpellEvent::Execute(uint64 e_time, uint32 p_time)
{
    // update spell if it is not finished
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->update(p_time);

    // check spell state to process
    switch (m_Spell->getState())
    {
        case SPELL_STATE_FINISHED:
        {
            // spell was finished, check deletable state
            if (m_Spell->IsDeletable())
            {
                // check, if we do have unfinished triggered spells

                return(true);                               // spell is deletable, finish event
            }
            // event will be re-added automatically at the end of routine)
        } break;

        case SPELL_STATE_CASTING:
        {
            // this spell is in channeled state, process it on the next update
            // event will be re-added automatically at the end of routine)
        } break;

        case SPELL_STATE_DELAYED:
        {
            // first, check, if we have just started
            if (m_Spell->GetDelayStart() != 0)
            {
                // no, we aren't, do the typical update
                // check, if we have channeled spell on our hands
                if (IsChanneledSpell(m_Spell->m_spellInfo))
                {
                    // evented channeled spell is processed separately, casted once after delay, and not destroyed till finish
                    // check, if we have casting anything else except this channeled spell and autorepeat
                    if (m_Spell->GetCaster()->IsNonMeleeSpellCasted(false, true, true))
                    {
                        // another non-melee non-delayed spell is casted now, abort
                        m_Spell->cancel();
                    }
                    else
                    {
                        // do the action (pass spell to channeling state)
                        m_Spell->handle_immediate();
                    }
                    // event will be re-added automatically at the end of routine)
                }
                else
                {
                    // run the spell handler and think about what we can do next
                    uint64 t_offset = e_time - m_Spell->GetDelayStart();
                    uint64 n_offset = m_Spell->handle_delayed(t_offset);
                    if (n_offset)
                    {
                        // re-add us to the queue
                        m_Spell->GetCaster()->m_Events.AddEvent(this, m_Spell->GetDelayStart() + n_offset, false);
                        return(false);                      // event not complete
                    }
                    // event complete
                    // finish update event will be re-added automatically at the end of routine)
                }
            }
            else
            {
                // delaying had just started, record the moment
                m_Spell->SetDelayStart(e_time);
                // re-plan the event for the delay moment
                m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + m_Spell->GetDelayMoment(), false);
                return(false);                              // event not complete
            }
        } break;

        default:
        {
            // all other states
            // event will be re-added automatically at the end of routine)
        } break;
    }

    // spell processing not complete, plan event on the next update interval
    m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + 1, false);
    return(false);                                          // event not complete
}

void SpellEvent::Abort(uint64 /*e_time*/)
{
    // oops, the spell we try to do is aborted
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();
}

bool SpellEvent::IsDeletable() const
{
    return m_Spell->IsDeletable();
}

SpellCastResult Spell::CanOpenLock(uint32 effIndex, uint32 lockId, SkillType& skillId, int32& reqSkillValue, int32& skillValue)
{
    if(!lockId)                                             // possible case for GO and maybe for items.
        return SPELL_CAST_OK;

    // Get LockInfo
    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

    if (!lockInfo)
        return SPELL_FAILED_BAD_TARGETS;

    bool reqKey = false;                                    // some locks not have reqs

    for(int j = 0; j < 8; ++j)
    {
        switch(lockInfo->Type[j])
        {
            // check key item (many fit cases can be)
            case LOCK_KEY_ITEM:
                if(lockInfo->Index[j] && m_CastItem && m_CastItem->GetEntry()==lockInfo->Index[j])
                    return SPELL_CAST_OK;
                reqKey = true;
                break;
                // check key skill (only single first fit case can be)
            case LOCK_KEY_SKILL:
            {
                reqKey = true;

                // wrong locktype, skip
                if(uint32(m_spellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                    continue;

                skillId = SkillByLockType(LockType(lockInfo->Index[j]));

                if ( skillId != SKILL_NONE )
                {
                    // skill bonus provided by casting spell (mostly item spells)
                    // add the damage modifier from the spell casted (cheat lock / skeleton key etc.) (use m_currentBasePoints, CalculateDamage returns wrong value)
                    uint32 spellSkillBonus = uint32(m_currentBasePoints[effIndex]+1);
                    reqSkillValue = lockInfo->Skill[j];

                    // castitem check: rogue using skeleton keys. the skill values should not be added in this case.
                    skillValue = m_CastItem || m_caster->GetTypeId()!= TYPEID_PLAYER ?
                        0 : ((Player*)m_caster)->GetSkillValue(skillId);

                    skillValue += spellSkillBonus;

                    if (skillValue < reqSkillValue)
                        return SPELL_FAILED_LOW_CASTLEVEL;
                }

                return SPELL_CAST_OK;
            }
        }
    }

    if(reqKey)
        return SPELL_FAILED_BAD_TARGETS;

    return SPELL_CAST_OK;
}

void Spell::FillAreaTargets( UnitList& TagUnitMap, float x, float y, float radius, SpellNotifyPushType pushType, SpellTargets spellTargets )
{
    CellPair p(MaNGOS::ComputeCellPair(x, y));
    Cell cell(p);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();
    MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, TagUnitMap, radius, pushType, spellTargets);
    TypeContainerVisitor<MaNGOS::SpellNotifierCreatureAndPlayer, WorldTypeMapContainer > world_notifier(notifier);
    TypeContainerVisitor<MaNGOS::SpellNotifierCreatureAndPlayer, GridTypeMapContainer > grid_notifier(notifier);
    CellLock<GridReadGuard> cell_lock(cell, p);
    cell_lock->Visit(cell_lock, world_notifier, *m_caster->GetMap());
    cell_lock->Visit(cell_lock, grid_notifier, *m_caster->GetMap());
}