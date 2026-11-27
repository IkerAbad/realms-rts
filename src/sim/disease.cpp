#include "sim/disease.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "sim/state_hash.hpp"

namespace reinos::sim {

namespace {

// Mezcla de bits sin estado (la de MurmurHash3): reparte los umbrales por unidad.
[[nodiscard]] std::uint32_t mix(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x85eb'ca6bU;
    x ^= x >> 13;
    x *= 0xc2b2'ae35U;
    x ^= x >> 16;
    return x;
}

// Distancia al cuadrado, en casillas, de una casilla al rectángulo de una huella.
[[nodiscard]] std::int64_t distance_sq(TileCoord t, const Footprint& f) noexcept {
    const std::int32_t dx = std::max({f.origin.x - t.x, 0, t.x - (f.origin.x + f.size - 1)});
    const std::int32_t dy = std::max({f.origin.y - t.y, 0, t.y - (f.origin.y + f.size - 1)});
    return std::int64_t{dx} * dx + std::int64_t{dy} * dy;
}

}  // namespace

DiseaseSystem::DiseaseSystem(const DiseaseParams& params, std::vector<UnitType> units,
                             std::vector<BuildingType> buildings, std::int32_t width, std::int32_t height,
                             std::int32_t players)
    : params_(params), units_(std::move(units)), buildings_(std::move(buildings)), players_(std::max(players, 1)) {
    const std::int32_t c = std::max(params_.cell_tiles, 1);
    cells_x_ = std::max((width + c - 1) / c, 1);
    cells_y_ = std::max((height + c - 1) / c, 1);
    const std::size_t n = static_cast<std::size_t>(cells_x_) * static_cast<std::size_t>(cells_y_) *
                          static_cast<std::size_t>(players_);
    crowd_.assign(n, 0);
    sick_.assign(n, 0);
}

std::size_t DiseaseSystem::cell_of(TileCoord t, PlayerId player) const noexcept {
    const std::int32_t c = std::max(params_.cell_tiles, 1);
    const std::int32_t x = std::clamp(t.x / c, 0, cells_x_ - 1);
    const std::int32_t y = std::clamp(t.y / c, 0, cells_y_ - 1);
    return (static_cast<std::size_t>(player) * static_cast<std::size_t>(cells_y_) + static_cast<std::size_t>(y)) *
               static_cast<std::size_t>(cells_x_) +
           static_cast<std::size_t>(x);
}

std::int32_t DiseaseSystem::around(const std::vector<std::int32_t>& v, TileCoord t, PlayerId player) const noexcept {
    const std::int32_t c = std::max(params_.cell_tiles, 1);
    const std::int32_t cx = std::clamp(t.x / c, 0, cells_x_ - 1);
    const std::int32_t cy = std::clamp(t.y / c, 0, cells_y_ - 1);
    std::int32_t sum = 0;
    for (std::int32_t y = std::max(cy - 1, 0); y <= std::min(cy + 1, cells_y_ - 1); ++y) {
        for (std::int32_t x = std::max(cx - 1, 0); x <= std::min(cx + 1, cells_x_ - 1); ++x) {
            sum += v[cell_of({x * c, y * c}, player)];
        }
    }
    return sum;
}

std::int32_t DiseaseSystem::threshold(entt::entity e) const noexcept {
    const std::int64_t spread = std::int64_t{params_.sick_after_ticks} * params_.sick_spread_percent / kPercent;
    if (spread <= 0) {
        return params_.sick_after_ticks;
    }
    const auto extra = mix(entt::to_integral(e)) % static_cast<std::uint32_t>(spread + 1);
    return params_.sick_after_ticks + static_cast<std::int32_t>(extra);
}

void DiseaseSystem::update(entt::registry& registry, EconomySystem& economy, Tick tick) {
    stats_ = DiseaseTickStats{};
    if (!params_.enabled) {
        return;
    }
    const auto admitted = [&](entt::entity e) {
        const Patient* pt = registry.try_get<Patient>(e);
        return pt != nullptr && pt->admitted;
    };

    // 1. Las enfermas, cada tick: cuenta atrás, vida perdida y alta.
    dead_.clear();
    for (const auto [e, sk, hp] : registry.view<Sickness, Health>().each()) {
        if (!sk.sick()) {
            continue;
        }
        ++stats_.sick;
        const bool ward = admitted(e);
        // Ingresada sana ward_recovery_percent más deprisa, repartido sin fracciones.
        const std::int64_t pace = ward ? params_.ward_recovery_percent : kPercent;
        const auto t = static_cast<std::int64_t>(tick);
        sk.sick_ticks -= static_cast<std::int32_t>((t + 1) * pace / kPercent - t * pace / kPercent);
        if (!ward && params_.sick_hp_interval_ticks > 0 && ++sk.hp_timer >= params_.sick_hp_interval_ticks) {
            sk.hp_timer = 0;
            hp.hp = std::max(hp.hp - 1, 0);
            if (hp.hp == 0) {
                dead_.push_back(e);
                continue;
            }
        }
        if (sk.sick_ticks <= 0) {
            sk = Sickness{};  // sana: la exposición vuelve a 0
            ++stats_.recovered;
        }
    }
    for (const entt::entity e : dead_) {
        economy.record_loss(registry.get<Owner>(e).player);
        registry.destroy(e);
        ++stats_.died;
    }

    // 2. La exposición, cada check_every_ticks.
    if (params_.check_every_ticks <= 0 || tick % static_cast<Tick>(params_.check_every_ticks) != 0) {
        return;
    }
    const auto troop = [&](const Unit& u) { return u.type < units_.size() && units_[u.type].morale_resolve > 0; };
    const auto camped = [&](entt::entity e) {
        const MoveGoal* goal = registry.try_get<MoveGoal>(e);
        return (goal == nullptr || goal->arrived) && !admitted(e);
    };
    std::ranges::fill(crowd_, 0);
    std::ranges::fill(sick_, 0);
    const auto view = registry.view<const Unit, const Owner, const Position>();
    for (const auto [e, unit, owner, pos] : view.each()) {
        if (!troop(unit) || std::cmp_greater_equal(owner.player, players_) || !camped(e)) {
            continue;
        }
        const std::size_t c = cell_of(tile_of(pos), owner.player);
        ++crowd_[c];
        const Sickness* sk = registry.try_get<Sickness>(e);
        sick_[c] += sk != nullptr && sk->sick() ? 1 : 0;
    }
    // Puestos médicos propios que funcionan: higiene y enfermeros.
    std::vector<std::pair<PlayerId, Footprint>> posts;
    for (const auto [b, building, foot, owner] : registry.view<const Building, const Footprint, const Owner>().each()) {
        if (building.working() && building.type < buildings_.size() && buildings_[building.type].beds > 0) {
            posts.emplace_back(owner.player, foot);
        }
    }
    const std::int64_t care_sq = std::int64_t{params_.care_radius_tiles} * params_.care_radius_tiles;
    const std::int32_t every = params_.check_every_ticks;
    for (const auto [e, unit, owner, pos] : view.each()) {
        if (!troop(unit) || std::cmp_greater_equal(owner.player, players_)) {
            continue;
        }
        Sickness* sk = registry.try_get<Sickness>(e);
        if (sk != nullptr && sk->sick()) {
            continue;  // ya enferma: cuenta atrás en el paso 1
        }
        const TileCoord t = tile_of(pos);
        if (camped(e) && around(crowd_, t, owner.player) >= params_.crowd_troops) {
            ++stats_.exposed;
            const std::int32_t sick_near = std::min(around(sick_, t, owner.player), params_.contagion_max_sick);
            std::int64_t gain = every;
            gain = gain * (kPercent + std::int64_t{params_.contagion_percent} * sick_near) / kPercent;
            const bool cared = std::ranges::any_of(posts, [&](const auto& p) {
                return p.first == owner.player && distance_sq(t, p.second) <= care_sq;
            });
            if (cared) {
                gain = gain * params_.care_exposure_percent / kPercent;
            }
            if (sk == nullptr) {
                sk = &registry.emplace<Sickness>(e);
            }
            sk->exposure = static_cast<std::int32_t>(std::min<std::int64_t>(
                std::int64_t{sk->exposure} + gain, std::numeric_limits<std::int32_t>::max() / 2));
            if (sk->exposure >= threshold(e)) {
                sk->sick_ticks = params_.illness_ticks;
                sk->hp_timer = 0;
                ++stats_.fell_sick;
            }
        } else if (sk != nullptr) {
            sk->exposure = std::max(sk->exposure - every, 0);
            if (sk->exposure == 0) {
                registry.remove<Sickness>(e);  // como si nunca se hubiera expuesto
            }
        }
    }
}

void DiseaseSystem::hash_into(StateHasher& h, const entt::registry& registry) const {
    for (const auto [e, sk] : registry.view<const Sickness>().each()) {
        h.add_u32(entt::to_integral(e));
        h.add_i32(sk.exposure);
        h.add_i32(sk.sick_ticks);
        h.add_i32(sk.hp_timer);
    }
}

}  // namespace reinos::sim
