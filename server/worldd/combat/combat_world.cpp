// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world.cpp (see combat_world.h)
// ============================================================================
#include "combat/combat_world.h"

#include "combat/npc_kits.h"
#include "combat/npc_replicator.h"
#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "spawns/generic_npc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

class EffectHost final : public ContextHost {
public:
    explicit EffectHost(std::function<double(int, const std::string&)> query) : query_(std::move(query)) {}
protected:
    double other_fn(int fn, const std::string& name, const double*, int) override { return query_(fn, name); }
private:
    std::function<double(int, const std::string&)> query_;
};

/// The kit of a creature kind: by the Blueprint's leaf without "_C"; else the kit whose name is the longest prefix of the leaf (or the leaf of it).
const npc_kits::Kit* kit_of(const std::string& leaf_in) {
    std::string leaf = leaf_in;
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    const npc_kits::Kit* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < npc_kits::kKitCount; ++i) {
        const npc_kits::Kit& k = npc_kits::kKits[i];
        const std::string name = k.blueprint;
        if (name == leaf) return &k;
        const bool related = leaf.rfind(name + "_", 0) == 0 || name.rfind(leaf + "_", 0) == 0;
        if (related && name.size() > best_len) {
            best = &k;
            best_len = name.size();
        }
    }
    return best;
}

}  // namespace
// [CoA ~ C:Y:B]

// ---- one NPC's combat state ----------------------------------------------------------------------------------------------------------

class CombatWorld::Npc final : public Fighter {
public:
    Npc(CombatWorld& w, uint64_t actor, npc::MotionStats bp_in, std::string leaf_in, int level_in)
        : world(w), actor_id(actor), bp(std::move(bp_in)), leaf(std::move(leaf_in)), lvl(level_in) {}

    uint64_t id() const override { return actor_id; }
    bool     is_player() const override { return false; }
    int      level() const override { return lvl; }
    Vec3     position() const override {
        const world::WorldActor* a = world.registry_.find(actor_id);
        return a ? Vec3{a->x, a->y, a->z} : Vec3{};
    }
    double radius_cm() const override { return bp.radius; }
    bool   alive() const override { return !dead; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats&      stats() override { return *cs; }

    double health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidHealth) : nullptr;
        return v ? v->value : 0.0;
    }
    double max_health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidMaxHealth) : nullptr;
        return v ? v->value : 0.0;
    }

    CombatWorld&                           world;
    uint64_t                               actor_id;
    npc::MotionStats                       bp;
    std::string                            leaf;
    int                                    lvl;
    std::vector<int64_t>                   aligns;
    std::unique_ptr<stats::CharacterStats> cs;
    npc::ai::NpcAi                         ai;
    bool                                   hostile = false;
    bool                                   dead = false;
    bool                                   flags_in_combat = false;
    uint64_t                               flags_target = 0;
    bool                                   returning = false;
    const npc_kits::Kit*                   kit = nullptr;
    CombatWorld::Attack                    next;
    double                                 next_attack_at = 0;
    uint64_t                               killer = 0;
    std::set<uint64_t>                     attackers;
    struct Shield {
        double amount = 0;
        double until = 0;   // 0: until used up
    };
    std::vector<Shield>                    shields;
    struct Effect {
        int64_t key = 0;
        const abilities::EffectDef* def = nullptr;
        uint64_t instigator = 0;
        int stacks = 1;
        double start = 0, end = 0;
        std::vector<stats::EffectMod> mods;
        std::optional<int32_t> array_id;
    };
    std::vector<Effect> effects;
    int64_t next_effect_key = 0;

    /// A loss goes through the shields first (the oldest first); what is left reaches Health.
    double absorb(double loss, double now) {
        shields.erase(std::remove_if(shields.begin(), shields.end(), [&](const Shield& s) { return s.amount <= 0 || (s.until > 0 && s.until <= now); }), shields.end());
        for (Shield& s : shields) {
            const double take = std::min(s.amount, loss);
            s.amount -= take;
            loss -= take;
        }
        return loss;
    }
};

CombatWorld::CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                         const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config)
    : registry_(registry), npcs_world_(npcs), nav_(nav), data_(data), catalog_(catalog), book_(book), motion_(motion), cfg_(std::move(config)),
      resolver_(catalog, data, cfg_.seed ? cfg_.seed : 1) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 7;
}

CombatWorld::~CombatWorld() = default;

// ---- players ----------------------------------------------------------------------------------------------------------------------

