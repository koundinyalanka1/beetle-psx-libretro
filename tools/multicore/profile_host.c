/*
 * profile_host: run the real core with its own profiler on, off-device.
 *
 * The September captures leave CPU_Run unattributed - `event timing off` and
 * `per-word timing off` in every window - so nothing says how the 25-46 ms
 * splits between compiled execution, event dispatch and GP0 work. The core
 * already has that instrumentation; it is opt-in and nobody had anywhere to
 * run it, because device profiling is suspended and the Vulkan hosts need a
 * GPU. The software renderer needs neither, so this is a headless libretro
 * frontend with no GL, no Vulkan and no window: load the core, turn the
 * diagnostics on, run frames, print what the core reports.
 *
 * What transfers and what does not. Absolute microseconds are this host's,
 * not a TV's - a wide out-of-order core at ~4 GHz against an in-order one at
 * 1.53 GHz. What does transfer is the shape: how many events of each type a
 * frame dispatches, how many JIT quanta it takes and why they exit, how much
 * of a frame is dispatch overhead rather than emulated work, and how those
 * move when a workload changes. Ratios against the same build, on one host,
 * are the measurement; the millisecond column is context.
 *
 * Usage: profile_host <core> <content> [frames] [key=value ...]
 *   PROFILE_HOST_SYSTEM_DIR / PROFILE_HOST_SAVE_DIR override the directories.
 * Options default to the diagnostics the attribution needs; anything passed
 * on the command line wins, so a paired diagnostics-off control is
 * `profile_host <core> <rom> 900 beetle_psx_gpu_diagnostics=disabled`.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <dlfcn.h>
#include "libretro.h"
#include "testrom_report.h"

/* ---- option table ------------------------------------------------------ */
#define MAX_VARS 128
static char *var_key[MAX_VARS];
static char *var_val[MAX_VARS];
static int   n_vars;

static void set_var(const char *k, const char *v)
{
   int i;
   for (i = 0; i < n_vars; i++)
   {
      if (!strcmp(var_key[i], k))
      {
         free(var_val[i]);
         var_val[i] = strdup(v);
         return;
      }
   }
   if (n_vars < MAX_VARS)
   {
      var_key[n_vars] = strdup(k);
      var_val[n_vars] = strdup(v);
      n_vars++;
   }
}

static const char *get_var(const char *k)
{
   int i;
   for (i = 0; i < n_vars; i++)
      if (!strcmp(var_key[i], k))
         return var_val[i];
   return NULL;
}

/* ---- core binding ------------------------------------------------------ */
static void *core_lib;
static void (*p_set_environment)(retro_environment_t);
static void (*p_set_video_refresh)(retro_video_refresh_t);
static void (*p_set_audio_sample)(retro_audio_sample_t);
static void (*p_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_set_input_poll)(retro_input_poll_t);
static void (*p_set_input_state)(retro_input_state_t);
static void (*p_init)(void);
static void (*p_deinit)(void);
static bool (*p_load_game)(const struct retro_game_info *);
static void (*p_unload_game)(void);
static void (*p_run)(void);
static void (*p_get_system_av_info)(struct retro_system_av_info *);
static void *(*p_get_memory_data)(unsigned);
static size_t (*p_get_memory_size)(unsigned);

#define BIND(sym) do {                                              \
      *(void **)(&p_##sym) = dlsym(core_lib, "retro_" #sym);        \
      if (!p_##sym) { fprintf(stderr, "missing retro_" #sym "\n");  \
                      return 1; }                                   \
   } while (0)

/* ---- callbacks --------------------------------------------------------- */
static const char *system_dir;
static const char *save_dir;
static unsigned    frames_run;
/* Core log lines are the payload: the core_profile_* records arrive here. */
static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   vfprintf(stdout, fmt, ap);
   va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = core_log;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = system_dir;
         return system_dir != NULL;
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = save_dir;
         return save_dir != NULL;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *v = (struct retro_variable *)data;
         v->value = get_var(v->key);
         return v->value != NULL;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
         /* Both enabled: a skipped frame would not exercise the paths the
          * attribution is measuring. */
         *(int *)data = 3;
         return true;
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
         return true;
      default:
         /* Core options v0-v2 and everything else this frontend does not
          * implement: the core falls back to defaults, which is what the
          * option table above then overrides. */
         return false;
   }
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t p)
{ (void)d; (void)w; (void)h; (void)p; }
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}

