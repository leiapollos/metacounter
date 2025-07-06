#pragma once
#include "../generated_counter_registry.h"

namespace Engine {
class Renderer {
public:
    void Initialize();
    void RenderFrame();

private:
    static constexpr auto RENDERER_DRAWCALLS = REGISTER_COUNTER(DrawCalls);
    static constexpr auto RENDERER_SHADERS = REGISTER_COUNTER(ShaderBinds);
    static constexpr auto RENDERER_TEXTURES = REGISTER_COUNTER(TextureBinds);
    static constexpr auto RENDER_CONTEXT = REGISTER_UNIQUE_COUNTER(MainRenderContext);
};
}