void CombatWorld::add_player(PlayerPort* port) {
    if (port && std::find(players_.begin(), players_.end(), port) == players_.end()) players_.push_back(port);
}

void CombatWorld::remove_player(PlayerPort* port) {
    players_.erase(std::remove(players_.begin(), players_.end(), port), players_.end());
    if (port) player_combat_until_.erase(port->actor_id());
}

PlayerPort* CombatWorld::player(uint64_t actor_id) {
    for (PlayerPort* p : players_) {
        if (p->actor_id() == actor_id) return p;
    }
    return nullptr;
}

void CombatWorld::player_exchange(uint64_t player_id, double now) {
    const double until = now + data_.in_combat_timer();
    const auto it = player_combat_until_.find(player_id);
    const bool was = it != player_combat_until_.end() && it->second > now;
    player_combat_until_[player_id] = until;
    if (!was) {
        if (PlayerPort* p = player(player_id)) p->set_in_combat(true, now);
    }
}

// ---- combatants -----------------------------------------------------------------------------------------------------------------------

CombatWorld::Npc* CombatWorld::make_combatant(const world::WorldActor& a) {
    if (a.kind != world::ActorKind::Npc) return nullptr;
    const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    auto n = std::make_unique<Npc>(*this, a.id, bp, leaf, a.npc.level);
    std::string err;
    n->cs = book_.make(bp, a.npc.level, err);
    if (!n->cs) {
        spdlog::warn("[combat] {} ({}): its stats do not run: {}", a.id, leaf, err);
        return nullptr;
    }
    n->aligns.assign(bp.alignments.begin(), bp.alignments.end());
    n->hostile = data_.relation(n->aligns, CombatData::player_alignments()) == Relation::Hostile;
    n->kit = kit_of(leaf);
    npc::ai::Params p;
    p.aggro_radius_cm = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : cfg_.default_aggro_cm;
    // who may start a fight by noticing a player: a creature hostile to players whose data does not switch its aggro off (retest 3: a Timid Grem, whose instance overrides the radius with 0, and a
    // huntable, whose alignment is Friendly, never attack first)
    p.proximity_aggro = notices_players(n->hostile, a.npc);
    p.leash_radius_cm = a.npc.leash_radius_cm > 0 ? a.npc.leash_radius_cm : std::max(cfg_.default_leash_cm, p.aggro_radius_cm * 3.0);
    const double run = std::max({bp.max_run_speed, bp.max_walk_speed, 300.0});
    p.chase_speed_cm_s = run;
    p.run_home_speed_cm_s = run;
    n->ai.set_params(p);
    n->next = choose_attack(*n);
    // the client sees the creature's own numbers from now on
    write_stats(registry_, a.id, *n->cs);
    write_elite_rank(registry_, a.id, static_cast<int>(n->cs->npc().elite_rank));
    Npc* raw = n.get();
    npcs_[a.id] = std::move(n);
    return raw;
}
// (C_Y_B | C.O.A)

CombatWorld::Npc* CombatWorld::combatant(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) return it->second.get();
    const world::WorldActor* a = registry_.find(actor_id);
    if (!a || a->kind != world::ActorKind::Npc) return nullptr;
    return make_combatant(*a);
}

Fighter* CombatWorld::fighter_of(uint64_t actor_id) {
    if (PlayerPort* p = player(actor_id)) return &p->fighter();
    return combatant(actor_id);
}

uint64_t CombatWorld::actor_of_object(uint64_t object_id) const {
    if (registry_.find(object_id)) return object_id;
    for (const PlayerPort* p : players_) if (p->actor_id() == object_id || p->hit_component_guid().ObjectId == object_id) return p->actor_id();
    // the components the registry indexes: the cylinder (what the client names once the server exported it) and the interactable
    // [E1 stage 3 fix 3: no arithmetic on the id; the draft's "actor + 24" was another actor's component]
    if (const world::WorldActor* a = registry_.find_by_component(object_id)) return a->id;
    return 0;
}

uint64_t CombatWorld::hit_component_of(uint64_t actor_id) const {
    for (const PlayerPort* p : players_) if (p->actor_id() == actor_id) return p->hit_component_guid().ObjectId;
    const world::WorldActor* a = registry_.find(actor_id);
    return a ? a->npc.collision_cylinder : 0;
}

