#pragma once
#include "../generated_counter_registry.h"

class Player {
public:
    void TakeDamage(int amount);
private:
    static constexpr auto PLAYER_HEALTH = REGISTER_COUNTER(PlayerHealth);
    static constexpr auto PLAYER_STAMINA = REGISTER_COUNTER(PlayerStamina);
};