// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

/// Reads the replicated status item, including its stable FastArray id, from a creature.
const repl::FastArrayItem* npc_effect_item(const world::WorldActor* actor, int64_t guid) {
    if (!actor) return nullptr;
    for (const repl::Subobject& sub : actor->actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const auto* arr = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, "CurrentEffects");
        if (!arr) return nullptr;
        for (const auto& item : arr->items()) {
            const auto it = item.values.values.find(2);
            if (it != item.values.values.end()) {
                const auto* value = it->second.get_if<int64_t>();
                if (value && *value == guid) return &item;
            }
        }
    }
    return nullptr;
}

void test_target_effects(Fixture& f) {
    std::printf("NPC targeted effects: stacks, duration, replication, expiry and death\n");
    Rig rig(2, 20, {});
    combat_rig::World w;
    CHECK(rig.ok() && w.problem.empty(), "the Bard and NPC world start");
    if (!rig.ok() || !w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, nullptr, nullptr, *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {5000, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter(), 100.0);
    const uint64_t wolf = w.spawn(wolf_archetype(f), 0, 0, 10, 1);
    const auto* wounded = f.catalog->effect_named("Status_Wounded");
    const auto* shocked = f.catalog->effect_named("Status_Shocked");
    CHECK(wolf && wounded && shocked, "the target and both status definitions exist");
    if (!wolf || !wounded || !shocked) return;

    CHECK(port.apply_effect(wolf, wounded->row.guid, 3, std::nullopt, 0), "three Wounded stacks apply through the runtime clock adapter");
    CHECK(port.effect_count(wolf, wounded->row.guid) == 3, "the target stat inputs contain the applied stacks");
    const auto* item = npc_effect_item(w.registry.find(wolf), wounded->row.guid);
    CHECK(item != nullptr, "the status is present in the NPC CurrentEffects array");
    const int32_t original_id = item ? item->id : -1;
    if (item) {
        const auto* start = item->values.values.at(3).get_if<double>();
        const auto* duration = item->values.values.at(4).get_if<float>();
        CHECK(start && near(*start, bootstrap::effect_clock(100), 1e-9) && duration && *duration == 15,
              "the status uses the world synchronized clock and its data-defined duration");
    }
    CHECK(port.apply_effect(wolf, wounded->row.guid, 100, std::nullopt, 1), "reapplying Wounded refreshes the instance");
    CHECK(port.effect_count(wolf, wounded->row.guid) == 10, "Wounded respects its ten-stack limit");
    item = npc_effect_item(w.registry.find(wolf), wounded->row.guid);
    CHECK(item && item->id == original_id, "a stack refresh retains its replicated array id");
    // NPC profiles export only their member stats; formulas can also read the other settled values.
    std::string wound_error;
    const auto wound_values = world.fighter_of(wolf)->stats().evaluation({}, wound_error, true);
    const auto wound_index = f.ctx->bundle->by_guid.find(6064629999720283747ll);
    CHECK(wound_values && wound_index != f.ctx->bundle->by_guid.end() && wound_values->result().values[wound_index->second] == 10,
          "the Wounded stat modifier runs once per stack in the target's formula inputs");
    world.tick(115.9);
    CHECK(port.effect_count(wolf, wounded->row.guid) == 10, "the refreshed Wounded duration remains active");
    world.tick(116);
    CHECK(port.effect_count(wolf, wounded->row.guid) == 0 && !npc_effect_item(w.registry.find(wolf), wounded->row.guid),
          "expiry removes both the stat input and the replicated item");

    CHECK(port.apply_effect(wolf, shocked->row.guid, 2, std::nullopt, 20), "Shocked applies with its own duration");
    CHECK(port.apply_effect(wolf, shocked->row.guid, 3, std::nullopt, 22), "Shocked extends on reapplication");
    item = npc_effect_item(w.registry.find(wolf), shocked->row.guid);
    if (item) {
        const auto* duration = item->values.values.at(4).get_if<float>();
        CHECK(duration && *duration == 7, "Shocked extends four remaining seconds by max(6 - 4, 3), from its expression");
    } else CHECK(false, "Shocked is replicated");
    CHECK(port.effect_count(wolf, shocked->row.guid) == 5, "Shocked stacks accumulate");
    world.tick(129);
    CHECK(port.effect_count(wolf, shocked->row.guid) == 0, "the extended Shocked duration expires on the world clock");

    CHECK(!port.apply_effect(wolf, wounded->row.guid, 1, -1.0, 30), "an invalid duration is rejected without adding an effect");
    CHECK(port.apply_effect(wolf, wounded->row.guid, 2, 30.0, 30), "a hit duration override is accepted");
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1e9});
    world.apply_hit_to_npc(wolf, 77, kill, 130);
    CHECK(port.effect_count(wolf, wounded->row.guid) == 0 && !npc_effect_item(w.registry.find(wolf), wounded->row.guid),
          "death clears a nonpersistent status and its replicated item");
    CHECK(!port.apply_effect(wolf, wounded->row.guid, 1, std::nullopt, 31), "a dead target cannot receive a new status");
}