TargetInfo CombatWorld::inspect(uint64_t target_actor, const Fighter& from) {
    TargetInfo t;
    if (PlayerPort* p = player(target_actor)) {
        const Fighter& f = p->fighter();
        t.exists = t.is_player = true;
        t.alive = f.alive();
        t.position = f.position();
        t.radius_cm = f.radius_cm();
        t.relation = data_.relation(from.alignments(), f.alignments());
        t.surface_distance_cm = std::max(0.0, distance_xy_cm(from.position(), t.position) - from.radius_cm() - t.radius_cm);
        if (nav_) {
            const nav::Vec3 a = to_nav(from.position()), b = to_nav(t.position);
            if (nav_->on_navmesh(a, 150, 300) && nav_->on_navmesh(b, 150, 300)) t.in_line_of_sight = nav_->line_of_sight(a, b);
        }
        return t;
    }
    const world::WorldActor* a = registry_.find(target_actor);
    if (!a || a->kind != world::ActorKind::Npc) return t;
    t.exists = true;
    t.position = {a->x, a->y, a->z};
    const npc::MotionStats bp = motion_.get(a->npc.archetype);
    t.radius_cm = bp.radius;
    const auto it = npcs_.find(target_actor);
    t.alive = it == npcs_.end() ? true : !it->second->dead;
    std::vector<int64_t> aligns(bp.alignments.begin(), bp.alignments.end());
    t.relation = data_.relation(from.alignments(), aligns);
    const Vec3 me = from.position();
    t.surface_distance_cm = std::max(0.0, distance_xy_cm(me, t.position) - from.radius_cm() - t.radius_cm);
    t.in_line_of_sight = true;
    if (nav_) {
        const nav::Vec3 p = to_nav(me), q = to_nav(t.position);
        if (nav_->on_navmesh(p, 150, 300) && nav_->on_navmesh(q, 150, 300)) t.in_line_of_sight = nav_->line_of_sight(p, q);
    }
    return t;
}

std::optional<npc::ai::State> CombatWorld::ai_state(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return std::nullopt;
    return it->second->ai.state();
}

const npc::ai::NpcAi* CombatWorld::ai_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? nullptr : &it->second->ai;
}

double CombatWorld::health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->health();
}

double CombatWorld::max_health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->max_health();
}

bool CombatWorld::dead(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it != npcs_.end() && it->second->dead;
}

std::string CombatWorld::next_attack_name(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end() || !it->second->next.ability) return {};
    return it->second->next.ability->row.name;
}

void CombatWorld::forget(uint64_t actor_id) {
    npcs_.erase(actor_id);
    huntable_stats_.erase(actor_id);
}

void CombatWorld::fill_huntable_stats(const world::WorldActor& a) {
    if (!huntable_stats_.insert(a.id).second) return;
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    std::string err;
    const auto cs = book_.make(bp, a.npc.level, err);
    if (!cs) {
        spdlog::warn("[combat] {} ({}): a huntable whose stats do not run keeps its template's: {}", a.id, spawns::blueprint_leaf(a.npc.archetype), err);
        return;
    }
    write_stats(registry_, a.id, *cs);
}

void CombatWorld::add_shield(uint64_t npc_actor, double amount, double seconds, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead || !std::isfinite(amount) || amount <= 0) return;
    if (n->shields.size() >= 16) n->shields.erase(n->shields.begin());
    n->shields.push_back({amount, seconds > 0 ? now + seconds : 0.0});
}

double CombatWorld::shield_of(uint64_t npc_actor, double now) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0;
    double total = 0;
    for (const Npc::Shield& s : it->second->shields) {
        if (s.amount > 0 && (s.until <= 0 || s.until > now)) total += s.amount;
    }
    return total;
}

void CombatWorld::add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return;
    n->ai.notice(attacker, amount, now);
    n->attackers.insert(attacker);
}

// ---- attacks ----------------------------------------------------------------------------------------------------------------------

