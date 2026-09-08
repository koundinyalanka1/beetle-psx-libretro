/* vkhost: headless libretro Vulkan frontend for renderer validation.
 *
 * Loads a libretro core with a Vulkan hardware context on whatever Vulkan
 * device is present (lavapipe in CI), with VK_LAYER_KHRONOS_validation
 * active and every message printed. Runs content, optionally loads a
 * savestate, and dumps the frame the core hands back through set_image()
 * as a PPM (tonemapped naively when the image is fp16/10-bit).
 *
 * This exists because two Vulkan regressions in a row shipped compile-clean:
 * a missing vertex attribute and a spec-constant capacity overflow, both of
 * which the validation layer reports by name on the first draw. The rule it
 * enforces: renderer changes get executed, not just compiled.
 *
 * Usage: vkhost <core.so> <content> [savestate] [frames] [outdir]
 *   VKHOST_VARS: semicolon list of key=value core option overrides.
 *   VKHOST_SYSTEM_DIR / VKHOST_SAVE_DIR: BIOS and save directories.
 *   VKHOST_HASHES=1: per-frame main RAM, scratchpad and audio FNV-1a hashes.
 *   VKHOST_ROUNDTRIP_FRAME=N: serialize/unserialize after frame N (1-based).
 *   VKHOST_RESET_FRAME=N: repeat context_reset after frame N.
 *   VKHOST_RECREATE_FRAME=N: destroy/renegotiate/reset after frame N.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include "libretro.h"
#include "libretro_vulkan.h"

/* ---- tiny dynamic table of core option overrides ---- */
static char *var_keys[512]; static char *var_vals[512]; static int n_vars;
static void add_var(const char *k, const char *v)
{ if (n_vars < 512) { var_keys[n_vars] = strdup(k); var_vals[n_vars] = strdup(v); n_vars++; } }
static const char *find_var(const char *k)
{ int i; for (i = 0; i < n_vars; i++) if (!strcmp(var_keys[i], k)) return var_vals[i]; return NULL; }

/* ---- state ---- */
static void *core;
static struct retro_hw_render_callback hw_render;
static const struct retro_hw_render_context_negotiation_interface_vulkan *negotiation;
static struct retro_hw_render_interface_vulkan iface;
static struct retro_vulkan_context vkctx;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDebugUtilsMessengerEXT messenger;
static int validation_errors, validation_warnings;
static bool validation_active;
static const struct retro_vulkan_image *last_image;
static unsigned frame_valid;
static unsigned last_w, last_h;
static char sysdir[512] = "/tmp/vkhost_sys";
static char savedir[512] = "/tmp/vkhost_save";
static struct retro_memory_descriptor *memory_descs;
static unsigned memory_desc_count;
static bool hash_frames;
#define FNV64_OFFSET UINT64_C(14695981039346656037)
#define FNV64_PRIME UINT64_C(1099511628211)
static uint64_t audio_hash = FNV64_OFFSET;
static size_t audio_frames;
static bool benchmark;
static uint64_t total_audio_frames, measured_audio_frames;
static bool measuring;
static FILE *audio_file;
static uint64_t wav_frames;
static uint32_t audio_rate = 44100;
static double reported_fps, reported_sample_rate;
static uint64_t timing_changes;
#define MAX_TIMING_SAMPLES 1000000u

struct timing_stats
{
   uint64_t *values;
   uint64_t total, min, max;
   size_t count, capacity;
};

static void fatal(const char *message)
{
   fprintf(stderr, "[vkhost] %s\n", message);
   exit(6);
}

static bool decimal_u32(const char *text, uint32_t limit, uint32_t *result)
{
   uint32_t value = 0;
   const unsigned char *p = (const unsigned char *)text;
   if (!p || !*p) return false;
   for (; *p; p++)
   {
      uint32_t digit;
      if (*p < '0' || *p > '9') return false;
      digit = *p - '0';
      if (digit > limit || value > (limit - digit) / 10) return false;
      value = value * 10 + digit;
   }
   *result = value;
   return true;
}

static uint64_t monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
         (uint64_t)now.tv_sec > (UINT64_MAX - 999999999u) / 1000000000u ||
         now.tv_nsec < 0 || now.tv_nsec >= 1000000000)
      fatal("monotonic clock failed");
   return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t elapsed_ns(uint64_t begin, uint64_t end)
{
   if (end < begin) fatal("monotonic clock moved backwards");
   return end - begin;
}

static void timing_init(struct timing_stats *stats, size_t capacity)
{
   if (!capacity || capacity > MAX_TIMING_SAMPLES ||
         capacity > SIZE_MAX / sizeof(*stats->values))
      fatal("benchmark requires 1..1000000 measured frames");
   memset(stats, 0, sizeof(*stats));
   stats->values = calloc(capacity, sizeof(*stats->values));
   if (!stats->values) fatal("cannot allocate benchmark timings");
   stats->capacity = capacity;
   stats->min = UINT64_MAX;
}

static void timing_add(struct timing_stats *stats, uint64_t ns)
{
   if (stats->count >= stats->capacity || UINT64_MAX - stats->total < ns)
      fatal("benchmark timing overflow");
   stats->values[stats->count++] = ns;
   stats->total += ns;
   if (ns < stats->min) stats->min = ns;
   if (ns > stats->max) stats->max = ns;
}

static int timing_compare(const void *a, const void *b)
{
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return (x > y) - (x < y);
}

static void timing_print(struct timing_stats *stats)
{
   size_t n = stats->count;
   if (!n) fatal("benchmark has no timing samples");
   qsort(stats->values, n, sizeof(*stats->values), timing_compare);
   printf("{\"sample_count\":%zu,\"total\":%" PRIu64
         ",\"mean\":%.3f,\"min\":%" PRIu64 ",\"max\":%" PRIu64
         ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 "}",
         n, stats->total, (double)stats->total / n, stats->min, stats->max,
         stats->values[(n * 50 + 99) / 100 - 1],
         stats->values[(n * 95 + 99) / 100 - 1],
         stats->values[(n * 99 + 99) / 100 - 1]);
}

static void put_le16(uint8_t *p, uint16_t value)
{
   p[0] = (uint8_t)value;
   p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
   put_le16(p, (uint16_t)value);
   put_le16(p + 2, (uint16_t)(value >> 16));
}