int main(int argc, char **argv)
{
   struct retro_game_info info;
   struct retro_system_av_info av;
   const char *core_path, *content_path;
   double t0, elapsed;
   unsigned i;
   int a;

   if (argc < 3)
   {
      fprintf(stderr,
            "usage: %s <core> <content> [frames] [key=value ...]\n", argv[0]);
      return 2;
   }
   core_path    = argv[1];
   content_path = argv[2];
   frames_run   = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 10) : 900;

   system_dir = getenv("PROFILE_HOST_SYSTEM_DIR");
   save_dir   = getenv("PROFILE_HOST_SAVE_DIR");
   if (!save_dir)
      save_dir = system_dir;

   /* Defaults chosen so a bare run produces the attribution. `timing` turns
    * on event and JIT accounting; the rest pin the configuration the device
    * capture used so the counts are comparable. */
   set_var("beetle_psx_gpu_diagnostics",     "timing");
   set_var("beetle_psx_hw_gpu_diagnostics",  "timing");
   set_var("beetle_psx_cpu_dynarec",         "execute");
   set_var("beetle_psx_hw_cpu_dynarec",      "execute");
   set_var("beetle_psx_dynarec_eventcycles", "128");
   set_var("beetle_psx_hw_dynarec_eventcycles", "128");
   set_var("beetle_psx_dynarec_spu_samples", "1");
   set_var("beetle_psx_hw_dynarec_spu_samples", "1");
   set_var("beetle_psx_skip_bios",           "enabled");
   set_var("beetle_psx_hw_skip_bios",        "enabled");

   for (a = 4; a < argc; a++)
   {
      char *eq = strchr(argv[a], '=');
      if (!eq)
         continue;
      *eq = '\0';
      set_var(argv[a], eq + 1);
   }

   core_lib = dlopen(core_path, RTLD_NOW);
   if (!core_lib)
   {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 1;
   }

   BIND(set_environment);
   BIND(set_video_refresh);
   BIND(set_audio_sample);
   BIND(set_audio_sample_batch);
   BIND(set_input_poll);
   BIND(set_input_state);
   BIND(init);
   BIND(deinit);
   BIND(load_game);
   BIND(unload_game);
   BIND(run);
   BIND(get_system_av_info);
   BIND(get_memory_data);
   BIND(get_memory_size);

   p_set_environment(env_cb);
   p_set_video_refresh(video_cb);
   p_set_audio_sample(audio_cb);
   p_set_audio_sample_batch(audio_batch_cb);
   p_set_input_poll(input_poll_cb);
   p_set_input_state(input_state_cb);

   p_init();

   memset(&info, 0, sizeof(info));
   info.path = content_path;
   if (!p_load_game(&info))
   {
      fprintf(stderr, "retro_load_game failed for %s\n", content_path);
      p_deinit();
      return 1;
   }

   p_get_system_av_info(&av);
   printf("profile_host: %ux%u fps=%.4f sample_rate=%.0f frames=%u\n",
         av.geometry.base_width, av.geometry.base_height,
         av.timing.fps, av.timing.sample_rate, frames_run);

   t0 = now_ms();
   for (i = 0; i < frames_run; i++)
      p_run();
   elapsed = now_ms() - t0;

   printf("profile_host: %u frames in %.1f ms = %.3f ms/frame "
          "(%.1f%% of a %.3f ms native period on THIS host)\n",
         frames_run, elapsed, elapsed / (double)frames_run,
         100.0 * (elapsed / (double)frames_run) * av.timing.fps / 1000.0,
         1000.0 / av.timing.fps);

   /* The generated ROM self-checks: it draws primitives whose VRAM result is
    * exactly predictable, reads them back and reports mismatches. Timing a
    * scheduler change is only meaningful next to that verdict, so the harness
    * refuses to be a stopwatch alone. */
   {
      int failed = testrom_check_report(
            (const unsigned char *)p_get_memory_data(RETRO_MEMORY_SYSTEM_RAM),
            p_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
      p_unload_game();
      p_deinit();
      dlclose(core_lib);
      return failed ? 1 : 0;
   }
}
