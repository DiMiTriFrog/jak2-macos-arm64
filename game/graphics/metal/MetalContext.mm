#include "MetalContext.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <sstream>

#include "third-party/SDL/include/SDL3/SDL_metal.h"

namespace metal_backend {

struct MetalContext::Impl {
  SDL_Window* window = nullptr;
  SDL_MetalView view = nullptr;
  CAMetalLayer* layer = nil;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  std::string error;
};

namespace {

std::string ns_error(NSError* error, const char* fallback) {
  if (!error) {
    return fallback;
  }
  std::ostringstream out;
  out << fallback << ": " << error.localizedDescription.UTF8String;
  return out.str();
}

}  // namespace

MetalContext::MetalContext(SDL_Window* window, bool debug) : m_impl(std::make_unique<Impl>()) {
  m_impl->window = window;
  if (!window) {
    m_impl->error = "Metal context requires an SDL window";
    return;
  }

  m_impl->view = SDL_Metal_CreateView(window);
  if (!m_impl->view) {
    m_impl->error = std::string("SDL_Metal_CreateView failed: ") + SDL_GetError();
    return;
  }

  m_impl->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_impl->view);
  if (!m_impl->layer) {
    m_impl->error = "SDL_Metal_GetLayer returned no CAMetalLayer";
    return;
  }

  m_impl->device = MTLCreateSystemDefaultDevice();
  if (!m_impl->device) {
    m_impl->error = "MTLCreateSystemDefaultDevice returned no device";
    return;
  }

  m_impl->queue = [m_impl->device newCommandQueue];
  if (!m_impl->queue) {
    m_impl->error = "MTLDevice could not create a command queue";
    return;
  }

  NSString* library_path = [[NSBundle mainBundle] pathForResource:@"OpenGOAL"
                                                             ofType:@"metallib"
                                                        inDirectory:@"metal"];
  if (!library_path) {
    m_impl->error = "OpenGOAL.metallib is missing from the application bundle";
    return;
  }
  NSError* library_error = nil;
  NSURL* library_url = [NSURL fileURLWithPath:library_path];
  m_impl->library = [m_impl->device newLibraryWithURL:library_url error:&library_error];
  if (!m_impl->library) {
    m_impl->error = ns_error(library_error, "Metal failed to load OpenGOAL.metallib");
    return;
  }
  if (![m_impl->library newFunctionWithName:@"synthetic_vertex"] ||
      ![m_impl->library newFunctionWithName:@"synthetic_color"]) {
    m_impl->error = "OpenGOAL.metallib is missing the required synthetic shader functions";
    return;
  }

  m_impl->layer.device = m_impl->device;
  m_impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  m_impl->layer.framebufferOnly = YES;
  m_impl->layer.maximumDrawableCount = 3;
  if (debug) {
    m_impl->layer.allowsNextDrawableTimeout = YES;
  }
  resize();
}

MetalContext::~MetalContext() {
  if (m_impl && m_impl->view) {
    SDL_Metal_DestroyView(m_impl->view);
    m_impl->view = nullptr;
  }
}

bool MetalContext::valid() const {
  return m_impl && m_impl->error.empty() && m_impl->layer && m_impl->device && m_impl->queue;
}

const std::string& MetalContext::error() const {
  static const std::string empty;
  return m_impl ? m_impl->error : empty;
}

void MetalContext::resize() {
  if (!valid()) {
    return;
  }
  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(m_impl->window, &width, &height) || width <= 0 || height <= 0) {
    return;
  }
  m_impl->layer.drawableSize = CGSizeMake(width, height);
}

bool MetalContext::clear_and_present(float red, float green, float blue, float alpha) {
  if (!valid()) {
    return false;
  }

  @autoreleasepool {
    resize();
    id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
    if (!drawable) {
      m_impl->error = "CAMetalLayer returned no drawable";
      return false;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);

    id<MTLCommandBuffer> command_buffer = [m_impl->queue commandBuffer];
    if (!command_buffer) {
      m_impl->error = "MTLCommandQueue could not create a command buffer";
      return false;
    }
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
  }
  return true;
}

}  // namespace metal_backend
