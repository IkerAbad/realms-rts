// Enfermedad en el real (H3): un real abarrotado y parado enferma; una oleada pequeña o
// un ejército en marcha, no; las enfermas contagian; un puesto médico cerca lo frena y,
// ingresada, se sana antes y sin perder vida; enferma se pega, se marcha y se vive
// peor; y sin reales abarrotados, nada cambia (ni el hash).

#include <algorithm>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "sim/disease.hpp"
#include "sim/world.hpp"
#include "test_helpers.hpp"

using reinos::sim::Command;
using reinos::sim::CommandType;
using reinos::sim::DiseaseParams;
using reinos::sim::Health;
using reinos::sim::Sickness;
using reinos::sim::World;
using reinos::sim::WorldParams;
using namespace reinos::test;

namespace {

constexpr std::int32_t kIllness = 2400;
constexpr std::int32_t kSickAfter = 3600;

// Las cifras de data/config/engine.toml.
DiseaseParams disease() {
    DiseaseParams d;
    d.enabled = true;
    d.check_every_ticks = 20;
    d.cell_tiles = 3;
    d.crowd_troops = 20;
    d.sick_after_ticks = kSickAfter;
    d.sick_spread_percent = 50;
    d.contagion_percent = 50;
    d.contagion_max_sick = 4;
    d.care_radius_tiles = 8;
    d.care_exposure_percent = 25;
    d.illness_ticks = kIllness;
    d.sick_hp_interval_ticks = 100;
    d.ward_recovery_percent = 200;
    return d;
}

WorldParams world_params(bool enabled = true) {
    WorldParams p = test_world_params(0, 16);
    p.map.bands = {{reinos::sim::kElevationRange, 1}};
    p.disease = disease();
    p.disease.enabled = enabled;
    p.combat.sick_attack_percent = 70;
    p.movement.sick_speed_percent = 75;
    for (auto& u : p.unit_types) {
        u.morale_resolve = u.worker ? 0 : 50;  // la tropa tiene moral; los aldeanos, no
    }
    return p;
}

entt::entity ent(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

Command order(CommandType type, std::vector<std::uint32_t> units, reinos::sim::TileCoord target = {},
              std::uint32_t object = reinos::sim::kNoObject) {
    Command c;
    c.type = type;
    c.player = 0;
    c.units = std::move(units);
    c.target = target;
    c.object = object;
    return c;
}

// n soldados en filas de 6 desde (x0, y0), a los que se ordena juntarse en `camp`.
std::vector<std::uint32_t> camp(World& w, std::int32_t n, reinos::sim::TileCoord camp_at) {
    std::vector<std::uint32_t> ids;
    for (std::int32_t i = 0; i < n; ++i) {
        ids.push_back(w.spawn_unit(0, kSoldier, {camp_at.x - 10 + i % 6, camp_at.y - 10 + i / 6}));
    }
    w.issue(order(CommandType::Move, ids, camp_at));
    return ids;
}

// Avanza `ticks` y devuelve cuántas han enfermado en ese tiempo.
std::int32_t fell_sick(World& w, std::int32_t ticks) {
    std::int32_t n = 0;
    for (std::int32_t t = 0; t < ticks; ++t) {
        w.step();
        n += w.disease().last_stats().fell_sick;
    }
    return n;
}

std::int32_t exposed_now(const World& w) {
    return static_cast<std::int32_t>(w.registry().view<const Sickness>().size());
}

void make_sick(World& w, std::uint32_t id) {
    w.registry_for_setup().emplace_or_replace<Sickness>(ent(id), Sickness{0, kIllness, 0});
}

}  // namespace

TEST_CASE("Enfermedad: un real de 30 parado enferma, y no antes de 3 minutos") {
    World world(world_params());
    camp(world, 30, {100, 100});
    CHECK(fell_sick(world, kSickAfter) == 0);  // nadie antes del umbral más bajo
    CHECK(exposed_now(world) == 30);           // pero todos se están exponiendo
    const std::int32_t later = fell_sick(world, 3000);
    MESSAGE("enfermaron ", later, " de 30 entre el minuto 3 y el 5,5");
    CHECK(later >= 15);
}

TEST_CASE("Enfermedad: ni una oleada de 12 parada ni 30 en marcha se exponen") {
    SUBCASE("12 parados") {
        World world(world_params());
        camp(world, 12, {100, 100});
        CHECK(fell_sick(world, 6000) == 0);
        CHECK(exposed_now(world) == 0);
    }
    SUBCASE("30 que van y vienen") {
        World world(world_params());
        const auto army = camp(world, 30, {100, 100});
        std::int32_t fell = 0;
        for (int leg = 0; leg < 15; ++leg) {
            world.issue(order(CommandType::Move, army, leg % 2 == 0 ? reinos::sim::TileCoord{130, 100}
                                                                    : reinos::sim::TileCoord{100, 100}));
            fell += fell_sick(world, 400);
        }
        CHECK(fell == 0);
    }
}

TEST_CASE("Enfermedad: cada unidad tiene su umbral, entre 3 y 4,5 minutos") {
    World world(world_params());
    std::vector<std::int32_t> thresholds;
    for (std::uint32_t i = 1; i <= 50; ++i) {
        const std::int32_t t = world.disease().threshold(ent(i));
        CHECK(t >= kSickAfter);
        CHECK(t <= kSickAfter * 3 / 2);
        thresholds.push_back(t);
    }
    std::ranges::sort(thresholds);
    CHECK(std::ranges::unique(thresholds).size() < 50);  // se repiten pocos: hay reparto
    CHECK(thresholds.front() < thresholds.back());
}

TEST_CASE("Enfermedad: las enfermas contagian a las de alrededor") {
    const auto sick_by = [](bool seeded) {
        World world(world_params());
        const auto army = camp(world, 30, {100, 100});
        if (seeded) {
            for (std::size_t i = 0; i < 4; ++i) {
                make_sick(world, army[i]);
            }
        }
        return fell_sick(world, 2500);
    };
    CHECK(sick_by(false) == 0);  // aún bajo el umbral
    const std::int32_t seeded = sick_by(true);
    MESSAGE("con 4 enfermas en el real, enfermaron ", seeded, " más en 2 minutos");
    CHECK(seeded >= 10);  // 4 enfermas: se exponen al triple y pasan el umbral antes
}

TEST_CASE("Enfermedad: un puesto médico cerca deja la exposición en la cuarta parte") {
    WorldParams p = world_params();
    p.building_types[kHouse].beds = 2;  // la casa hace de puesto médico
    World world(p);
    REQUIRE(world.spawn_building(0, kHouse, {104, 96}, true).has_value());
    const auto army = camp(world, 30, {100, 100});
    CHECK(fell_sick(world, 6000) == 0);  // al 25 %, harían falta más de 12 minutos
    const auto& sk = world.registry().get<Sickness>(ent(army.front()));
    CHECK(sk.exposure > 0);
    CHECK(sk.exposure < kSickAfter / 2);
}

TEST_CASE("Enfermedad: enferma pega al 70 %, marcha al 75 % y pierde vida; luego sana") {
    // Cada una en su mundo, por el mismo camino: el terreno no cambia la cuenta.
    const auto marched = [](bool ill) {
        World world(world_params());
        const auto id = world.spawn_unit(0, kSoldier, {100, 100});
        if (ill) {
            make_sick(world, id);
        }
        world.issue(order(CommandType::Move, {id}, {140, 100}));
        for (int t = 0; t < 100; ++t) {
            world.step();
        }
        return world.registry().get<reinos::sim::Position>(ent(id)).x.raw() - reinos::sim::Fixed::from_int(100).raw();
    };
    const std::int64_t fast = marched(false);
    const std::int64_t slow = marched(true);
    REQUIRE(fast > 0);
    CHECK(slow * 100 >= fast * 73);
    CHECK(slow * 100 <= fast * 77);

    World world(world_params());
    const auto sick = world.spawn_unit(0, kSoldier, {100, 100});
    const auto well = world.spawn_unit(0, kSoldier, {100, 104});
    make_sick(world, sick);
    const auto& reg = world.registry();
    const std::int32_t healthy =
        world.combat().attack_percent(reg, ent(well), reg.get<reinos::sim::Combatant>(ent(well)), false);
    CHECK(world.combat().attack_percent(reg, ent(sick), reg.get<reinos::sim::Combatant>(ent(sick)), false) ==
          healthy * 70 / 100);

    const std::int32_t hp0 = reg.get<Health>(ent(sick)).hp;
    for (int t = 0; t < 100; ++t) {
        world.step();
    }
    reinos::sim::Snapshot s;
    world.write_snapshot(s);
    const auto flag = [&](std::uint32_t id) {
        return std::ranges::find(s.entities, id, &reinos::sim::SnapshotEntity::id)->sick;
    };
    CHECK(flag(sick));
    CHECK_FALSE(flag(well));
    CHECK(reg.get<Health>(ent(sick)).hp == hp0 - 1);  // 1 de vida cada 100 ticks

    for (int t = 100; t < kIllness; ++t) {
        world.step();
    }
    CHECK_FALSE(reg.get<Sickness>(ent(sick)).sick());  // sana a los 2 minutos
    CHECK(reg.get<Health>(ent(sick)).hp == hp0 - kIllness / 100);  // también el último tick
    for (int t = 0; t < 40; ++t) {
        world.step();  // sin real abarrotado y con la exposición a 0: el componente se va
    }
    CHECK_FALSE(reg.all_of<Sickness>(ent(sick)));
}

TEST_CASE("Enfermedad: ingresada en un puesto médico no pierde vida y sana el doble de deprisa") {
    WorldParams p = world_params();
    for (auto& u : p.unit_types) {
        u.treatable = !u.combat.buildings_only;
    }
    p.building_types[kHouse].beds = 2;
    p.building_types[kHouse].care_percent = 100;
    p.building_types[kHouse].heal_to_percent = 100;
    p.medicine.bed_heal_milli_per_tick = 1;  // apenas cura heridas: sigue ingresada
    p.medicine.patient_ration_ticks = 100;
    p.medicine.care_reach = reinos::sim::Fixed::from_ratio(4, 5);
    p.supply.ration_cost = stock(1, 0, 0, 0);
    World world(p);
    world.set_stock(0, stock(1000, 0, 0, 0));
    const auto post = *world.spawn_building(0, kHouse, {104, 99}, true);
    const auto soldier = world.spawn_unit(0, kSoldier, {102, 100});
    world.set_hp(soldier, 20);
    world.issue(order(CommandType::Treat, {soldier}, {}, post));
    for (int t = 0; t < 200; ++t) {
        world.step();
    }
    const reinos::sim::Patient* pt = world.registry().try_get<reinos::sim::Patient>(ent(soldier));
    REQUIRE((pt != nullptr && pt->admitted));
    make_sick(world, soldier);
    const std::int32_t hp0 = world.registry().get<Health>(ent(soldier)).hp;
    for (int t = 0; t < kIllness / 2 + 10; ++t) {
        world.step();
    }
    const Sickness* sk = world.registry().try_get<Sickness>(ent(soldier));
    CHECK((sk == nullptr || !sk->sick()));  // sana (y, sin exposición, sin componente)
    CHECK(world.registry().get<Health>(ent(soldier)).hp >= hp0);
}

TEST_CASE("Enfermedad: sin reales abarrotados no cambia nada, ni el hash") {
    const auto play = [](bool enabled) {
        World world(world_params(enabled));
        for (int i = 0; i < 8; ++i) {
            const auto id = world.spawn_unit(0, kSoldier, {80 + i * 5, 100});
            world.issue(order(CommandType::Move, {id}, {80 + i * 5, 130}));
        }
        for (int t = 0; t < 3000; ++t) {
            world.step();
        }
        CHECK(world.registry().view<const Sickness>().empty());
        return world.state_hash();
    };
    CHECK(play(true) == play(false));
}

