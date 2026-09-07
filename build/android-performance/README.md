Android performance build

Built with NDK 28.2.13676358, API 21, Vulkan + GLES3 + Lightrec,
for armeabi-v7a, arm64-v8a and x86_64. Each .so has 16 KB LOAD alignment.
Use the .so matching the frontend process ABI.

Changes in this build:
- GL texture dependencies persist across frames. Only required regions are
  copied on the GPU; the unconditional full-VRAM copy and CPU table clear
  during presentation are removed.
- Lightrec keeps running through completed unmapped guest accesses. Repeated
  failures with no emulated-cycle progress still invoke interpreter recovery.
- Routine unmapped accesses no longer flood frontend logging callbacks.
- Existing inline GP0 accounting is retained; per-word timing stays opt-in.

Assumes the frontend supplies a working GPU presentation surface on ARM32.
The frontend was not modified. No new per-frame CPU readback is introduced.
Guest VRAM readback and state serialization retain their required barriers.
Libretro memory exports and RetroAchievements memory-map interfaces are unchanged.

Validation:
- All three Android builds passed; required libretro exports verified.
- SPU/worker deterministic state, audio, IRQ and lifecycle tests passed.
- macOS OpenGL: 19 GPU cases / 701 words passed at 1x, 2x, with context
  recreation, and with the software framebuffer enabled.
- macOS Lightrec execute and run_interpreter: 1024 unmapped loads followed
  by all 19 GPU checks passed without interpreter fallback.

No new physical TV/phone or RetroAchievements service integration test was run.
These checks do not establish a frame-rate gain or universal game compatibility.
