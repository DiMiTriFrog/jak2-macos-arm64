#include "game/external/discord.h"

#include <sstream>

int gDiscordRpcEnabled = 0;
int64_t gStartTime = 0;

void init_discord_rpc() {}
void set_discord_rpc(int state) {
  gDiscordRpcEnabled = state;
}
std::string get_time_of_day(float time) {
  std::ostringstream result;
  const int hour = static_cast<int>(time);
  const int minute = static_cast<int>((time - hour) * 60.0f);
  result << hour << ':' << (minute < 10 ? "0" : "") << minute;
  return result.str();
}
std::string get_base_level_name(const std::map<std::string, std::string>& remap,
                                const char* level_name) {
  const auto it = remap.find(level_name);
  return it == remap.end() ? level_name : it->second;
}
const char* get_full_level_name(const std::map<std::string, std::string>& names,
                                const std::map<std::string, std::string>& remap,
                                const char* level_name) {
  const auto base = get_base_level_name(remap, level_name);
  const auto it = names.find(base);
  return it == names.end() ? "unknown" : it->second.c_str();
}
bool indoors(std::vector<std::string> levels, const char* level_name) {
  return std::find(levels.begin(), levels.end(), level_name) != levels.end();
}
void handleDiscordReady(const DiscordUser*) {}
void handleDiscordDisconnected(int, const char*) {}
void handleDiscordError(int, const char*) {}
void handleDiscordJoin(const char*) {}
void handleDiscordJoinRequest(const DiscordUser*) {}
void handleDiscordSpectate(const char*) {}

extern "C" {
void Discord_Initialize(const char*, DiscordEventHandlers*, int, const char*) {}
void Discord_Shutdown(void) {}
void Discord_RunCallbacks(void) {}
void Discord_UpdatePresence(const DiscordRichPresence*) {}
void Discord_ClearPresence(void) {}
void Discord_Respond(const char*, int) {}
void Discord_UpdateHandlers(DiscordEventHandlers*) {}
}