void test_shocked_reaction(Fixture& f) {
    std::printf("Shocked consumes one stack for a damaging direct hit\n");
    Rig rig(2, 20, {});
    combat_rig::World w;
    if (!rig.ok() || !w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, nullptr, nullptr, *f.data, *f.catalog, book, f.motion);
    combat_rig::FakePlayer player(rig, 77, {500, 0, 1098});
    world.add_player(&player);
    const uint64_t wolf = w.spawn(wolf_archetype(f), 0, 0, 20, 1);
    const auto* shocked = f.catalog->effect_named("Status_Shocked");
    const auto* reaction = f.catalog->hit_named("Mage_Status_Shocked_ReactedHit");
    CHECK(wolf && shocked && reaction && !reaction->mods.empty(), "the Shocked reaction and its real damage formula were compiled");
    if (!wolf || !shocked || !reaction) return;
    CHECK(world.apply_effect(wolf, 77, shocked->row.guid, 3, 30.0, 0), "the target starts with three Shocked stacks");
    combat::Resolved hit;
    hit.source_type = content::kSourceTypeAbility;
    hit.mods.push_back({1, content::kGuidHealth, -1});
    const double before = world.health_of(wolf);
    world.apply_hit_to_npc(wolf, 77, hit, 0);
    CHECK(world.effect_count(wolf, shocked->row.guid) == 2, "one direct hit consumes exactly one Shocked stack");
    CHECK(world.health_of(wolf) < before - 1, "the data-defined lightning reaction deals additional damage");
    hit.source_type = content::kSourceTypeReaction;
    world.apply_hit_to_npc(wolf, 77, hit, 0);
    CHECK(world.effect_count(wolf, shocked->row.guid) == 2, "reaction damage does not recursively consume Shocked");
    hit.source_type = content::kSourceTypeEffect;
    world.apply_hit_to_npc(wolf, 77, hit, 0);
    CHECK(world.effect_count(wolf, shocked->row.guid) == 2, "ordinary effect damage does not trigger the direct-hit reaction");
    hit.source_type = content::kSourceTypeAbility;
    hit.flags = stats::kHitMiss;
    world.apply_hit_to_npc(wolf, 77, hit, 0);
    CHECK(world.effect_count(wolf, shocked->row.guid) == 2, "a missed direct hit does not trigger the reaction");
    hit.flags = 0;
    hit.mods.front().value = 0;
    world.apply_hit_to_npc(wolf, 77, hit, 0);
    CHECK(world.effect_count(wolf, shocked->row.guid) == 2, "a non-damaging hit does not trigger the reaction");
}

