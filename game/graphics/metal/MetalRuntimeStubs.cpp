#include <mutex>

// MIPS2C blend-shape code shares this lock with the OpenGL character renderer. The Metal
// renderer will replace this stub when the character resource path is ported; it is intentionally
// a data-only synchronization primitive and carries no GL dependency.
std::mutex g_merc_data_mutex;
