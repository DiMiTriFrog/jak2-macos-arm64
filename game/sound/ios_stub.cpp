#include "game/sound/sdshim.h"
#include "game/sound/sndshim.h"

#include <cstring>

// IOS-006 replaces these compile-only shims with an AudioUnit/SDL audio
// backend. Keeping the symbols here makes the iOS target honest: it does not
// link the desktop cubeb/CoreAudio device implementation while the AOT and
// Metal work is validated independently.
std::shared_ptr<snd::Voice> voices[kNVoices];
u8 spu_memory[0x15160 * 10] = {};
namespace snd {
u64 SoundFlavaHack = 0;
}

void snd_StartSoundSystem() {}
void snd_StopSoundSystem() {}
s32 snd_GetTick() {
  return 0;
}
void snd_RegisterIOPMemAllocator(AllocFun, FreeFun) {}
int snd_LockVoiceAllocator(bool) {
  return 0;
}
void snd_UnlockVoiceAllocator() {}
s32 snd_ExternVoiceAlloc(s32, s32) {
  return 0;
}
u32 snd_SRAMMalloc(u32) {
  return 0;
}
void snd_SRAMMarkUsed(u32, u32) {}
void snd_SetMixerMode(s32, s32) {}
void snd_SetGroupVoiceRange(s32, s32, s32) {}
void snd_SetReverbDepth(s32, s32, s32) {}
void snd_SetReverbType(s32, s32) {}
void snd_SetPanTable(s16*) {}
void snd_SetPlayBackMode(s32) {}
s32 snd_SoundIsStillPlaying(s32) {
  return 0;
}
void snd_StopSound(s32) {}
u32 snd_GetSoundID(s32) {
  return static_cast<u32>(-1);
}
void snd_SetSoundVolPan(s32, s32, s32) {}
void snd_SetMasterVolume(s32, s32) {}
void snd_UnloadBank(snd::BankHandle) {}
void snd_ResolveBankXREFS() {}
void snd_ContinueAllSoundsInGroup(u8) {}
void snd_PauseAllSoundsInGroup(u8) {}
void snd_SetMIDIRegister(s32, u8, u8) {}
void snd_SetGlobalExcite(u8) {}
s32 snd_PlaySoundVolPanPMPB(snd::BankHandle, s32, s32, s32, s32, s32) {
  return 0;
}
s32 snd_PlaySoundByNameVolPanPMPB(snd::BankHandle, char*, char*, s32, s32, s32, s32) {
  return 0;
}
void snd_SetSoundPitchModifier(s32, s32) {}
void snd_SetSoundPitchBend(s32, s32) {}
void snd_PauseSound(s32) {}
void snd_ContinueSound(s32) {}
void snd_AutoPitch(s32, s32, s32, s32) {}
void snd_AutoPitchBend(s32, s32, s32, s32) {}
snd::BankHandle snd_BankLoadEx(const char*, s32, u32, u32) {
  return nullptr;
}
void snd_BankLoadFromIOPPartialEx_Start() {}
void snd_BankLoadFromIOPPartialEx(const u8*, u32, u32, u32) {}
snd::BankHandle snd_BankLoadFromIOPPartialEx_Completion() {
  return nullptr;
}
s32 snd_GetVoiceStatus(s32) {
  return 0;
}
s32 snd_GetFreeSPUDMA() {
  return 0;
}
void snd_FreeSPUDMA(s32) {}
void snd_keyOnVoiceRaw(u32, u32) {}
void snd_keyOffVoiceRaw(u32, u32) {}
s32 snd_GetSoundUserData(snd::BankHandle, char*, s32, char*, SFXUserData*) {
  return 0;
}
void snd_SetSoundReg(s32, s32, u8) {}
s8 snd_GetSoundGroup(s32) {
  return 0;
}
void snd_RegisterPluginHandler(snd::PluginHandler) {}

u32 sceSdGetSwitch(u32) {
  return 0;
}
u32 sceSdGetAddr(u32) {
  return 0;
}
void sceSdSetSwitch(u32, u32) {}
void sceSdSetAddr(u32, u32) {}
void sceSdSetParam(u32, u32) {}
void sceSdSetTransIntrHandler(s32, sceSdTransIntrHandler, void*) {}
u32 sceSdVoiceTrans(s32, s32, const void* source, u32 destination, u32 size) {
  if (source && destination <= sizeof(spu_memory) && size <= sizeof(spu_memory) - destination) {
    std::memcpy(spu_memory + destination, source, size);
  }
  return size;
}
