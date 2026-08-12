#include "gfx_test.h"

#include "common/log/log.h"
#include "game/graphics/metal/MetalContext.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace tests {

void to_json(json& j, const GPUTestOutput& obj) {
  json_serialize(success);
  json_serialize(error);
  json_serialize(errorCause);
  json_serialize_optional(gpuRendererString);
  json_serialize_optional(gpuVendorString);
}

GPUTestOutput run_gpu_test(const std::string& test_type) {
  if (test_type != "metal") {
    return {false, "Only the Metal GPU test is available in a Metal build", test_type};
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return {false, "SDL initialization failed", SDL_GetError()};
  }
  SDL_Window* window = SDL_CreateWindow("Metal Test", 64, 64,
                                        SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
  if (!window) {
    const std::string error = SDL_GetError();
    SDL_Quit();
    return {false, "Metal window creation failed", error};
  }
  metal_backend::MetalContext context(window, false);
  GPUTestOutput result = {context.valid(), "", context.error()};
  if (result.success && !context.clear_and_present(0.0f, 0.0f, 0.0f, 1.0f)) {
    result = {false, "Metal clear/present failed", context.error()};
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}

}  // namespace tests