namespace {
// C_Y_B.C.O.A

/// Whether an ability does harm to enemies: some hit of it is aimed at enemies and moves Health.
bool is_attack(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_attack(Npc& n) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (n.kit) {
        struct Cand {
            const abilities::AbilityDef* def;
            const npc_kits::KitAbility*  k;
        };
        std::vector<Cand> cands;
        double total = 0;
        for (size_t i = 0; i < n.kit->count; ++i) {
            const npc_kits::KitAbility& k = n.kit->abilities[i];
            const abilities::AbilityDef* d = catalog_.ability_named(k.name);
            if (!d || !is_attack(*d, catalog_, data_.ids().health)) continue;
            cands.push_back({d, &k});
            total += std::max(1, k.uses);
        }
        if (!cands.empty()) {
            double r = rng_.rand_double() * total;
            for (const Cand& c : cands) {
                r -= std::max(1, c.k->uses);
                if (r <= 0 || &c == &cands.back()) {
                    a.ability = c.def;
                    a.windup_ms = c.k->windup_ms;
                    a.range_cm = c.k->range_cm;
                    return a;
                }
            }
        }
    }
    if (const abilities::AbilityDef* d = catalog_.ability_named(cfg_.fallback_ability)) {
        if (is_attack(*d, catalog_, data_.ids().health)) a.ability = d;
    }
    return a;
}

void CombatWorld::start_attack(Npc& n, uint64_t target, double now) {
    const Attack a = n.next;
    const double interval = (n.kit && n.kit->interval_ms > 0 ? n.kit->interval_ms / 1000.0 : cfg_.default_attack_interval);
    n.next_attack_at = now + interval * (1.0 + cfg_.attack_variance * (2.0 * rng_.rand_double() - 1.0));
    n.next = choose_attack(n);
    if (!a.ability) {
        ++stats_.refused_no_attack;
        return;
    }
    Pending p;
    p.due = now + a.windup_ms / 1000.0;
    p.npc = n.actor_id;
    p.target = target;
    p.ability = a.ability;
    std::set<int64_t> seen;
    for (const content::AbilityHitRow& h : a.ability->hits) {
        if ((h.source == "hit" || h.source == "projectile") && seen.insert(h.hit_guid).second) p.hits.push_back(h.hit_guid);
    }
    pending_.push_back(std::move(p));
    ++stats_.npc_attacks;
}

void CombatWorld::resolve_pending(double now) {
    for (size_t i = 0; i < pending_.size();) {
        Pending p = pending_[i];
        if (p.due > now) {
            ++i;
            continue;
        }
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        const auto it = npcs_.find(p.npc);
        PlayerPort* port = player(p.target);
        if (it == npcs_.end() || it->second->dead || !port || !port->fighter().alive()) continue;
        Npc& n = *it->second;
        // the target must still be near the swing (it may have run out of range during the windup)
        const double reach = static_cast<double>(n.next.range_cm) * 1.5 + 300.0;
        const double d = distance_xy_cm(n.position(), port->fighter().position()) - n.radius_cm() - port->fighter().radius_cm();
        if (d > std::max(reach, 600.0)) continue;
        for (const int64_t guid : p.hits) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
            HitMeta meta;
            meta.source_type = content::kSourceTypeAbility;
            meta.source_guid = p.ability->row.guid;
            meta.ability_tags = p.ability->row.tags;
            Resolved r;
            std::string why;
            if (!resolver_.resolve(n, port->fighter(), *hit, meta, r, why)) {
                spdlog::debug("[combat] {} ({}): {}", n.actor_id, n.leaf, why);
                continue;
            }
            IncomingHit in;
            in.instigator = n.actor_id;
            in.hit_guid = guid;
            in.source_type = meta.source_type;
            in.source_guid = meta.source_guid;
            in.flags = r.flags;
            in.mods = r.mods;
            in.at = now;
            r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
            port->receive_hit(in, now);
            player_exchange(port->actor_id(), now);
        }
    }
}

// ---- hits from a player -------------------------------------------------------------------------------------------------------------
// -- C.Y.B & CoA --

Delivery CombatWorld::apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& r, double now) {
    Delivery d;
    d.target_object = hit_component_of(npc_actor);
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return d;
    d.applied = true;
    d.flags = r.flags;
    d.effective.assign(r.mods.size(), 0.0);
    n->attackers.insert(attacker);
    ++stats_.player_hits;
    if (!r.miss()) {
        const CombatStatIds& id = data_.ids();
        for (size_t i = 0; i < r.mods.size(); ++i) {
            const ResolvedMod& m = r.mods[i];
            if ((m.stat_guid != id.health && m.stat_guid != id.mana) || !std::isfinite(m.value)) continue;
            const stats::StatValue* cur = n->cs->find(m.stat_guid);
            const stats::StatValue* max = n->cs->find(m.stat_guid == id.health ? id.max_health : id.max_mana);
            if (!cur || !max) continue;
            const double before = cur->value;
            double value = m.value;
            if (m.stat_guid == id.health && value < 0) value = -n->absorb(-value, now);   // the shields take it first
            const double after = std::clamp(before + value, 0.0, static_cast<double>(max->value));
            d.effective[i] = after - before;
            n->cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, npc_actor, m.stat_guid, static_cast<float>(after));
        }
    }
    const bool lethal = n->health() <= 0.0;
    // the threat: what the hit says, else what it took
    double threat = r.threat;
    if (threat <= 0 && !r.miss()) {
        for (const double e : d.effective) threat += e < 0 ? -e : 0;
    }
    if (!lethal) n->ai.notice(attacker, std::max(threat, 1.0), now);
    player_exchange(attacker, now);
    if (lethal) {
        d.lethal = true;
        d.flags |= stats::kHitLethal;
        kill(*n, attacker, now);
    } else {
        think(*n, now);   // the fight starts in this step: the flags go up with the first exchange
        shocked_reaction(*n, attacker, r, now);
    }
    return d;
}

