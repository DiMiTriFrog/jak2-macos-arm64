#include "metal.h"

#include <string>

#include "common/log/log.h"
#include "common/platform/BuildConfig.h"
#include "game/graphics/metal/MetalContext.h"
#include "game/system/hid/sdl_util.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {

bool metal_initialized = false;

int metal_init(GfxGlobalSettings& settings) {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    lg::error("Metal SDL initialization failed: {}", SDL_GetError());
    return 1;
  }
  metal_initialized = true;
  lg::info("Metal backend selected (AOT={}, iOS={})", goal_platform::kAotCode,
           goal_platform::kIos);
  (void)settings;
  return 0;
}

void metal_exit() {
  metal_initialized = false;
  SDL_Quit();
}

std::shared_ptr<GfxDisplay> metal_make_display(int width,
                                               int height,
                                               const char* title,
                                               GfxGlobalSettings& settings,
                                               GameVersion,
                                               bool is_main) {
  const auto flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
  if (!window) {
    lg::error("Metal window creation failed: {}", SDL_GetError());
    return nullptr;
  }

  auto display = std::make_shared<MetalDisplay>(window, is_main);
  if (!display->get_display_manager()) {
    SDL_DestroyWindow(window);
    return nullptr;
  }
  display->set_imgui_visible(false);
  (void)settings;
  return display;
}

u32 metal_vsync() {
  return 0;
}

u32 metal_sync_path() {
  return 0;
}

void metal_send_chain(const void*, u32) {}
void metal_texture_upload_now(const u8*, int, u32) {}
void metal_texture_relocate(u32, u32, u32) {}
void metal_set_levels(const std::vector<std::string>&) {}
void metal_set_active_levels(const std::vector<std::string>&) {}
void metal_force_reload_all() {}
void metal_force_reload_level(const std::string&) {}
void metal_force_reload_common() {}
void metal_set_pmode_alp(float) {}

}  // namespace

MetalDisplay::MetalDisplay(SDL_Window* window, bool is_main)
    : m_window(window),
      m_context(std::make_unique<metal_backend::MetalContext>(window, Gfx::g_global_settings.debug)),
      m_display_manager(std::make_shared<DisplayManager>(window)),
      m_input_manager(std::make_shared<InputManager>(window)) {
  m_main = is_main;
  if (!m_context->valid()) {
    lg::error("Metal context creation failed: {}", m_context->error());
    m_display_manager.reset();
    m_input_manager.reset();
    return;
  }
  m_display_manager->set_input_manager(m_input_manager);
}

MetalDisplay::~MetalDisplay() {
  m_input_manager.reset();
  m_display_manager.reset();
  m_context.reset();
  if (m_window) {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
  }
  if (m_main) {
    metal_exit();
  }
}

void MetalDisplay::process_sdl_events() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      m_should_quit = true;
    }
    if (m_display_manager) {
      m_display_manager->process_sdl_event(event);
    }
    if (m_input_manager) {
      m_input_manager->process_sdl_event(event);
    }
  }
}

void MetalDisplay::render() {
  if (!m_context || !m_context->valid()) {
    m_should_quit = true;
    return;
  }
  m_input_manager->poll_keyboard_data();
  m_input_manager->poll_mouse_data();
  m_input_manager->finish_polling();
  process_sdl_events();
  m_display_manager->process_ee_events();
  m_input_manager->process_ee_events();
  if (!m_context->clear_and_present(0.02f, 0.02f, 0.025f, 1.0f)) {
    lg::error("Metal frame failed: {}", m_context->error());
    m_should_quit = true;
  }
}

const GfxRendererModule gRendererMetal = {
    metal_init,
    metal_make_display,
    metal_exit,
    metal_vsync,
    metal_sync_path,
    metal_send_chain,
    metal_texture_upload_now,
    metal_texture_relocate,
    metal_set_levels,
    metal_set_active_levels,
    metal_force_reload_all,
    metal_force_reload_level,
    metal_force_reload_common,
    metal_set_pmode_alp,
    GfxPipeline::Metal,
    "Metal (frame-clear)"};
