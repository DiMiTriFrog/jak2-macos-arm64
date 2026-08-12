#pragma once

#include <memory>

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"

namespace metal_backend {
class MetalContext;
}

class MetalDisplay final : public GfxDisplay {
 public:
  MetalDisplay(SDL_Window* window, bool is_main);
  ~MetalDisplay() override;

  std::shared_ptr<DisplayManager> get_display_manager() const override { return m_display_manager; }
  std::shared_ptr<InputManager> get_input_manager() const override { return m_input_manager; }
  void render() override;
  void init_splash() override {}
  void draw_splash(int, int) override {}

 private:
  SDL_Window* m_window = nullptr;
  std::unique_ptr<metal_backend::MetalContext> m_context;
  std::shared_ptr<DisplayManager> m_display_manager;
  std::shared_ptr<InputManager> m_input_manager;
  bool m_should_quit = false;

  void process_sdl_events();
};

extern const GfxRendererModule gRendererMetal;