Delivery CombatWorld::apply_hit(uint64_t target_actor, uint64_t attacker, const Resolved& r, double now) {
    if (PlayerPort* p = player(target_actor)) {
        Delivery d;
        if (!cfg_.enabled || !p->fighter().alive()) return d;
        IncomingHit in;
        in.instigator = attacker;
        in.hit_guid = r.hit_guid;
        in.source_type = r.source_type;
        in.source_guid = r.source_guid;
        in.flags = r.flags;
        in.mods = r.mods;
        in.at = now;
        if (!p->queue_hit(in, now)) return d;
        d.applied = d.deferred_confirmation = true;
        d.flags = r.flags;
        return d;
    }
    return apply_hit_to_npc(target_actor, attacker, r, now);
}

void CombatWorld::sync_effects(Npc& n) {
    std::vector<stats::ActiveEffect> active;
    for (Npc::Effect& e : n.effects) {
        auto tags = e.def->row.tags;
        tags.insert(tags.end(), e.def->row.effect_tags.begin(), e.def->row.effect_tags.end());
        active.push_back({{stats::kRefTypeEffect, e.def->row.guid}, e.stacks, std::move(tags), e.mods});
        bootstrap::EffectItem item;
        item.effect_id = e.key;
        item.effect_guid = e.def->row.guid;
        item.start_time = bootstrap::effect_clock(e.start);
        item.duration = std::isinf(e.end) ? std::numeric_limits<float>::max() : static_cast<float>(e.end - e.start);
        item.tick_time = static_cast<float>(e.def->row.tick_timer);
        item.stacks = e.stacks;
        PlayerPort* source = player(e.instigator);
        e.array_id = write_effect(registry_, n.actor_id, e.array_id, source ? source->character_guid() : std::array<uint32_t, 4>{}, item);
    }
    n.cs->set_effects(std::move(active));
    std::vector<stats::StatValue> changed;
    std::string error;
    if (n.cs->update({}, changed, error)) write_stats(registry_, n.actor_id, *n.cs);
}

void CombatWorld::remove_effect(Npc& n, size_t index) {
    if (n.effects[index].array_id) erase_effect(registry_, n.actor_id, *n.effects[index].array_id);
    n.effects.erase(n.effects.begin() + static_cast<std::ptrdiff_t>(index));
    sync_effects(n);
}

void CombatWorld::clear_effects(Npc& n) {
    for (const Npc::Effect& e : n.effects) if (e.array_id) erase_effect(registry_, n.actor_id, *e.array_id);
    n.effects.clear();
    sync_effects(n);
}

int CombatWorld::effect_count(uint64_t target_actor, int64_t effect_guid) {
    Fighter* f = fighter_of(target_actor);
    int count = 0;
    if (f) for (const auto& e : f->stats().effects()) if (e.effect.guid == effect_guid) count += e.stacks;
    return count;
}

bool CombatWorld::remove_effects_by_tag(uint64_t target_actor, const std::string& tag, int limit, double now) {
    if (tag.empty() || limit <= 0 || !std::isfinite(now)) return false;
    if (PlayerPort* p = player(target_actor)) return p->receive_effect_removal(tag, limit, now);
    Npc* n = combatant(target_actor);
    if (!n || n->dead) return false;
    int removed = 0;
    for (size_t i = 0; i < n->effects.size() && removed < limit;) {
        const auto& row = n->effects[i].def->row;
        const auto matches = [&](const auto& tags) { return std::any_of(tags.begin(), tags.end(), [&](const std::string& t) { return tag_matches(t, tag); }); };
        if (matches(row.tags) || matches(row.effect_tags)) {
            remove_effect(*n, i);
            ++removed;
        } else ++i;
    }
    return removed > 0;
}

