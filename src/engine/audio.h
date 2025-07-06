#pragma once
#include "../generated_counter_registry.h"

namespace Engine {
class AudioSystem {
public:
    void PlaySound();
private:
    static constexpr auto ACTIVE_CHANNELS = REGISTER_COUNTER(ActiveAudioChannels);
};
}