void test_rejected_player_hit(Fixture& f) {
    std::printf("a full player inbox refuses delivery\n");
    Rig rig(2, 20, {});
    if (!rig.ok()) return;
    struct FullInbox final : combat::PlayerPort {
        combat_rig::RigFighter self;
        int delivered = 0;
        explicit FullInbox(Rig& r) : self(r, 88, {0, 0, 1098}) {}
        uint64_t actor_id() const override { return self.id(); }
        combat::Fighter& fighter() override { return self; }
        void receive_hit(const combat::IncomingHit&, double) override { ++delivered; }
        bool queue_hit(const combat::IncomingHit&, double) override { return false; }
        void set_in_combat(bool, double) override {}
    } recipient(rig);
    world::ActorRegistry registry;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(registry, nullptr, nullptr, *f.data, *f.catalog, book, f.motion);
    world.add_player(&recipient);
    const double before = rig.cs->find(content::kGuidHealth)->value;
    combat::Resolved hit;
    hit.mods.push_back({1, content::kGuidHealth, 5});
    const auto delivery = world.apply_hit(88, 77, hit, 0);
    CHECK(!delivery.applied && !delivery.deferred_confirmation && delivery.effective.empty(), "a rejected queue creates no successful delivery or confirmation");
    CHECK(recipient.delivered == 0 && rig.cs->find(content::kGuidHealth)->value == before, "the rejected hit never reaches the recipient");
}

void test_effect_source_snapshot(Fixture& f) {
    std::printf("queued effects retain the actual caster's stat inputs\n");
    auto caster = std::make_unique<Rig>(2, 20);
    Rig target(4, 10);
    if (!caster->ok() || !target.ok()) return;
    struct SnapshotPort final : combat::PlayerPort {
        combat_rig::RigFighter self;
        std::shared_ptr<const combat::EffectSource> source;
        explicit SnapshotPort(Rig& r, uint64_t id) : self(r, id, {0, 0, 1098}) {}
        uint64_t actor_id() const override { return self.id(); }
        aoc::protocol::emit::FIntrepidNetworkGUID actor_guid() const override {
            auto guid = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(self.id());
            guid.ServerId = 60;
            guid.Randomizer = 1234;
            return guid;
        }
        std::array<uint32_t, 4> character_guid() const override { return {1, 2, 3, static_cast<uint32_t>(self.id())}; }
        combat::Fighter& fighter() override { return self; }
        void receive_hit(const combat::IncomingHit&, double) override {}
        void set_in_combat(bool, double) override {}
        bool receive_effect_from(int64_t, int, std::optional<double>, uint64_t,
                                 std::shared_ptr<const combat::EffectSource> snapshot, double) override {
            source = std::move(snapshot);
            return true;
        }
    };
    auto source = std::make_unique<SnapshotPort>(*caster, 77);
    SnapshotPort recipient(target, 88);
    world::ActorRegistry registry;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(registry, nullptr, nullptr, *f.data, *f.catalog, book, f.motion);
    world.add_player(source.get());
    world.add_player(&recipient);
    const auto* effect = f.catalog->effect_named("Bard_Glee");
    const double health = caster->cs->find(content::kGuidMaxHealth)->value;
    const auto source_guid = source->actor_guid();
    const auto source_character = source->character_guid();
    CHECK(effect && world.apply_effect(88, 77, effect->row.guid, 1, std::nullopt, 0), "a queued ally effect accepts a source snapshot");
    CHECK(recipient.source && recipient.source->evaluation && recipient.source->actor == source_guid && recipient.source->character_guid == source_character,
          "the effect carries the caster's full actor GUID, character GUID and evaluation");
    if (!recipient.source || !recipient.source->evaluation) return;
    CHECK(recipient.source->evaluation->inputs().class_enum == 2 && recipient.source->evaluation->inputs().level == 20,
          "the effect uses the Bard caster's class and level rather than the Fighter recipient's");
    world.remove_player(source.get());
    source.reset();
    caster.reset();
    CHECK(near(recipient.source->evaluation->stat(content::kGuidMaxHealth), health, 1e-4),
          "the retained caster evaluation remains valid after its session and character stats are destroyed");
}