bool CombatWorld::apply_effect(uint64_t target_actor, uint64_t instigator, int64_t effect_guid, int stacks,
                               std::optional<double> duration, double now) {
    if (!cfg_.enabled || stacks < 1 || !std::isfinite(now)) return false;
    const abilities::EffectDef* def = catalog_.effect(effect_guid);
    if (!def) return false;
    Fighter* source = fighter_of(instigator);
    if (PlayerPort* p = player(target_actor)) {
        if (!p->fighter().alive() || !source || !source->alive()) return false;
        std::string error;
        auto evaluation = source->stats().evaluation(source->worn(), error, true);
        if (!evaluation) return false;
        auto snapshot = std::make_shared<EffectSource>();
        snapshot->evaluation = std::move(evaluation);
        if (PlayerPort* from = player(instigator)) {
            snapshot->actor = from->actor_guid();
            snapshot->character_guid = from->character_guid();
        }
        else if (const world::WorldActor* actor = registry_.find(instigator)) snapshot->actor = actor->actor.guid;
        return p->receive_effect_from(effect_guid, stacks, duration, instigator, std::move(snapshot), now);
    }
    Npc* n = combatant(target_actor);
    if (!n || n->dead || !source || !source->alive()) return false;
    std::string error;
    auto a = source->stats().evaluation(source->worn(), error, true);
    auto target = n->cs->evaluation({}, error, true);
    if (!a || !target || !a->set_actor(stats::kRoleTarget, *target, error)) return false;
    EffectHost host([&](int fn, const std::string& name) {
        if (fn == stats::role_fn(stats::kRoleTarget, stats::kRoleOpEffectRemaining)) {
            const int64_t guid = std::strtoll(name.c_str(), nullptr, 10);
            for (const auto& e : n->effects) if (e.def->row.guid == guid) return std::max(0.0, e.end - now);
        }
        return 0.0;
    });
    const auto eval = [&](int64_t expression, double& value) {
        const int program = catalog_.program(expression);
        return program >= 0 && a->eval(program, host, value, error) && std::isfinite(value);
    };
    double v = 0;
    if (def->row.requirement_expr && (!eval(def->row.requirement_expr, v) || v == 0)) return false;
    double seconds = std::numeric_limits<double>::infinity();
    if (duration) {
        if (!std::isfinite(*duration) || *duration < 0) return false;
        seconds = *duration;
    } else if (!def->row.infinite_duration) {
        if (!def->row.duration_expr || !eval(def->row.duration_expr, seconds) || seconds < 0) return false;
    }
    std::vector<stats::EffectMod> mods;
    for (const content::ModRow& m : def->mods) {
        if (m.shield_absorb || !m.value_expr) continue;
        if (!eval(m.value_expr, v)) return false;
        mods.push_back({m.stat_guid, v});
    }
    int limit = std::clamp(def->row.global_stack_limit, 1, 4096);
    if (def->row.in_place_stack_limit_expr && eval(def->row.in_place_stack_limit_expr, v)) limit = static_cast<int>(std::clamp(v, 1.0, 4096.0));
    size_t existing = n->effects.size();
    int same = 0;
    for (size_t i = 0; i < n->effects.size(); ++i) if (n->effects[i].def == def) {
        if (existing == n->effects.size()) existing = i;
        ++same;
    }
    const bool separate = def->row.stack_type.find("StackSeperate") != std::string::npos;
    const bool in_place = def->row.stack_type.find("StackInPlace") != std::string::npos;
    const bool never_refresh = def->row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
    const bool extend = def->row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
    if (existing != n->effects.size() && !separate) {
        Npc::Effect& e = n->effects[existing];
        if (!never_refresh) {
            double remaining = std::isinf(e.end) ? e.end : std::max(0.0, e.end - now);
            if (extend && !duration && def->extension_expr && !eval(def->extension_expr, seconds)) return false;
            if (extend) seconds += remaining;
            if (extend && def->extended_maximum > 0) seconds = std::min(seconds, def->extended_maximum);
            e.start = now;
            e.end = now + seconds;
            e.mods = std::move(mods);
            e.instigator = instigator;
        }
        if (in_place) e.stacks = std::min(limit, e.stacks + std::min(stacks, 4096));
        sync_effects(*n);
        return true;
    }
    if (n->effects.size() >= 256 || (separate && same >= limit)) return false;
    Npc::Effect e;
    e.key = n->next_effect_key++;
    e.def = def;
    e.instigator = instigator;
    e.stacks = in_place ? std::min(stacks, limit) : 1;
    e.start = now;
    e.end = now + seconds;
    e.mods = std::move(mods);
    n->effects.push_back(std::move(e));
    sync_effects(*n);
    return true;
}

