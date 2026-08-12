#include "kmalloc.h"

#include <cstdio>
#include <cstring>

#include "common/goal_constants.h"
#include "common/jit_memory.h"
#include "common/platform/BuildConfig.h"

#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/memory_layout.h"

// global and debug kernel heaps
Ptr<kheapinfo> kglobalheap;
Ptr<kheapinfo> kdebugheap;
// if we should count the number of strings and types allocated on the global heap.
bool kheaplogging = false;
enum MemItemsCategory {
  STRING = 0,
  TYPE = 1,
  NUM_CATEGORIES = 2,
};
int MemItemsCount[NUM_CATEGORIES] = {0, 0};
int MemItemsSize[NUM_CATEGORIES] = {0, 0};

#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
struct Arm64FunctionPool {
  u32 heap_offset = 0;
  u32 next = 0;
  u32 end = 0;
};

constexpr size_t ARM64_FUNCTION_POOL_COUNT = 32;
Arm64FunctionPool arm64_function_pools[ARM64_FUNCTION_POOL_COUNT];

Arm64FunctionPool* get_arm64_function_pool(u32 heap_offset) {
  Arm64FunctionPool* free_pool = nullptr;
  for (auto& pool : arm64_function_pools) {
    if (pool.heap_offset == heap_offset) {
      return &pool;
    }
    if (!pool.heap_offset && !free_pool) {
      free_pool = &pool;
    }
  }
  if (free_pool) {
    free_pool->heap_offset = heap_offset;
  }
  return free_pool;
}
#endif

void kmalloc_init_globals_common() {
  // _globalheap and _debugheap
  kglobalheap.offset = GLOBAL_HEAP_INFO_ADDR;
  kdebugheap.offset = DEBUG_HEAP_INFO_ADDR;
  kheaplogging = false;
  for (auto& x : MemItemsCount)
    x = 0;
  for (auto& x : MemItemsSize)
    x = 0;
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
  for (auto& pool : arm64_function_pools) {
    pool = {};
  }
#endif
}

/*!
 * In the game, this wraps PS2's libc's malloc/calloc.
 * These don't work with GOAL's custom memory management, and this function
 * is unused.
 * DONE, malloc/calloc calls commented out because memory allocated with calloc/malloc
 * cannot trivially be accessed from within GOAL.
 */
Ptr<u8> ksmalloc(Ptr<kheapinfo> heap, s32 size, u32 flags, char const* name) {
  (void)heap;
  (void)size;
  (void)name;
  printf("[ERROR] ksmalloc : cannot be used!\n");
  u32 align = flags & 0xfff;
  Ptr<u8> mem;

  if ((flags & KMALLOC_MEMSET) == 0) {
    // mem = malloc(size + align);
  } else {
    // mem = calloc(1, size + align);
  }

  if (align == KMALLOC_ALIGN_64) {
    mem.offset = (mem.offset + 0x3f) & 0xffffffc0;
  } else if (align == KMALLOC_ALIGN_256) {
    mem.offset = (mem.offset + 0xff) & 0xffffff00;
  }

  return mem;
}

/*!
 * Print the status of a kheap.  This prints to stdout on the runtime,
 * which will not be sent to the Listener.
 * DONE, EXACT
 */
Ptr<kheapinfo> kheapstatus(Ptr<kheapinfo> heap) {
  Msg(6,
      "[%8x] kheap\n"
      "\tbase: #x%x\n"
      "\ttop-base: #x%x\n"
      "\tcur: #x%x\n"
      "\ttop: #x%x\n",
      heap.offset, heap->base.offset, heap->top_base.offset, heap->current.offset,
      heap->top.offset);
  // note: max symbols here is game-version dependent
  Msg(6,
      "\t used bot: %d of %d bytes\n"
      "\t used top: %d of %d bytes\n"
      "\t symbols: %d of %d\n",
      heap->current - heap->base, heap->top_base - heap->base, heap->top_base - heap->top,
      heap->top_base - heap->base, NumSymbols, max_symbols(g_game_version));

  if (heap == kglobalheap) {
    Msg(6, "\t %d bytes before stack\n", GLOBAL_HEAP_END - heap->current.offset);
  }

  for (int i = 0; i < NUM_CATEGORIES; i++) {
    printf("  %d: %d %d\n", i, MemItemsCount[i], MemItemsSize[i]);
  }

  // might not have returned heap in jak 1
  return heap;
}

