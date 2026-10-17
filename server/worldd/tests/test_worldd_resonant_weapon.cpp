// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "ability_rig.h"
#include "combat_rig.h"
#include "combat/hit_port.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <variant>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;
static int passed = 0, failed = 0;
#define CHECK(condition, message) do { \
    if (condition) { ++passed; std::printf("  [ok ] %s\n", message); } \
    else { ++failed; std::printf("  [FAIL] %s\n", message); } \
} while (0)

namespace {
class QueuedAlly final : public combat::PlayerPort {
public:
    struct Buff { int64_t guid; int stacks; std::optional<double> duration; std::shared_ptr<const combat::EffectSource> source; };
    struct Dispel { std::string tag; int limit; };
    using Receipt = std::variant<combat::IncomingHit, Buff, Dispel>;
    Rig rig;
    combat_rig::RigFighter body;
    std::vector<Receipt> inbox;
    QueuedAlly(uint64_t actor, double x) : rig(7, 25), body(rig, actor, {x, 0, 1098}) {}
    uint64_t actor_id() const override { return body.id(); }
    combat::Fighter& fighter() override { return body; }
    void receive_hit(const combat::IncomingHit& hit, double) override { inbox.emplace_back(hit); }
    bool receive_effect(int64_t guid, int stacks, std::optional<double> duration, uint64_t, double) override {
        inbox.emplace_back(Buff{guid, stacks, duration, nullptr});
        return true;
    }
    bool receive_effect_from(int64_t guid, int stacks, std::optional<double> duration, uint64_t,
                             std::shared_ptr<const combat::EffectSource> source, double) override {
        inbox.emplace_back(Buff{guid, stacks, duration, std::move(source)});
        return true;
    }
    bool receive_effect_removal(const std::string& tag, int limit, double) override {
        inbox.emplace_back(Dispel{tag, limit});
        return true;
    }
    void set_in_combat(bool, double) override {}
    void flush(double now) {
        for (const Receipt& receipt : inbox) {
            if (const auto* hit = std::get_if<combat::IncomingHit>(&receipt)) rig.pipe->receive_hit(*hit, now);
            else if (const auto* buff = std::get_if<Buff>(&receipt)) rig.pipe->grant_effect_from(buff->guid, buff->stacks, now, buff->duration, buff->source);
            else if (const auto* dispel = std::get_if<Dispel>(&receipt)) rig.pipe->remove_effects_by_tag(dispel->tag, dispel->limit, now);
            rig.take();
        }
        inbox.clear();
    }
};

struct Fixture {
    Rig rig;
    combat_rig::World scene;
    npc::MotionTable motion;
    std::optional<combat::CombatData> data;
    std::unique_ptr<combat::NpcStatBook> book;
    std::unique_ptr<combat::CombatWorld> world;
    std::unique_ptr<combat_rig::FakePlayer> player;
    std::unique_ptr<combat::WorldHitPort> port;
    uint64_t wolf = 0;
    std::string problem;
    explicit Fixture(const std::vector<std::string>& skills = {}) : rig(2, 25, skills) {
        if (!rig.ok()) { problem = rig.problem; return; }
        if (!scene.problem.empty()) { problem = scene.problem; return; }
        std::string error;
        if (!motion.load(NPC_MOTION_PATH, error)) { problem = error; return; }
        auto rows = rig.db->combat_rows(error);
        if (!rows) { problem = error; return; }
        data = combat::CombatData::build(*rows, *rig.ctx->bundle, error);
        if (!data) { problem = error; return; }
        book = std::make_unique<combat::NpcStatBook>(*rig.ctx->bundle, *rig.ctx->gear);
        world = std::make_unique<combat::CombatWorld>(scene.registry, scene.npcs.get(), &navfix::shared_nav(), *data, *rig.catalog, *book, motion);
        std::string wolf_path;
        for (const auto& [path, row] : motion.rows()) {
            if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
        }
        wolf = scene.spawn(wolf_path, 300, 0, 40, 1);
        if (wolf == 0) { problem = "the NPC fixture did not spawn"; return; }
        player = std::make_unique<combat_rig::FakePlayer>(rig, 77, combat::Vec3{0, 0, 1098});
        world->add_player(player.get());
        port = std::make_unique<combat::WorldHitPort>(*world, scene.registry, player->fighter());
        rig.pipe->set_combat(port.get());
        rig.know({"WeaponCombo_Melee_1H_Axe"});
        rig.asks = false;
    }
    ~Fixture() { if (world && player) world->remove_player(player.get()); }
    bool ok() const { return problem.empty(); }
    int stacks() const { return rig.pipe->stacks_of(rig.effect("Bard_Resonance")); }
    void prepare(const char* melody, int resonance = 6) {
        if (melody) rig.pipe->grant_effect(rig.effect(melody), 1, 0.1);
        rig.pipe->grant_effect(rig.effect("Bard_Resonance"), resonance, 0.2);
        rig.take();
    }
    bool swing(bool ending = true, double start = 2) {
        const auto* ability = rig.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
        if (!ability) return false;
        const abilities::HitDef* hit = nullptr;
        int64_t targeting = content::kTargetingDefaultSelf;
        for (const auto& row : ability->hits) {
            const auto* candidate = rig.catalog->hit(row.hit_guid);
            if (row.source != "hit" || !candidate) continue;
            const bool end = std::any_of(candidate->tags.begin(), candidate->tags.end(), [](const auto& tag) { return combat::tag_matches(tag, "combostep.end"); });
            if (end != ending) continue;
            hit = candidate;
            for (const auto& target : ability->targetings) if (target.key == row.key) targeting = target.guid;
            break;
        }
        if (!hit) return false;
        TriggerInfo info;
        info.target_count = 1;
        info.target_actors = {wolf};
        if (rig.pipe->trigger(ability->row.guid, rig.next_client_id++, start, info) != Refusal::None) return false;
        rig.run_to(start + 0.4);
        const auto verdict = rig.pipe->hit_request(ability->row.guid, targeting, hit->row.guid, 1, 0, start + 0.5, {wolf});
        rig.run_to(start + 1);
        return verdict == AbilityPipeline::HitVerdict::Scheduled;
    }
    bool confirmed(const char* name) const {
        const auto* hit = rig.catalog->hit_named(name);
        if (!hit) return false;
        for (const auto& event : rig.log)
            for (const auto& confirmation : event.confirmations)
                if (confirmation.record_guid == hit->row.guid) return true;
        return false;
    }
};

void test_melody(const char* melody, const char* confirmation, const char* status, int cost) {
    std::printf("%s\n", melody);
    Fixture f;
    CHECK(f.ok(), "the real-data combat fixture is ready");
    if (!f.ok()) { std::printf("    %s\n", f.problem.c_str()); return; }
    CHECK(f.rig.catalog->resonant_weapon_rules().size() == 7, "all seven Resonant Weapon reactions are compiled");
    f.prepare(melody);
    f.rig.set_current(content::kGuidHealth, f.rig.max_health() / 2);
    f.rig.set_current(content::kGuidMana, f.rig.pawn.stat(content::kGuidMaxMana) / 2);
    CHECK(f.swing(), "a verified weapon-combo finisher lands on a real NPC");
    CHECK(f.confirmed(confirmation), "the melody's actual proc hit is confirmed");
    CHECK(f.stacks() == 6 - cost, "the proc consumes only its data-defined number of Resonance stacks");
    if (status) {
        const auto* effect = f.rig.catalog->effect_named(status);
        CHECK(effect && f.port->effect_count(f.wolf, effect->row.guid) > 0, "the proc's debuff is present on its NPC target");
    }
    CHECK(f.rig.pawn.broken.empty(), "every caster effect/stat delta is valid for the client");
}

void test_gates() {
    Fixture f;
    CHECK(f.ok(), "the gate fixture is ready");
    if (!f.ok()) return;
    f.prepare("Bard_Music_Menacing_Self");
    CHECK(f.swing(false), "a non-finishing combo hit lands");
    CHECK(f.stacks() == 6 && !f.confirmed("Bard_ResonantWeapon_End_Menacing"), "an intermediate combo hit neither procs nor spends Resonance");
    f.rig.cs->set_passives({});
    f.rig.pipe->refresh(4, "the passive is absent");
    f.rig.take();
    CHECK(f.swing(true, 5), "a finisher still lands without the passive");
    CHECK(f.stacks() == 6 && !f.confirmed("Bard_ResonantWeapon_End_Menacing"), "a Bard without Resonant Weapon cannot trigger its combo proc");
    Fixture empty;
    CHECK(empty.ok(), "the empty-resource fixture is ready");
    if (!empty.ok()) return;
    empty.rig.pipe->grant_effect(empty.rig.effect("Bard_Music_Menacing_Self"), 1, 0.1);
    empty.rig.take();
    CHECK(empty.swing() && empty.stacks() == 0 && !empty.confirmed("Bard_ResonantWeapon_End_Menacing"), "a finisher with no Resonance has no proc");
}

void test_allied_heal_selection_and_receipts() {
    Fixture f;
    QueuedAlly nearer(88, 1000), injured(99, 2000), outside(111, 4000);
    CHECK(f.ok() && nearer.rig.ok() && injured.rig.ok() && outside.rig.ok(), "the allied proc fixtures are ready");
    if (!f.ok() || !nearer.rig.ok() || !injured.rig.ok() || !outside.rig.ok()) return;
    nearer.rig.set_current(content::kGuidHealth, nearer.rig.max_health() / 2);
    injured.rig.set_current(content::kGuidHealth, injured.rig.max_health() / 5);
    outside.rig.set_current(content::kGuidHealth, 1);
    f.world->add_player(&nearer);
    f.world->add_player(&injured);
    f.world->add_player(&outside);
    const double before = injured.rig.health();
    f.prepare("Bard_Music_Cheerful_Self");
    CHECK(f.swing(), "a finisher triggers Cheerful with several injured allies nearby");
    f.rig.run_to(5);
    CHECK(nearer.inbox.empty() && !injured.inbox.empty() && outside.inbox.empty(), "the actual targeting formula chooses the more injured ally within 30 metres");
    CHECK(injured.rig.health() == before, "the ally's queued hit has not changed its stats before its session applies it");
    const int64_t heal = f.rig.catalog->hit_named("Bard_ResonantWeapon_End_Cheerful_Projectile")->row.guid;
    bool guessed = false;
    for (const Seen& event : f.rig.log) for (const auto& confirmation : event.confirmations)
        guessed = guessed || (confirmation.record_guid == heal && !confirmation.targets.empty());
    CHECK(!guessed, "the caster does not send a guessed immediate confirmation for a deferred allied heal");
    injured.flush(5);
    bool confirmed = false;
    for (const Seen& event : injured.rig.log) for (const auto& confirmation : event.confirmations)
        confirmed = confirmed || (confirmation.record_guid == heal && !confirmation.mods.empty());
    CHECK(confirmed && injured.rig.health() > before, "the recipient applies the heal and confirms its authoritative stat delta");
    CHECK(injured.rig.pipe->stacks_of(injured.rig.effect("Bard_Glee")) == 1 && f.stacks() == 3,
          "the same ordered delivery applies Glee and consumes the caster's three stacks once");

    injured.rig.run_to(5);
    injured.rig.set_current(content::kGuidHealth, injured.rig.max_health() - 5);
    injured.rig.log.clear();
    combat::IncomingHit hit;
    hit.instigator = 77;
    hit.hit_guid = heal;
    hit.mods.push_back({1, content::kGuidHealth, 1000});
    injured.receive_hit(hit, 5);
    injured.receive_hit(hit, 5);
    injured.flush(5);
    std::vector<float> effective;
    for (const Seen& event : injured.rig.log) for (const auto& confirmation : event.confirmations)
        if (confirmation.record_guid == heal) for (const auto& mod : confirmation.mods) effective.push_back(mod.effective);
    CHECK(effective.size() == 2 && std::fabs(effective[0] - 5) <= 1e-4 && effective[1] == 0,
          "two queued heals clamp sequentially and report five actual Health followed by zero");
    f.world->remove_player(&nearer);
    f.world->remove_player(&injured);
    f.world->remove_player(&outside);
}

void test_effects_keep_the_casters_talents() {
    auto source = std::make_unique<Fixture>(std::vector<std::string>{"Bard_Passive_Merriment", "Bard_Passive_Dynamism"});
    QueuedAlly ally(88, 1000);
    CHECK(source->ok() && ally.rig.ok(), "a talented Bard and a Tank ally are ready");
    if (!source->ok() || !ally.rig.ok()) return;
    ally.rig.set_current(content::kGuidHealth, ally.rig.max_health() / 4);
    source->world->add_player(&ally);
    source->prepare("Bard_Music_Cheerful_Self");
    CHECK(source->swing(), "the talented Bard sends Cheerful's proc to the Tank");
    source->rig.run_to(4);
    source->world->remove_player(&ally);
    source.reset();   // the application snapshot must outlive the casting session
    ally.flush(4);
    const int64_t glee = ally.rig.effect("Bard_Glee");
    bool merriment = false;
    for (const auto& effect : ally.rig.cs->effects()) if (effect.effect.guid == glee)
        for (const auto& mod : effect.mods) merriment = merriment || std::fabs(mod.value - 0.03) < 1e-6;
    CHECK(merriment, "Glee retains the caster's Merriment value after the caster leaves");
    ally.rig.log.clear();
    ally.rig.run_to(6.1);
    bool credited = false;
    for (const Seen& event : ally.rig.log) for (const auto& hit : event.confirmations) {
        if (!hit.instigator || hit.instigator->ObjectId != 77) continue;
        for (const auto& mod : hit.mods) credited = credited || (mod.value > 0 && mod.effective > 0);
    }
    CHECK(credited, "Glee's subsequent healing tick still uses and credits the original caster");

    Fixture epic({"Bard_Passive_Dynamism"});
    QueuedAlly other(99, 1000);
    CHECK(epic.ok() && other.rig.ok(), "the caster-duration fixture is ready");
    if (!epic.ok() || !other.rig.ok()) return;
    epic.rig.pipe->grant_effect(epic.rig.effect("Bard_Pep"), 1, 0.01); // exclude the caster from Epic's target filter
    epic.rig.take();
    epic.world->add_player(&other);
    epic.prepare("Bard_Music_Epic_Self");
    CHECK(epic.swing(), "Epic sends its proc to an ally without Pep");
    epic.rig.run_to(4);
    other.flush(4);
    const auto end = other.rig.pipe->effect_end(other.rig.effect("Bard_Pep"));
    CHECK(end && std::fabs(*end - 9) < 1e-6, "Pep lasts five seconds from the caster's Dynamism, even on a Tank");
    epic.world->remove_player(&other);
}

void test_epic_dispels_only_three_constraints() {
    Fixture f;
    QueuedAlly ally(88, 1000);
    CHECK(f.ok() && ally.rig.ok(), "the Epic dispel fixtures are ready");
    if (!f.ok() || !ally.rig.ok()) return;
    const std::vector<const char*> constraints = {"General_Knockdown", "Generic_Snared_Effect", "Status_Rooted", "Status_Silenced"};
    // The first record has only gameplay tags; the second has the constraint only in EffectTags.
    for (const char* name : constraints) {
        CHECK(ally.rig.pipe->grant_effect(ally.rig.effect(name), 1, 0.01, 60.0), "a real crowd-control constraint is present before Epic");
    }
    const int64_t wounded = ally.rig.effect("Status_Wounded"), pep = ally.rig.effect("Bard_Pep");
    CHECK(ally.rig.pipe->grant_effect(wounded, 1, 0.01, 60.0), "an unrelated debuff is present beside the constraints");
    CHECK(f.rig.pipe->grant_effect(f.rig.effect("Bard_Pep"), 1, 0.01, 60.0), "the caster already has Pep and needs no constraint removal");
    ally.rig.take();
    f.rig.take();
    f.world->add_player(&ally);
    f.prepare("Bard_Music_Epic_Self");
    CHECK(f.swing(), "a verified Epic finisher selects the constrained ally");
    f.rig.run_to(4);
    ally.flush(4);
    bool first_three_removed = true;
    for (size_t i = 0; i < 3; ++i) first_three_removed = first_three_removed && ally.rig.pipe->stacks_of(ally.rig.effect(constraints[i])) == 0;
    CHECK(first_three_removed, "Epic removes three constraints, matching descendants in both tag lists");
    CHECK(ally.rig.pipe->stacks_of(ally.rig.effect(constraints.back())) == 1 && ally.rig.pipe->stacks_of(wounded) == 1,
          "Epic preserves the fourth constraint and the unrelated Wounded debuff");
    const auto end = ally.rig.pipe->effect_end(pep);
    CHECK(end && std::fabs(*end - 8) < 1e-6, "Epic grants Pep for its data-defined four seconds");
    bool credited = false;
    for (const Seen& event : ally.rig.log) for (const auto& effect : event.change.effects_added) {
        credited = credited || (effect.effect_guid == pep && effect.instigator_character_guid &&
                                *effect.instigator_character_guid == f.player->character_guid());
    }
    CHECK(credited, "the ally's Pep item retains the casting Bard's character GUID");
    CHECK(f.stacks() == 5, "the Epic finisher consumes exactly one caster Resonance stack");
    CHECK(ally.rig.pawn.broken.empty(), "the dispel and buff deltas remain valid for the recipient");
    f.world->remove_player(&ally);
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!real_db_present()) {
        std::printf("Resonant Weapon: skipped (content.db is unavailable)\n");
        return 0;
    }
    Rig prerequisite(2, 25);
    CHECK(prerequisite.ok(), "the content file and stat program load");
    if (!prerequisite.ok()) return 1;
    CHECK(prerequisite.catalog->resonant_weapon_rules().size() == 7, "content includes the full Resonant Weapon reaction closure");
    if (prerequisite.catalog->resonant_weapon_rules().size() != 7) {
        std::printf("Rebuild or refresh content.db with this content compiler before running the combo-proc tests.\n");
        return 1;
    }
    test_melody("Bard_Music_Cheerful_Self", "Bard_ResonantWeapon_End_Cheerful_Projectile", nullptr, 3);
    test_melody("Bard_Music_Pensive_Self", "Bard_ResonantWeapon_End_Pensive_Projectile", nullptr, 3);
    test_melody("Bard_Music_Epic_Self", "Bard_ResonantWeapon_End_Epic_Projectile", nullptr, 1);
    test_melody("Bard_Music_Menacing_Self", "Bard_ResonantWeapon_End_Menacing", "Status_Shocked", 3);
    test_melody("Bard_Music_Cathartic_Self", "Bard_ResonantWeapon_End_Cathartic", "Status_Wounded", 3);
    test_gates();
    test_allied_heal_selection_and_receipts();
    test_effects_keep_the_casters_talents();
    test_epic_dispels_only_three_constraints();
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