void CombatWorld::tick_effects(Npc& n, double now) {
    for (size_t i = 0; i < n.effects.size();) {
        if (n.effects[i].end <= now) remove_effect(n, i);
        else ++i;
    }
}

void CombatWorld::shocked_reaction(Npc& n, uint64_t attacker, const Resolved& incoming, double now) {
    // Shocked's reaction accepts damaging direct hits, excluding reactions and ordinary effect ticks.
    if (incoming.miss() || incoming.source_type == content::kSourceTypeReaction || incoming.source_type == content::kSourceTypeEffect) return;
    bool damage = false;
    for (const auto& m : incoming.mods) damage = damage || (m.stat_guid == data_.ids().health && m.value < 0);
    if (!damage) return;
    const abilities::EffectDef* shocked = catalog_.effect_named("Status_Shocked");
    const abilities::HitDef* hit = catalog_.hit_named("Mage_Status_Shocked_ReactedHit");
    Fighter* source = fighter_of(attacker);
    if (!shocked || !hit || !source) return;
    auto it = std::find_if(n.effects.begin(), n.effects.end(), [&](const Npc::Effect& e) { return e.def == shocked; });
    if (it == n.effects.end()) return;
    HitMeta meta;
    meta.source_type = content::kSourceTypeReaction;
    meta.source_guid = 6064628911287762946ll; // Mage_Status_Shocked_Reaction, the effect's data-defined reaction record.
    Resolved resolved;
    std::string error;
    if (!resolver_.resolve(*source, n, *hit, meta, resolved, error)) return;
    const size_t index = static_cast<size_t>(it - n.effects.begin());
    if (--n.effects[index].stacks <= 0) remove_effect(n, index);
    else sync_effects(n);
    const Delivery delivery = apply_hit_to_npc(n.actor_id, attacker, resolved, now);
    if (PlayerPort* p = player(attacker); p && delivery.applied) {
        IncomingHit in;
        in.instigator = attacker;
        in.hit_guid = hit->row.guid;
        in.source_type = meta.source_type;
        in.source_guid = meta.source_guid;
        in.flags = delivery.flags;
        in.mods = resolved.mods;
        in.at = now;
        double taken = 0;
        for (size_t i = 0; i < resolved.mods.size(); ++i) if (resolved.mods[i].stat_guid == data_.ids().health) taken -= delivery.effective[i];
        p->confirm_hit_on(in, n.actor_id, taken, delivery.lethal, now);
    }
}

void CombatWorld::kill(Npc& n, uint64_t killer, double now) {
    if (n.dead) return;
    n.dead = true;
    clear_effects(n);
    n.killer = killer;
    n.ai.kill();
    n.cs->set_in_combat(false);
    n.cs->set_current(content::kGuidHealth, 0.0f);
    write_stat(registry_, n.actor_id, content::kGuidHealth, 0.0f);
    write_combat(registry_, n.actor_id, false, true, 0);
    n.flags_in_combat = false;
    n.flags_target = 0;
    if (npcs_world_) npcs_world_->halt(n.actor_id);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    ++stats_.kills;
    if (death_hook_) {
        const world::WorldActor* a = registry_.find(n.actor_id);
        DeathEvent e;
        e.npc = n.actor_id;
        e.killer = killer;
        e.at = now;
        e.level = n.lvl;
        e.position = n.position();
        e.blueprint = n.leaf;
        e.info = a ? &a->npc : nullptr;
        e.motion = &n.bp;
        e.attackers = n.attackers;
        death_hook_(e);
    }
}

// ---- thinking -------------------------------------------------------------------------------------------------------------------

void CombatWorld::set_flags(Npc& n, bool in_combat, uint64_t target_actor) {
    const uint64_t object = target_actor;
    if (n.flags_in_combat == in_combat && n.flags_target == object) return;
    n.flags_in_combat = in_combat;
    n.flags_target = object;
    n.cs->set_in_combat(in_combat);
    write_combat(registry_, n.actor_id, in_combat, false, object);
}

void CombatWorld::begin_reset(Npc& n, double now) {
    (void)now;
    ++stats_.resets;
    n.returning = true;
    clear_effects(n);
    n.attackers.clear();
    // it heals to full as it walks home
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, n.actor_id, content::kGuidHealth, max->value);
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    set_flags(n, false, 0);
    if (npcs_world_) npcs_world_->order_return_home(n.actor_id);
}