/*!
 * Initialize a kheapinfo structure, and clear the kheap's memory to 0.
 * DONE, EXACT
 */
Ptr<kheapinfo> kinitheap(Ptr<kheapinfo> heap, Ptr<u8> mem, s32 size) {
  heap->base = mem;
  heap->current = mem;
  heap->top = mem + size;
  heap->top_base = heap->top;
  std::memset(mem.c(), 0, size);
  return heap;
}

/*!
 * Return how much of the bottom (non-temp) allocator is used.
 * DONE, EXACT
 */
u32 kheapused(Ptr<kheapinfo> heap) {
  return heap->current - heap->base;
}

/*!
 * Allocate memory using bump allocation strategy.
 * @param heapPtr : heap to allocate on. If null heap, use global but print a warning
 * @param size    : size of memory needed
 * @param flags   : flags for alignment, top/bottom allocation, set to zero
 * @param name    : name of allocation (printed if things go wrong)
 * @return        : memory.  0 if we run out of room
 * DONE, PRINT ADDED
 */
Ptr<u8> kmalloc(Ptr<kheapinfo> heap, s32 size, u32 flags, char const* name) {
  uint32_t alignment_flag = flags & 0xfff;
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
  const s32 requested_size = size;
  const bool executable_allocation = (flags & KMALLOC_EXECUTABLE) != 0;
#else
  constexpr bool executable_allocation = false;
#endif
  uint32_t executable_page_size = 0;
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
  if (executable_allocation) {
    executable_page_size = static_cast<uint32_t>(jit_memory::page_size());
    if (size > 0) {
      size = static_cast<s32>((static_cast<uint32_t>(size) + executable_page_size - 1) &
                              ~(executable_page_size - 1));
    }
  }
#else
  (void)executable_allocation;
#endif

  // if we got a null heap, put it on the global heap, but warn about it
  if (!heap.offset) {
    // the 0 is uninitialized in jak1, set to zero in jak 2. might just be compiler differences.
    Msg(6, "-----------> kmalloc: alloc %s,  mem %s #x%x (a:%d  %dbytes)\n", "DEBUG", name, 0,
        alignment_flag, size);
    heap = kglobalheap;
  }

  uint32_t memstart;

#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
  // C-backed GOAL functions are tiny executable objects. Keep their pages
  // isolated from mutable heap data, but pack multiple objects into each page
  // instead of charging one 16 KiB page per 0x40-byte trampoline.
  if (executable_allocation && !(flags & KMALLOC_TOP) && requested_size > 0 &&
      static_cast<uint32_t>(requested_size) < executable_page_size &&
      strcmp(name, "function") == 0) {
    auto* pool = get_arm64_function_pool(heap.offset);
    if (!pool) {
      MsgErr("kmalloc: no ARM64 function pool for heap %x\n", heap.offset);
      return Ptr<u8>(0);
    }

    const u32 object_size = (static_cast<u32>(requested_size) + 0xf) & ~0xf;
    // alloc_heap_object returns the address after the type tag, and the
    // trampoline writer uses that address for the executable bytes. Reserve
    // the tag too, so the JIT write range cannot cross into the next page.
    const u32 allocation_size = object_size + BASIC_OFFSET;
    if (!pool->next || pool->next + allocation_size > pool->end) {
      auto page = kmalloc(heap, static_cast<s32>(executable_page_size), KMALLOC_EXECUTABLE,
                          "function-page");
      if (!page.offset) {
        return page;
      }
      pool->next = page.offset;
      pool->end = page.offset + executable_page_size;
    }

    memstart = pool->next;
    pool->next += allocation_size;
    jit_memory::make_writable(Ptr<u8>(memstart).c(), allocation_size);
    if (flags & KMALLOC_MEMSET) {
      std::memset(Ptr<u8>(memstart).c(), 0, allocation_size);
    }
    return Ptr<u8>(memstart);
  }
#endif

  if (!(flags & KMALLOC_TOP)) {
    // allocate from bottom
    if (executable_allocation) {
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
      memstart = (heap->current.offset + executable_page_size - 1) &
                 ~(executable_page_size - 1);
#else
      memstart = heap->current.offset;
#endif
    } else if (alignment_flag == KMALLOC_ALIGN_64)
      memstart = (0xffffffc0 & (heap->current.offset + 0x40 - 1));
    else if (alignment_flag == KMALLOC_ALIGN_256)
      memstart = (0xffffff00 & (heap->current.offset + 0x100 - 1));
    else  // includes 0x10!
      memstart = (0xfffffff0 & (heap->current.offset + 0x10 - 1));

    if (size == 0) {
      Msg(6, "[WARNING] kmalloc : size 0 allocation from bottom.\n");
      return Ptr<u8>(memstart);
    }

    uint32_t memend = memstart + size;

    if (heap->top.offset < memend) {
      kheapstatus(heap);
      Msg(6, "kmalloc: !alloc mem %s (%d bytes) heap %x\n", name, size, heap.offset);
      return Ptr<u8>(0);
    }

#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
    // A bottom allocation may reach a page previously used by a temporary
    // top-level segment after that segment's top pointer is reset. Restore RW
    // before the caller fills the allocation or clears it. Executable
    // allocations are page-aligned and reserve complete pages, so this cannot
    // overlap a persistent executable allocation.
    jit_memory::make_writable(Ptr<u8>(memstart).c(), static_cast<size_t>(size));
#endif
    heap->current.offset = memend;
    if (flags & KMALLOC_MEMSET)
      std::memset(Ptr<u8>(memstart).c(), 0, (size_t)size);
    return Ptr<u8>(memstart);
  } else {
    // allocate from top
    if (alignment_flag == 0) {
      alignment_flag = KMALLOC_ALIGN_16;
    }

    if (executable_allocation) {
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
      memstart = (heap->top.offset - size) & ~(executable_page_size - 1);
#else
      memstart = heap->top.offset - size;
#endif
    } else {
      memstart = (heap->top.offset - size) & (-alignment_flag);
    }

    if (size == 0) {
      Msg(6, "[WARNING] kmalloc : size 0 allocation from top\n");
      return Ptr<u8>(memstart);
    }

    if (heap->current.offset >= memstart) {
      Msg(6, "kmalloc: !alloc mem from top %s (%d bytes) heap %x\n", name, size, heap.offset);
      kheapstatus(heap);
      return Ptr<u8>(0);
    }

#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
    // Top-level code is temporary.  Once its RX pages are returned to the
    // top allocator, a later mutable allocation may reuse them; restore RW
    // before the caller fills the allocation or clears it.
    jit_memory::make_writable(Ptr<u8>(memstart).c(), static_cast<size_t>(size));
#endif
    heap->top.offset = memstart;

    if (flags & KMALLOC_MEMSET)
      std::memset(Ptr<u8>(memstart).c(), 0, (size_t)size);

    // this logging was added in Jak 3, but we port it back to all games:
    if ((heap == kglobalheap) && (kheaplogging != 0)) {
      if (strcmp(name, "string") == 0) {
        MemItemsCount[STRING]++;
        MemItemsSize[STRING] += size;
      } else if (strcmp(name, "type") == 0) {
        MemItemsCount[TYPE]++;
        MemItemsSize[TYPE] += size;
      }
    }
    return Ptr<u8>(memstart);
  }
}

/*!
 * GOAL does not support automatic freeing of memory. This function does nothing.
 * Programmers wishing to free memory must do it themselves.
 * DONE, PRINT ADDED
 */
void kfree(Ptr<u8> a) {
  (void)a;
  Msg(6, "[ERROR] kmalloc: kfree called\n");
}
