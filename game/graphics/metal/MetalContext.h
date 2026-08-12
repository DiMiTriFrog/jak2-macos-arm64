#pragma once

#include <memory>
#include <string>

struct SDL_Window;

namespace metal_backend {

class MetalContext {
 public:
  MetalContext(SDL_Window* window, bool debug);
  ~MetalContext();

  MetalContext(const MetalContext&) = delete;
  MetalContext& operator=(const MetalContext&) = delete;

  bool valid() const;
  const std::string& error() const;
  void resize();
  bool clear_and_present(float red, float green, float blue, float alpha);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace metal_backend