void CombatWorld::think(Npc& n, double now) {
    if (n.dead) return;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a) return;
    npc::ai::Sense s;
    s.now = now;
    s.self = {a->x, a->y, a->z};
    s.self_radius_cm = n.bp.radius;
    s.home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
    s.attack_range_cm = n.next.range_cm;
    const double far = n.ai.params().leash_radius_cm * 2.0 + n.ai.params().aggro_radius_cm;
    for (PlayerPort* p : players_) {
        const Fighter& f = p->fighter();
        if (distance_xy_cm(f.position(), s.self) > far) continue;
        npc::ai::Candidate c;
        c.id = p->actor_id();
        c.pos = f.position();
        c.radius_cm = f.radius_cm();
        c.alive = f.alive();
        c.hostile = true;
        s.known.push_back(c);
    }
    if (nav_) {
        s.line_of_sight = [this](const Vec3& from, const Vec3& to) {
            const nav::Vec3 p = to_nav(from), q = to_nav(to);
            if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;   // off the navmesh the ground says nothing
            return nav_->line_of_sight(p, q);
        };
    }
    const npc::ai::State before = n.ai.state();
    const npc::ai::Orders o = n.ai.think(s);
    if (o.began_fight) ++stats_.aggros;
    if (o.began_reset) begin_reset(n, now);
    if (o.reset_done) {
        n.returning = false;
        if (npcs_world_) npcs_world_->release(n.actor_id);
    }
    if (o.in_combat) set_flags(n, true, o.target);
    else if (n.ai.state() == npc::ai::State::Idle || n.ai.state() == npc::ai::State::Reset) set_flags(n, false, 0);
    if (npcs_world_ && !o.began_reset) {
        const nav::Vec3 here{a->x, a->y, a->z};
        switch (o.move) {
            case npc::ai::Orders::Move::ChaseTo: npcs_world_->order_move(n.actor_id, to_nav(o.dest), o.speed_cm_s, o.stop_distance_cm); break;
            case npc::ai::Orders::Move::Stand:
                if (before != npc::ai::State::Attack) npcs_world_->order_move(n.actor_id, here, 0.0, 0.0);
                break;
            default: break;
        }
    }
    if (o.attack && now >= n.next_attack_at) start_attack(n, o.target, now);
}
// Cyb + C-O-A

void CombatWorld::tick(double now) {
    now_ = now;
    if (!cfg_.enabled) return;
    resolve_pending(now);
    for (auto& [id, n] : npcs_) if (!n->dead) tick_effects(*n, now);
    if (now < next_think_) return;
    next_think_ = now + cfg_.think_seconds;

    // the players' combat flags end a while after the last exchange
    for (auto it = player_combat_until_.begin(); it != player_combat_until_.end();) {
        if (now >= it->second) {
            if (PlayerPort* p = player(it->first)) p->set_in_combat(false, now);
            it = player_combat_until_.erase(it);
        } else {
            ++it;
        }
    }

    std::set<uint64_t> todo;
    for (PlayerPort* p : players_) {
        const Vec3 pos = p->fighter().position();
        std::vector<uint64_t> ids;
        registry_.query(pos.x, pos.y, cfg_.scan_radius_cm, ids);
        for (const uint64_t id : ids) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            const auto known = npcs_.find(id);
            if (known != npcs_.end()) {
                todo.insert(id);
                continue;
            }
            if (huntable_ && huntable_(a->npc)) {   // a huntable is no fighter: its stats, nothing more
                fill_huntable_stats(*a);
                continue;
            }
            auto h = hostile_by_archetype_.find(a->npc.archetype);
            if (h == hostile_by_archetype_.end()) {
                const npc::MotionStats bp = motion_.get(a->npc.archetype);
                const std::vector<int64_t> al(bp.alignments.begin(), bp.alignments.end());
                h = hostile_by_archetype_.emplace(a->npc.archetype, data_.relation(al, CombatData::player_alignments()) == Relation::Hostile).first;
            }
            if (h->second) todo.insert(id);
        }
    }
    for (const auto& [id, n] : npcs_) {
        if (!n->dead && n->ai.state() != npc::ai::State::Idle) todo.insert(id);
    }
    for (const uint64_t id : todo) {
        Npc* n = combatant(id);
        if (n) think(*n, now);
    }
}

}  // namespace aoc::worldd::combat