static void wav_header(void)
{
   uint8_t header[44] = {0};
   uint32_t bytes;
   if (wav_frames > (UINT32_MAX - 36u) / 4u)
      fatal("WAV exceeds RIFF size limit");
   bytes = (uint32_t)(wav_frames * 4u);
   memcpy(header, "RIFF", 4);
   put_le32(header + 4, bytes + 36u);
   memcpy(header + 8, "WAVEfmt ", 8);
   put_le32(header + 16, 16);
   put_le16(header + 20, 1);
   put_le16(header + 22, 2);
   put_le32(header + 24, audio_rate);
   put_le32(header + 28, audio_rate * 4u);
   put_le16(header + 32, 4);
   put_le16(header + 34, 16);
   memcpy(header + 36, "data", 4);
   put_le32(header + 40, bytes);
   if (fseek(audio_file, 0, SEEK_SET) != 0 ||
         fwrite(header, 1, sizeof(header), audio_file) != sizeof(header))
      fatal("cannot write WAV header");
}

static void wav_set_rate(double rate)
{
   uint32_t value;
   if (!(rate >= 1.0 && rate <= UINT32_MAX / 4u))
      fatal("WAV sample rate is invalid");
   value = (uint32_t)rate;
   if ((double)value != rate) fatal("WAV requires an integer sample rate");
   if (wav_frames && value != audio_rate)
      fatal("WAV sample rate changed after capture started");
   audio_rate = value;
}

static void note_timing(double fps, double sample_rate)
{
   if (benchmark && (!(fps > 0.0 && fps <= 1000.0) ||
         !(sample_rate > 0.0 && sample_rate <= 1000000000.0)))
      fatal("invalid core AV timing");
   if (reported_fps > 0.0 &&
         (fps != reported_fps || sample_rate != reported_sample_rate))
      timing_changes++;
   reported_fps = fps;
   reported_sample_rate = sample_rate;
   if (audio_file)
      wav_set_rate(sample_rate);
}

static void wav_write(const int16_t *data, size_t frames)
{
   uint8_t buffer[4096];
   size_t remaining = frames;
   if (wav_frames > (UINT32_MAX - 36u) / 4u ||
         frames > (UINT32_MAX - 36u) / 4u - wav_frames)
      fatal("WAV exceeds RIFF size limit");
   while (remaining)
   {
      size_t i, chunk = remaining > sizeof(buffer) / 4 ? sizeof(buffer) / 4 : remaining;
      for (i = 0; i < chunk * 2; i++) put_le16(buffer + i * 2, (uint16_t)data[i]);
      if (fwrite(buffer, 4, chunk, audio_file) != chunk)
         fatal("cannot write WAV samples");
      data += chunk * 2;
      remaining -= chunk;
   }
   wav_frames += frames;
}

static void wav_close(void)
{
   if (!audio_file) return;
   wav_header();
   if (fflush(audio_file) != 0) fatal("cannot flush WAV output");
   if (fclose(audio_file) != 0) fatal("cannot close WAV output");
   audio_file = NULL;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
   const uint8_t *p = (const uint8_t *)data;
   while (size--) { hash ^= *p++; hash *= FNV64_PRIME; }
   return hash;
}

static uint64_t memory_hash(size_t start, size_t *size)
{
   unsigned i;
   *size = 0;
   for (i = 0; i < memory_desc_count; i++)
   {
      const struct retro_memory_descriptor *desc = &memory_descs[i];
      if (desc->start == start && desc->ptr &&
            (desc->flags & RETRO_MEMDESC_SYSTEM_RAM))
      {
         *size = desc->len;
         return hash_bytes(FNV64_OFFSET,
               (const uint8_t *)desc->ptr + desc->offset, desc->len);
      }
   }
   return FNV64_OFFSET;
}

