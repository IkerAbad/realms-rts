#pragma once

// Enfermedad en el real (H3). En las guerras de antes la disentería mataba a veces más
// que las armas: «en ocasiones era la causante de más muertes que las que causaba la
// guerra en sí» (Wikipedia, «Disentería»), y un ejército sitiador, «encamped in possibly
// squalid field conditions», podía acabar sufriendo «the disease and starvation intended
// for the besieged» (Wikipedia, «Siege»). Se contagia por «contamination of food and
// water with feces due to poor sanitation» (Wikipedia, «Dysentery»).
//
// Regla, cada check_every_ticks:
//   - Una tropa (con moral) parada y sin ingresar se expone si en su celda de
//     cell_tiles x cell_tiles y las 8 de alrededor hay crowd_troops tropas paradas de su
//     dueño o más: el real abarrotado. Contar también las vecinas evita que el borde de
//     una celda parta el real en trozos que no llegan. Cada enferma de alrededor (hasta
//     contagion_max_sick) añade contagion_percent a lo que se expone; un puesto médico
//     propio a care_radius_tiles o menos (higiene, enfermeros) lo deja en
//     care_exposure_percent.
//   - Sin real abarrotado (en marcha, o con sitio) la exposición baja al mismo ritmo.
//   - Con la exposición por encima de su umbral, enferma. El umbral es sick_after_ticks
//     más hasta un sick_spread_percent, distinto por unidad (de su número de entidad):
//     un real no enferma entero a la vez.
//   - Enferma, durante illness_ticks pega y marcha peor (combat.sick_attack_percent,
//     movement.sick_speed_percent) y pierde 1 de vida cada sick_hp_interval_ticks;
//     puede morir. Ingresada en un puesto médico no pierde vida y sana
//     ward_recovery_percent más deprisa. Al sanar, la exposición vuelve a 0.
//
// Las unidades que nunca se exponen no llevan el componente Sickness: sin reales
// abarrotados, el estado y el hash son los de siempre.

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "sim/economy.hpp"
#include "sim/tick.hpp"
#include "sim/units.hpp"

namespace reinos::sim {

class StateHasher;

// data/config/engine.toml, sección [disease].
struct DiseaseParams {
    bool enabled = false;
    std::int32_t check_every_ticks = 1;
    std::int32_t cell_tiles = 1;
    std::int32_t crowd_troops = 1;
    std::int32_t sick_after_ticks = 1;
    std::int32_t sick_spread_percent = 0;
    std::int32_t contagion_percent = 0;
    std::int32_t contagion_max_sick = 0;
    std::int32_t care_radius_tiles = 0;
    std::int32_t care_exposure_percent = 100;
    std::int32_t illness_ticks = 1;
    std::int32_t sick_hp_interval_ticks = 0;  // 0: no quita vida
    std::int32_t ward_recovery_percent = 100;
};

struct DiseaseTickStats {
    std::int32_t exposed = 0;    // tropas en un real abarrotado este tick
    std::int32_t sick = 0;       // enfermas ahora
    std::int32_t fell_sick = 0;  // han enfermado este tick
    std::int32_t recovered = 0;  // han sanado este tick
    std::int32_t died = 0;       // muertas de enfermedad este tick
};

class DiseaseSystem {
public:
    DiseaseSystem(const DiseaseParams& params, std::vector<UnitType> units, std::vector<BuildingType> buildings,
                  std::int32_t width, std::int32_t height, std::int32_t players);

    void update(entt::registry& registry, EconomySystem& economy, Tick tick);

    [[nodiscard]] const DiseaseParams& params() const noexcept { return params_; }
    [[nodiscard]] const DiseaseTickStats& last_stats() const noexcept { return stats_; }
    // Umbral de exposición de esta unidad (sick_after_ticks más su parte del reparto).
    [[nodiscard]] std::int32_t threshold(entt::entity e) const noexcept;

    void hash_into(StateHasher& h, const entt::registry& registry) const;

private:
    [[nodiscard]] std::size_t cell_of(TileCoord t, PlayerId player) const noexcept;
    // Suma de v en la celda de t y las 8 de alrededor, del jugador.
    [[nodiscard]] std::int32_t around(const std::vector<std::int32_t>& v, TileCoord t, PlayerId player) const noexcept;

    DiseaseParams params_;
    std::vector<UnitType> units_;
    std::vector<BuildingType> buildings_;
    std::int32_t cells_x_ = 1;
    std::int32_t cells_y_ = 1;
    std::int32_t players_ = 1;
    std::vector<std::int32_t> crowd_;  // tropas paradas por celda y jugador
    std::vector<std::int32_t> sick_;   // de ellas, enfermas
    std::vector<entt::entity> dead_;
    DiseaseTickStats stats_;
};

}  // namespace reinos::sim