static bool make_directory(const char *path)
{
   char buf[1024];
   char *p;
   if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
      return false;
   for (p = buf + 1; *p; p++)
   {
      if (*p != '/') continue;
      *p = 0;
      if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
      *p = '/';
   }
   return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

/* ---- validation output ---- */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(
      VkDebugUtilsMessageSeverityFlagBitsEXT sev,
      VkDebugUtilsMessageTypeFlagsEXT types,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *ud)
{
   (void)types; (void)ud;
   if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   { validation_errors++;   fprintf(stderr, "[VVL:ERROR] %s\n", data->pMessage); }
   else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
   { validation_warnings++; fprintf(stderr, "[VVL:WARN ] %s\n", data->pMessage); }
   return VK_FALSE;
}

/* ---- log passthrough ---- */
static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap; (void)level;
   if (benchmark) return;
   va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* ---- hw render interface the core consumes ---- */
#define MAX_FRAME_SEMAPHORES 64u
#define MAX_FRAME_COMMAND_BUFFERS 4096u
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static VkSemaphore frame_semaphores[MAX_FRAME_SEMAPHORES], signal_semaphore;
static VkCommandBuffer frame_commands[MAX_FRAME_COMMAND_BUFFERS + 1];
static uint32_t frame_semaphore_count, frame_command_count, image_queue_family;
static bool image_pending;
static VkImage pending_image;
static VkImageLayout pending_layout;
static VkImageSubresourceRange pending_range;
static VkCommandPool ownership_pool;
static VkCommandBuffer ownership_command;

static void vk_check(VkResult result, const char *operation)
{
   if (result != VK_SUCCESS)
   {
      fprintf(stderr, "[vkhost] %s failed: VkResult %d\n", operation, (int)result);
      exit(6);
   }
}

static void vk_lock_queue(void *handle)
{
   (void)handle;
   if (benchmark && pthread_mutex_lock(&queue_mutex) != 0) fatal("queue lock failed");
}

static void vk_unlock_queue(void *handle)
{
   (void)handle;
   if (benchmark && pthread_mutex_unlock(&queue_mutex) != 0) fatal("queue unlock failed");
}

static void vk_wait_sync_index(void *handle)
{
   (void)handle;
   if (!benchmark || !vkctx.device) return;
   vk_lock_queue(NULL);
   vk_check(vkQueueWaitIdle(vkctx.queue), "vkQueueWaitIdle");
   vk_unlock_queue(NULL);
}

static void vk_set_image(void *handle, const struct retro_vulkan_image *image,
      uint32_t num_semaphores, const VkSemaphore *semaphores, uint32_t src_queue_family)
{
   (void)handle;
   if (benchmark)
   {
      if (!image || num_semaphores > MAX_FRAME_SEMAPHORES ||
            (num_semaphores && !semaphores) || image_pending)
         fatal("invalid or overlapping Vulkan image handoff");
      pending_image = image->create_info.image;
      pending_layout = image->image_layout;
      pending_range = image->create_info.subresourceRange;
      image_queue_family = src_queue_family;
      frame_semaphore_count = num_semaphores;
      if (num_semaphores)
         memcpy(frame_semaphores, semaphores, num_semaphores * sizeof(*semaphores));
      image_pending = true;
      return;
   }
   if (!last_image || last_image->create_info.image != image->create_info.image ||
       last_image->create_info.format != image->create_info.format)
      fprintf(stderr, "[vkhost] set_image img=%p fmt=%d layout=%d extent-hint=%ux%u\n",
              (void*)image->create_info.image, (int)image->create_info.format,
              (int)image->image_layout, last_w, last_h);
   last_image = image;
}
static uint32_t vk_get_sync_index(void *handle) { (void)handle; return 0; }
static uint32_t vk_get_sync_index_mask(void *handle) { (void)handle; return 1; }
static void vk_set_command_buffers(void *handle, uint32_t num, const VkCommandBuffer *cmd)
{
   (void)handle;
   if (!benchmark) return;
   if (num > MAX_FRAME_COMMAND_BUFFERS - frame_command_count || (num && !cmd))
      fatal("invalid or oversized Vulkan command buffer handoff");
   if (num) memcpy(frame_commands + frame_command_count, cmd, num * sizeof(*cmd));
   frame_command_count += num;
}
static void vk_set_signal_semaphore(void *handle, VkSemaphore sem)
{
   (void)handle;
   if (benchmark) signal_semaphore = sem;
}

static void benchmark_video(const void *data)
{
   VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   VkPipelineStageFlags stages[MAX_FRAME_SEMAPHORES];
   uint32_t i;
   bool use_image = data == RETRO_HW_FRAME_BUFFER_VALID && image_pending;
   if (!vkctx.device) return;
   submit.waitSemaphoreCount = use_image && !frame_command_count ? frame_semaphore_count : 0;
   submit.pWaitSemaphores = frame_semaphores;
   for (i = 0; i < submit.waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   submit.pWaitDstStageMask = stages;
   if (submit.waitSemaphoreCount && image_queue_family != VK_QUEUE_FAMILY_IGNORED &&
         image_queue_family != vkctx.queue_family_index)
   {
      VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      vk_wait_sync_index(NULL);
      if (!ownership_pool)
      {
         VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
         VkCommandBufferAllocateInfo alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
         pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
         pool.queueFamilyIndex = vkctx.queue_family_index;
         vk_check(vkCreateCommandPool(vkctx.device, &pool, NULL, &ownership_pool), "vkCreateCommandPool");
         alloc.commandPool = ownership_pool;
         alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
         alloc.commandBufferCount = 1;
         vk_check(vkAllocateCommandBuffers(vkctx.device, &alloc, &ownership_command), "vkAllocateCommandBuffers");
      }
      vk_check(vkResetCommandBuffer(ownership_command, 0), "vkResetCommandBuffer");
      vk_check(vkBeginCommandBuffer(ownership_command, &begin), "vkBeginCommandBuffer");
      barrier.oldLayout = barrier.newLayout = pending_layout;
      barrier.srcQueueFamilyIndex = image_queue_family;
      barrier.dstQueueFamilyIndex = vkctx.queue_family_index;
      barrier.image = pending_image;
      barrier.subresourceRange = pending_range;
      vkCmdPipelineBarrier(ownership_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
      barrier.srcQueueFamilyIndex = vkctx.queue_family_index;
      barrier.dstQueueFamilyIndex = image_queue_family;
      vkCmdPipelineBarrier(ownership_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
      vk_check(vkEndCommandBuffer(ownership_command), "vkEndCommandBuffer");
      frame_commands[frame_command_count++] = ownership_command;
   }
   submit.commandBufferCount = frame_command_count;
   submit.pCommandBuffers = frame_commands;
   submit.signalSemaphoreCount = signal_semaphore != VK_NULL_HANDLE ? 1 : 0;
   submit.pSignalSemaphores = &signal_semaphore;
   if (submit.waitSemaphoreCount || submit.commandBufferCount || submit.signalSemaphoreCount)
   {
      vk_lock_queue(NULL);
      vk_check(vkQueueSubmit(vkctx.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
      vk_unlock_queue(NULL);
   }
   frame_command_count = frame_semaphore_count = 0;
   signal_semaphore = VK_NULL_HANDLE;
   image_pending = false;
}

/* ---- environment ---- */
static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_cb; return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = sysdir; return true;
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = savedir; return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true; return true;
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
      {
         const struct retro_memory_map *map = (const struct retro_memory_map *)data;
         struct retro_memory_descriptor *copy = NULL;
         if (map->num_descriptors)
         {
            copy = malloc(map->num_descriptors * sizeof(*copy));
            if (!copy) return false;
            memcpy(copy, map->descriptors, map->num_descriptors * sizeof(*copy));
         }
         free(memory_descs);
         memory_descs = copy;
         memory_desc_count = map->num_descriptors;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      {
         const struct retro_variable *v = (const struct retro_variable *)data;
         for (; v && v->key; v++)
         {
            /* value format: "Label; default|alt|..." */
            const char *semi = strchr(v->value, ';');
            if (semi && !find_var(v->key))
            {
               char buf[256]; const char *p = semi + 1; size_t n = 0;
               while (*p == ' ') p++;
               while (p[n] && p[n] != '|' && n < sizeof(buf) - 1) n++;
               memcpy(buf, p, n); buf[n] = 0;
               add_var(v->key, buf);
            }
         }
         return true;
      }
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
      {
         const struct retro_core_options_v2 *o2 =
            (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL)
            ? ((const struct retro_core_options_v2_intl *)data)->us
            : (const struct retro_core_options_v2 *)data;
         const struct retro_core_option_v2_definition *d;
         if (!o2) return true;
         for (d = o2->definitions; d && d->key; d++)
            if (d->default_value && !find_var(d->key))
               add_var(d->key, d->default_value);
         return true;
      }
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
         if (audio_file || benchmark)
         {
            const struct retro_system_av_info *av = data;
            note_timing(av->timing.fps, av->timing.sample_rate);
         }
         return true;
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         const char *v = find_var(var->key);
         var->value = v;
         return v != NULL;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false; return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (cb->context_type != RETRO_HW_CONTEXT_VULKAN) return false;
         hw_render = *cb;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
         negotiation = (const struct retro_hw_render_context_negotiation_interface_vulkan *)data;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         *(const struct retro_hw_render_interface_vulkan **)data = &iface;
         return true;
      case RETRO_ENVIRONMENT_GET_HDR_PAPER_WHITE_NITS:
         *(float *)data = 200.0f; return true;
      case RETRO_ENVIRONMENT_GET_HDR_MAX_NITS:
         *(float *)data = 1000.0f; return true;
      case RETRO_ENVIRONMENT_GET_HDR_EXPAND_GAMUT:
         *(bool *)data = false; return true;
      case RETRO_ENVIRONMENT_GET_HDR_OUTPUT_MODE:
         *(unsigned *)data = 1; return true; /* HDR10 */
      default:
         return false;
   }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{ (void)pitch;
   if (benchmark) benchmark_video(data);
   if (data == RETRO_HW_FRAME_BUFFER_VALID) { frame_valid++;
   if (!benchmark && (last_w != width || last_h != height))
      fprintf(stderr, "[vkhost] geometry %ux%u\n", width, height);
   last_w = width; last_h = height; } }
static void input_poll_cb(void) {}
static int cur_frame;
/* VKHOST_INPUT: comma list of first-last:joypad_id held ranges, e.g.
 * "20-30:3,60-300:4" (3=START, 4=UP per RETRO_DEVICE_ID_JOYPAD_*). */
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   const char *e = getenv("VKHOST_INPUT");
   (void)device; (void)index;
   if (port != 0 || !e) return 0;
   {
      char buf[256]; char *tok;
      strncpy(buf, e, sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
      tok = strtok(buf, ",");
      while (tok)
      {
         int a, b; unsigned bid;
         if (sscanf(tok, "%d-%d:%u", &a, &b, &bid) == 3 &&
             cur_frame >= a && cur_frame <= b && bid == id)
            return 1;
         tok = strtok(NULL, ",");
      }
   }
   return 0;
}
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
   if (frames > SIZE_MAX / (2 * sizeof(*data)) || (frames && !data) ||
         frames > UINT64_MAX - total_audio_frames ||
         (measuring && frames > UINT64_MAX - measured_audio_frames))
      fatal("audio sample count overflow or invalid buffer");
   total_audio_frames += frames;
   if (measuring) measured_audio_frames += frames;
   if (audio_file) wav_write(data, frames);
   if (hash_frames && !benchmark)
   {
      audio_hash = hash_bytes(audio_hash, data, frames * 2 * sizeof(*data));
      audio_frames += frames;
   }
   return frames;
}
static void audio_cb(int16_t l, int16_t r)
{
   int16_t samples[2] = { l, r };
   audio_batch_cb(samples, 1);
}

/* ---- vulkan bring-up ---- */
static PFN_vkGetInstanceProcAddr gipa;
static bool create_instance(void)
{
   const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
   const char *exts[]   = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   bool debug_utils = false;
   uint32_t count = 0, i;


   app.pApplicationName = "vkhost";
   app.apiVersion = VK_API_VERSION_1_2;
   if (negotiation && negotiation->get_application_info)
   {
      const VkApplicationInfo *ai = negotiation->get_application_info();
      if (ai) app = *ai;
   }
   ci.pApplicationInfo = &app;
   /* VKHOST_NO_VALIDATION=1 drops the layer: needed under ThreadSanitizer,
    * where the layer's own rwlock teardown races and aborts the run. */
   if (!benchmark && !getenv("VKHOST_NO_VALIDATION") &&
         vkEnumerateInstanceLayerProperties(&count, NULL) == VK_SUCCESS && count)
   {
      VkLayerProperties *props = calloc(count, sizeof(*props));
      if (props && vkEnumerateInstanceLayerProperties(&count, props) == VK_SUCCESS)
         for (i = 0; i < count; i++)
            if (!strcmp(props[i].layerName, layers[0])) validation_active = true;
      free(props);
   }
   fprintf(stderr, "[vkhost] Vulkan validation: %s\n",
         validation_active ? "enabled" : "disabled (no validation result)");
   ci.enabledLayerCount = validation_active ? 1 : 0;
   ci.ppEnabledLayerNames = layers;
   count = 0;
   if (!benchmark && vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count)
   {
      VkExtensionProperties *props = calloc(count, sizeof(*props));
      if (props && vkEnumerateInstanceExtensionProperties(NULL, &count, props) == VK_SUCCESS)
         for (i = 0; i < count; i++)
            if (!strcmp(props[i].extensionName, exts[0])) debug_utils = true;
      free(props);
   }
   ci.enabledExtensionCount = debug_utils ? 1 : 0;
   ci.ppEnabledExtensionNames = exts;
   if (vkCreateInstance(&ci, NULL, &instance) != VK_SUCCESS)
      return false;

   {
      PFN_vkCreateDebugUtilsMessengerEXT cdm = (PFN_vkCreateDebugUtilsMessengerEXT)
         vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
      VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
      mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
      mi.pfnUserCallback = debug_cb;
      if (debug_utils && cdm) cdm(instance, &mi, NULL, &messenger);
   }

   {
      uint32_t n = 0; VkPhysicalDevice devs[8];
      vkEnumeratePhysicalDevices(instance, &n, NULL);
      if (!n) return false;
      if (n > 8) n = 8;
      vkEnumeratePhysicalDevices(instance, &n, devs);
      {
         /* Not GRANITE_VULKAN_DEVICE_INDEX: that one is read by the core's
          * own device creation, which the negotiation interface bypasses -
          * the harness picks the GPU here and hands it to create_device. */
         const char *e  = getenv("VKHOST_DEVICE_INDEX");
         uint32_t    ix = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
         if (ix >= n)
            ix = 0;
         gpu = devs[ix];
      }
      {
         VkPhysicalDeviceProperties props;
         vkGetPhysicalDeviceProperties(gpu, &props);
         fprintf(stderr, "[vkhost] device: %s\n", props.deviceName);
      }
   }
   return true;
}

/* ---- frame dump: copy last_image to host memory and write PPM ---- */
/* The .raw next to each .ppm holds the exact copied bytes; decode it by the
 * numeric VkFormat in the log line, not by assumption. The trap that cost
 * this harness two debugging sessions: VkFormat 64 is
 * VK_FORMAT_A2B10G10R10_UNORM_PACK32 - 4-byte packed texels - while
 * R16G16B16A16_SFLOAT is 97. The core's HDR scanout prefers A2B10G10R10
 * when the device supports it (renderer_hdr_scanout_format), so an "HDR"
 * raw is normally 10-bit packed PQ, not fp16. Reading those 4-byte texels
 * as 8-byte half-float pairs manufactures a doubled side-by-side picture
 * over a dead lower half, +/-50000 pseudo-values, NaNs, and an
 * always-wrong alpha lane - which a correct 10-10-10-2 decode of the same
 * bytes reveals to be an ordinary, healthy PQ frame. */
static int dump_frame(const char *path)
{
   VkDevice dev = vkctx.device;
   VkQueue queue = vkctx.queue;
   uint32_t qfam = vkctx.queue_family_index;
   VkImage img;
   VkExtent2D ext;
   VkCommandPool pool; VkCommandBuffer cmd;
   VkBuffer buf; VkDeviceMemory mem;
   VkDeviceSize size;
   VkFormat fmt;
   if (!last_image) { fprintf(stderr, "[vkhost] no image set\n"); return -1; }
   /* The interface hands semaphores with set_image; a real frontend waits
    * them on its own submission. A harness can afford the sledgehammer. */
   vkDeviceWaitIdle(dev);
   img = last_image->create_info.image;
   fmt = last_image->create_info.format;
   { const char *e = getenv("VKHOST_DUMP_WH");
     if (e) sscanf(e, "%ux%u", &ext.width, &ext.height);
     else { ext.width = last_w ? last_w : 1024; ext.height = last_h ? last_h : 512; } }
   size = (VkDeviceSize)ext.width * ext.height * 8 + 65536; /* room for fp16 RGBA */

   { VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
     pi.queueFamilyIndex = qfam;
     vkCreateCommandPool(dev, &pi, NULL, &pool); }
   { VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
     ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
     vkAllocateCommandBuffers(dev, &ai, &cmd); }
   { VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
     bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
     vkCreateBuffer(dev, &bi, NULL, &buf); }
   { VkMemoryRequirements req; VkPhysicalDeviceMemoryProperties mp;
     VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
     uint32_t i;
     vkGetBufferMemoryRequirements(dev, buf, &req);
     vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
     mi.allocationSize = req.size;
     for (i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        { mi.memoryTypeIndex = i; break; }
     vkAllocateMemory(dev, &mi, NULL, &mem);
     vkBindBufferMemory(dev, buf, mem, 0); }

   { VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
     VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
     VkBufferImageCopy copy;
     vkBeginCommandBuffer(cmd, &bi);
     bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
     bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
     bar.oldLayout = last_image->image_layout;
     bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
     bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
     bar.image = img;
     bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
     bar.subresourceRange.levelCount = 1;
     bar.subresourceRange.layerCount = 1;
     vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &bar);
     memset(&copy, 0, sizeof(copy));
     copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
     copy.imageSubresource.layerCount = 1;
     copy.imageOffset.x = 0;
     copy.imageOffset.y = 0;
     copy.imageExtent.width  = ext.width;
     copy.imageExtent.height = ext.height;
     copy.imageExtent.depth  = 1;
     vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
     bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
     bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
     bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
     bar.newLayout = last_image->image_layout;
     vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &bar);
     vkEndCommandBuffer(cmd); }

   { VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
     VkFence fence; VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
     si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
     vkCreateFence(dev, &fi, NULL, &fence);
     vkQueueSubmit(queue, 1, &si, fence);
     vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
     vkDestroyFence(dev, fence, NULL); }

   { void *map = NULL; FILE *f; unsigned x, y;
     vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
     { char rawpath[640]; FILE *rf;
       if (!getenv("VKHOST_DUMP_RAW")) rawpath[0] = 0;
       else snprintf(rawpath, sizeof(rawpath), "%s.raw", path);
       rf = rawpath[0] ? fopen(rawpath, "wb") : NULL;
       if (rf) { fwrite(map, 1, (size_t)size, rf); fclose(rf); } }
     f = fopen(path, "wb");
     if (!f)
     {
        /* A full disk here used to crash the harness inside fprintf(NULL):
         * the .raw fopen above was guarded, this one was not. */
        fprintf(stderr, "[vkhost] cannot open %s for writing\n", path);
        vkUnmapMemory(dev, mem);
        vkDestroyBuffer(dev, buf, NULL);
        vkFreeMemory(dev, mem, NULL);
        return 0;
     }
     fprintf(f, "P6\n%u %u\n255\n", ext.width, ext.height);
     for (y = 0; y < ext.height; y++)
        for (x = 0; x < ext.width; x++)
        {
           unsigned char px[3] = {0,0,0};
           if (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM)
           {
              const unsigned char *p = (const unsigned char *)map + (y * (size_t)ext.width + x) * 4;
              if (fmt == VK_FORMAT_B8G8R8A8_UNORM) { px[0]=p[2]; px[1]=p[1]; px[2]=p[0]; }
              else { px[0]=p[0]; px[1]=p[1]; px[2]=p[2]; }
           }
           else if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
           {
              uint32_t v = ((const uint32_t *)map)[y * (size_t)ext.width + x];
              px[0] = (unsigned char)(((v      ) & 0x3FF) >> 2);
              px[1] = (unsigned char)(((v >> 10) & 0x3FF) >> 2);
              px[2] = (unsigned char)(((v >> 20) & 0x3FF) >> 2);
           }
           else if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT)
           {
              const uint16_t *p = (const uint16_t *)map + (y * (size_t)ext.width + x) * 4;
              unsigned c;
              for (c = 0; c < 3; c++)
              {
                 uint16_t h = p[c];
                 int e = (h >> 10) & 0x1F; int m = h & 0x3FF;
                 float v = 0.0f;
                 if (e) v = (float)(m + 1024) * (1.0f / 1024.0f) * (float)(1 << e) / 32768.0f;
                 else   v = (float)m * (1.0f / 1024.0f) / 16384.0f;
                 if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
                 px[c] = (unsigned char)(v * 255.0f + 0.5f);
              }
           }
           fwrite(px, 1, 3, f);
        }
     fclose(f);
     vkUnmapMemory(dev, mem); }

   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   fprintf(stderr, "[vkhost] dumped %s (%ux%u fmt %d)\n", path, ext.width, ext.height, (int)fmt);
   return 0;
}

typedef void (*set_env_t)(retro_environment_t);
typedef void (*set_cb_t)(void *);

int main(int argc, char **argv)
{
   const char *core_path, *content;
   const char *state_path = NULL;
   int frames = 120;
   const char *outdir = "/tmp/vkhost_out";
   char cmdbuf[1024];
   uint32_t warmup = 0;
   uint64_t measured_start = 0, measured_end = 0;
   int summary_fd = -1;
   struct timing_stats core_times = {0}, frame_times = {0};
   const char *audio_path = getenv("VKHOST_AUDIO_PATH");
   const char *benchmark_env = getenv("VKHOST_BENCHMARK");

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s core content [state] [frames] [outdir]\n"
            "VKHOST_BENCHMARK=1: no validation, hashing, readback, capture or output directories; JSON on stdout.\n"
            "VKHOST_WARMUP_FRAMES=N: decimal warmup within total frames (default 0); 1..1000000 measured frames.\n"
            "Benchmark frame times include single-index Vulkan queue completion, not presentation.\n"
            "VKHOST_AUDIO_PATH=path: PCM16 stereo WAV at the core AV sample rate (not in benchmark mode).\n", argv[0]);
      return 2;
   }
   if (benchmark_env && strcmp(benchmark_env, "0") && strcmp(benchmark_env, "1"))
      fatal("VKHOST_BENCHMARK must be 0 or 1");
   benchmark = benchmark_env && !strcmp(benchmark_env, "1");
   core_path = argv[1]; content = argv[2];
   if (argc > 3 && strcmp(argv[3], "-")) state_path = argv[3];
   if (benchmark)
   {
      uint32_t count = 120;
      const char *warmup_env = getenv("VKHOST_WARMUP_FRAMES");
      const char *incompatible[] = { "VKHOST_AUDIO_PATH", "VKHOST_SAVE_STATE",
         "VKHOST_ROUNDTRIP_FRAME", "VKHOST_RESET_FRAME", "VKHOST_RECREATE_FRAME",
         "VKHOST_BENCH", "VKHOST_BENCH_SKIP", "VKHOST_CORE_DUMP" };
      size_t i;
      if (argc > 4 && (!decimal_u32(argv[4], INT_MAX, &count) || !count))
         fatal("benchmark frames must be a positive decimal integer <= INT_MAX");
      frames = (int)count;
      if (warmup_env && !decimal_u32(warmup_env, INT_MAX, &warmup))
         fatal("VKHOST_WARMUP_FRAMES must be a nonnegative decimal integer <= INT_MAX");
      if (warmup >= count) fatal("warmup must be smaller than total frames");
      for (i = 0; i < sizeof(incompatible) / sizeof(incompatible[0]); i++)
         if (getenv(incompatible[i]))
         {
            fprintf(stderr, "[vkhost] %s is incompatible with VKHOST_BENCHMARK=1\n", incompatible[i]);
            return 2;
         }
      timing_init(&core_times, count - warmup);
      timing_init(&frame_times, count - warmup);
      if (setenv("VK_LOADER_LAYERS_DISABLE", "*", 1) != 0 ||
            unsetenv("VK_INSTANCE_LAYERS") != 0 || unsetenv("VK_LOADER_LAYERS_ENABLE") != 0)
         fatal("cannot disable Vulkan loader layers");
      fprintf(stderr, "[vkhost] benchmark: validation, hashes, readback, dumps and output directories disabled; warmup=%u; sync=queue_idle_per_frame\n", warmup);
   }
   else
   {
      if (getenv("VKHOST_WARMUP_FRAMES")) fatal("VKHOST_WARMUP_FRAMES requires VKHOST_BENCHMARK=1");
      if (argc > 4) frames = atoi(argv[4]);
   }
   if (argc > 5) outdir = argv[5];
   if (!benchmark)
   {
      snprintf(cmdbuf, sizeof(cmdbuf), "mkdir -p %s %s %s", outdir, sysdir, savedir);
      system(cmdbuf);
   }

   /* defaults, overridable via VKHOST_VARS */
   /* Must be a value the option declares; an undeclared value leaves
    * the internal upscale on the CPU side, a state no frontend
    * produces (same defect the GL harness had). */
   add_var("beetle_psx_hw_renderer", "hardware_vk");
   add_var("beetle_psx_hw_pgxp_mode", benchmark ? "disabled" : "memory only");
   add_var("beetle_psx_hw_color_format", benchmark ? "24bit" : "30bit_hdr");
   add_var("beetle_psx_hw_internal_resolution", benchmark ? "1x(native)" : "1x");
   add_var("beetle_psx_hw_filter", "nearest");
   {
      const char *e = getenv("VKHOST_VARS");
      if (e)
      {
         char *dup = strdup(e), *tok = strtok(dup, ";");
         while (tok)
         {
            char *eq = strchr(tok, '=');
            if (eq)
            {
               int i; *eq = 0;
               for (i = 0; i < n_vars; i++)
                  if (!strcmp(var_keys[i], tok)) { free(var_vals[i]); var_vals[i] = strdup(eq + 1); break; }
               if (i == n_vars) add_var(tok, eq + 1);
            }
            tok = strtok(NULL, ";");
         }
      }
   }

   if (benchmark)
   {
      summary_fd = dup(STDOUT_FILENO);
      if (summary_fd < 0 || fflush(stdout) != 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
         fatal("cannot reserve stdout for benchmark JSON");
   }
   core = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
   if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

#define SYM(name) *(void **)(&name##_fn) = dlsym(core, #name)
   { set_env_t retro_set_environment_fn; SYM(retro_set_environment);
     retro_set_environment_fn(env_cb); }
   { void (*retro_init_fn)(void); SYM(retro_init); retro_init_fn(); }
   { void (*f)(retro_video_refresh_t) = dlsym(core, "retro_set_video_refresh"); f(video_cb); }
   { void (*f)(retro_input_poll_t) = dlsym(core, "retro_set_input_poll"); f(input_poll_cb); }
   { void (*f)(retro_input_state_t) = dlsym(core, "retro_set_input_state"); f(input_state_cb); }
   { void (*f)(retro_audio_sample_t) = dlsym(core, "retro_set_audio_sample"); f(audio_cb); }
   { void (*f)(retro_audio_sample_batch_t) = dlsym(core, "retro_set_audio_sample_batch"); f(audio_batch_cb); }

   if (audio_path)
   {
      if (!*audio_path) fatal("VKHOST_AUDIO_PATH must not be empty");
      audio_file = fopen(audio_path, "wb");
      if (!audio_file) fatal("cannot open WAV output");
      wav_header();
   }

   {
      struct retro_game_info info;
      bool (*retro_load_game_fn)(const struct retro_game_info *) = dlsym(core, "retro_load_game");
      memset(&info, 0, sizeof(info));
      info.path = content;
      if (!retro_load_game_fn(&info))
      { fprintf(stderr, "[vkhost] retro_load_game failed\n"); return 3; }
   }

   if (audio_file || benchmark)
   {
      struct retro_system_av_info av;
      void (*get_av)(struct retro_system_av_info *) = dlsym(core, "retro_get_system_av_info");
      if (!get_av) fatal("core lacks retro_get_system_av_info");
      memset(&av, 0, sizeof(av));
      get_av(&av);
      note_timing(av.timing.fps, av.timing.sample_rate);
   }

   if (!negotiation && !hw_render.context_reset)
   {
      fprintf(stderr, "[vkhost] software renderer: no Vulkan bring-up\n");
      goto run_frames_sw;
   }
   if (!create_instance()) { fprintf(stderr, "[vkhost] instance failed\n"); return 3; }
   gipa = vkGetInstanceProcAddr;

   memset(&vkctx, 0, sizeof(vkctx));
   if (negotiation && negotiation->create_device)
   {
      static const VkPhysicalDeviceFeatures no_features; /* zeroed, as RetroArch passes */
      if (!negotiation->create_device(&vkctx, instance, gpu, VK_NULL_HANDLE,
               vkGetInstanceProcAddr, NULL, 0, NULL, 0, &no_features))
      { fprintf(stderr, "[vkhost] core create_device failed\n"); return 3; }
   }
   else { fprintf(stderr, "[vkhost] no negotiation interface\n"); return 3; }

   iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
   iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
   iface.handle = NULL;
   iface.instance = instance;
   iface.gpu = vkctx.gpu;
   iface.device = vkctx.device;
   iface.get_device_proc_addr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
   iface.get_instance_proc_addr = vkGetInstanceProcAddr;
   iface.queue = vkctx.queue;
   iface.queue_index = vkctx.queue_family_index;
   iface.set_image = vk_set_image;
   iface.get_sync_index = vk_get_sync_index;
   iface.get_sync_index_mask = vk_get_sync_index_mask;
   iface.wait_sync_index = vk_wait_sync_index;
   iface.set_command_buffers = vk_set_command_buffers;
   iface.lock_queue = vk_lock_queue;
   iface.unlock_queue = vk_unlock_queue;
   iface.set_signal_semaphore = vk_set_signal_semaphore;

   if (hw_render.context_reset) hw_render.context_reset();

run_frames_sw:

   if (state_path)
   {
      FILE *f = fopen(state_path, "rb"); long sz; void *data;
      bool (*retro_unserialize_fn)(const void *, size_t) = dlsym(core, "retro_unserialize");
      if (!f) { fprintf(stderr, "[vkhost] cannot open state\n"); return 4; }
      fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
      data = malloc(sz); fread(data, 1, sz, f); fclose(f);
      /* RetroArch RASTATE container: find the MEM chunk and use its payload. */
      if (sz > 8 && !memcmp(data, "RASTATE", 7))
      {
         unsigned char *p = (unsigned char *)data + 8;
         unsigned char *end = (unsigned char *)data + sz;
         void *payload = NULL; size_t paysz = 0;
         while (p + 8 <= end)
         {
            uint32_t chunk_sz;
            memcpy(&chunk_sz, p + 4, 4);
            if (!memcmp(p, "MEM ", 4)) { payload = p + 8; paysz = chunk_sz; break; }
            p += 8 + ((chunk_sz + 7u) & ~7u);
         }
         if (payload)
         { fprintf(stderr, "[vkhost] RASTATE MEM chunk %zu bytes\n", paysz);
           if (!retro_unserialize_fn(payload, paysz))
           { fprintf(stderr, "[vkhost] unserialize failed\n"); return 4; } }
         else { fprintf(stderr, "[vkhost] RASTATE without MEM chunk\n"); return 4; }
      }
      else if (!retro_unserialize_fn(data, (size_t)sz))
      { fprintf(stderr, "[vkhost] unserialize failed (raw)\n"); return 4; }
      free(data);
      fprintf(stderr, "[vkhost] savestate loaded\n");
   }

   {
      int i;
      /* VKHOST_BENCH turns the harness into a stopwatch: the periodic PPM
       * dump is suppressed (it does a vkDeviceWaitIdle, a full image copy
       * and a file write every 30 frames, which is most of the wall time
       * at any real frame rate), and the first VKHOST_BENCH_SKIP frames
       * run untimed so pipeline creation and first-use shader compilation
       * stay out of the measurement. */
      int bench      = !benchmark && getenv("VKHOST_BENCH") != NULL;
      int bench_skip = 0;
      struct timespec t0, t1;
      void (*retro_run_fn)(void) = dlsym(core, "retro_run");

      if (bench)
      {
         const char *e = getenv("VKHOST_BENCH_SKIP");
         bench_skip = e ? atoi(e) : 120;
         if (bench_skip >= frames)
            bench_skip = frames / 4;
      }

      if (benchmark) vk_wait_sync_index(NULL);
      for (i = 0; i < frames; i++)
      {
         uint64_t frame_begin = 0, core_begin = 0;
         cur_frame = i;
         if (benchmark)
         {
            measuring = (uint32_t)i >= warmup;
            frame_begin = monotonic_ns();
            if ((uint32_t)i == warmup) measured_start = frame_begin;
         }
         if (bench && i == bench_skip)
         {
            if (vkctx.device)
               vkDeviceWaitIdle(vkctx.device);
            clock_gettime(CLOCK_MONOTONIC, &t0);
         }
         if (benchmark) core_begin = monotonic_ns();
         retro_run_fn();
         if (benchmark)
         {
            uint64_t core_end = monotonic_ns(), frame_end;
            if (image_pending || frame_command_count || signal_semaphore)
               fatal("core left a Vulkan handoff without video_refresh");
            vk_wait_sync_index(NULL);
            frame_end = monotonic_ns();
            if (measuring)
            {
               timing_add(&core_times, elapsed_ns(core_begin, core_end));
               timing_add(&frame_times, elapsed_ns(frame_begin, frame_end));
               measured_end = frame_end;
            }
            continue;
         }
         {
            /* VKHOST_SAVE_STATE=path:frame - checkpoint mid-run so long
             * cold-boot treks can be chained across invocations. */
            static char sv_path[512]; static int sv_at = -1;
            if (sv_at < 0)
            {
               const char *ss = getenv("VKHOST_SAVE_STATE");
               sv_at = 0;
               if (ss)
               {  const char *c = strrchr(ss, ':');
                  if (c) { sv_at = atoi(c + 1);
                           snprintf(sv_path, sizeof(sv_path), "%.*s",
                                    (int)(c - ss), ss); } }
            }
            if (sv_at && i + 1 == sv_at && sv_path[0])
            {
               size_t (*ssz_fn)(void) = dlsym(core, "retro_serialize_size");
               bool (*ser_fn)(void *, size_t) = dlsym(core, "retro_serialize");
               size_t need = ssz_fn();
               void *sb = malloc(need);
               if (sb && ser_fn(sb, need))
               {  FILE *sf = fopen(sv_path, "wb");
                  if (sf) { fwrite(sb, 1, need, sf); fclose(sf);
                            fprintf(stderr, "[vkhost] state saved %s (%zu) at frame %d\n",
                                    sv_path, need, i + 1); } }
               free(sb);
            }
         }
         { static int dump_iv = 0;
           if (!dump_iv) { const char *e = getenv("VKHOST_DUMP_INTERVAL");
                           dump_iv = e ? atoi(e) : 30; if (dump_iv < 1) dump_iv = 30; }
         if (!bench && vkctx.device && ((i % dump_iv) == dump_iv - 1 || i == frames - 1))
         {
            char path[600];
            snprintf(path, sizeof(path), "%s/frame_%04d.ppm", outdir, i + 1);
            dump_frame(path);
         } }
      }

      if (bench)
      {
         double secs;
         /* Charge queued-but-unretired GPU work to the measurement rather
          * than to teardown: wait_sync_index is a no-op in this harness,
          * so the core is free to run ahead of the device. */
         if (vkctx.device)
            vkDeviceWaitIdle(vkctx.device);
         clock_gettime(CLOCK_MONOTONIC, &t1);
         secs = (double)(t1.tv_sec - t0.tv_sec)
              + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
         fprintf(stderr, "[vkhost] BENCH %d frames in %.4f s = %.1f fps\n",
               frames - bench_skip, secs,
               secs > 0.0 ? (double)(frames - bench_skip) / secs : 0.0);
      }
   }

   measuring = false;
   if (benchmark && ownership_pool)
   {
      vk_wait_sync_index(NULL);
      vkDestroyCommandPool(vkctx.device, ownership_pool, NULL);
      ownership_pool = VK_NULL_HANDLE;
   }
   fprintf(stderr, "[vkhost] done: %d frames run, %u valid, %d validation errors, %d warnings\n",
           frames, frame_valid, validation_errors, validation_warnings);
   { void (*f)(void) = dlsym(core, "retro_unload_game"); if (f) f(); }
   { void (*f)(void) = dlsym(core, "retro_deinit"); if (f) f(); }
   wav_close();
   if (benchmark)
   {
      if (fflush(stdout) != 0 || dup2(summary_fd, STDOUT_FILENO) < 0 || close(summary_fd) != 0)
         fatal("cannot restore benchmark JSON output");
      if (!setlocale(LC_NUMERIC, "C")) fatal("cannot set JSON numeric locale");
      printf("{\"benchmark\":true,\"frames\":%d,\"warmup_frames\":%u,\"measured_frames\":%zu,"
            "\"sample_count\":%zu,\"valid_video_frames\":%u,\"audio_frames_total\":%" PRIu64
            ",\"audio_frames_measured\":%" PRIu64 ",\"audio_channels\":2,"
            "\"reported_fps\":%.9f,\"reported_sample_rate\":%.9f,\"timing_changes\":%" PRIu64 ","
            "\"validation\":false,\"readback\":false,\"capture\":false,"
            "\"sync\":\"%s\",\"percentiles\":\"nearest_rank\","
            "\"total_core_wall_ns\":%" PRIu64 ",\"measured_wall_ns\":%" PRIu64 ",\"core_time_ns\":",
            frames, warmup, core_times.count, core_times.count, frame_valid,
            total_audio_frames, measured_audio_frames,
            reported_fps, reported_sample_rate, timing_changes,
            vkctx.device ? "queue_idle_per_frame" : "software",
            core_times.total, elapsed_ns(measured_start, measured_end));
      timing_print(&core_times);
      printf(",\"frame_time_ns\":");
      timing_print(&frame_times);
      printf("}\n");
      if (fflush(stdout) != 0 || ferror(stdout)) fatal("cannot write benchmark JSON");
      free(core_times.values);
      free(frame_times.values);
   }
   return validation_errors ? 5 : 0;
}
