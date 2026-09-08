import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile


PREAMBLE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <locale.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
'''

MOCK_VULKAN = r'''
typedef int VkResult;
typedef uintptr_t VkSemaphore;
typedef uintptr_t VkCommandBuffer;
typedef uintptr_t VkCommandPool;
typedef uintptr_t VkImage;
typedef unsigned VkImageLayout;
typedef unsigned VkPipelineStageFlags;
typedef struct { unsigned aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } VkImageSubresourceRange;
typedef struct { VkImage image; int format; VkImageSubresourceRange subresourceRange; } VkImageViewCreateInfo;
struct retro_vulkan_image { uintptr_t image_view; VkImageLayout image_layout; VkImageViewCreateInfo create_info; };
typedef struct {
   int sType; const void *pNext; unsigned waitSemaphoreCount;
   const VkSemaphore *pWaitSemaphores; const VkPipelineStageFlags *pWaitDstStageMask;
   unsigned commandBufferCount; const VkCommandBuffer *pCommandBuffers;
   unsigned signalSemaphoreCount; const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;
typedef struct { int sType; const void *pNext; unsigned flags; } VkCommandBufferBeginInfo;
typedef struct {
   int sType; const void *pNext; unsigned srcAccessMask, dstAccessMask;
   VkImageLayout oldLayout, newLayout; unsigned srcQueueFamilyIndex, dstQueueFamilyIndex;
   VkImage image; VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;
typedef struct { int sType; const void *pNext; unsigned flags, queueFamilyIndex; } VkCommandPoolCreateInfo;
typedef struct { int sType; const void *pNext; VkCommandPool commandPool; unsigned level, commandBufferCount; } VkCommandBufferAllocateInfo;
#define VK_SUCCESS 0
#define VK_NULL_HANDLE 0
#define VK_QUEUE_FAMILY_IGNORED UINT32_MAX
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 1
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 2
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 3
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 4
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 5
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 1
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 1
#define VK_PIPELINE_STAGE_ALL_COMMANDS_BIT 1
#define RETRO_HW_FRAME_BUFFER_VALID ((const void *)(intptr_t)-1)
static struct { uintptr_t device, queue; unsigned queue_family_index; } vkctx = { 1, 2, 3 };
static const struct retro_vulkan_image *last_image;
static unsigned last_w, last_h, frame_valid;
static int cur_frame;
static int mock_failure, submits, waits, work_pending, barrier_count, command_count;
static unsigned char sem_state[128];
static VkImageMemoryBarrier barriers[2];
static VkResult vkQueueWaitIdle(uintptr_t queue);
static VkResult vkQueueSubmit(uintptr_t queue, unsigned n, const VkSubmitInfo *submit, uintptr_t fence);
static VkResult vkCreateCommandPool(uintptr_t dev, const VkCommandPoolCreateInfo *info, const void *alloc, VkCommandPool *pool)
{ (void)dev; (void)info; (void)alloc; *pool = 20; return VK_SUCCESS; }
static VkResult vkAllocateCommandBuffers(uintptr_t dev, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *cmd)
{ (void)dev; (void)info; *cmd = 21; return VK_SUCCESS; }
static VkResult vkResetCommandBuffer(VkCommandBuffer cmd, unsigned flags)
{ (void)cmd; (void)flags; assert(!work_pending); return VK_SUCCESS; }
static VkResult vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{ (void)cmd; (void)info; return VK_SUCCESS; }
static VkResult vkEndCommandBuffer(VkCommandBuffer cmd)
{ (void)cmd; return VK_SUCCESS; }
static void vkCmdPipelineBarrier(VkCommandBuffer cmd, unsigned src, unsigned dst, unsigned flags,
      unsigned nmem, const void *mem, unsigned nbuf, const void *buf, unsigned nimg, const VkImageMemoryBarrier *bar)
{
   (void)cmd; (void)src; (void)dst; (void)flags; (void)nmem; (void)mem; (void)nbuf; (void)buf;
   assert(nimg == 1 && barrier_count < 2);
   barriers[barrier_count++] = *bar;
}
'''

MOCK_FUNCTIONS = r'''
static void assert_queue_locked(void)
{
   int result = pthread_mutex_trylock(&queue_mutex);
   if (!result) pthread_mutex_unlock(&queue_mutex);
   assert(result != 0);
}
static VkResult vkQueueWaitIdle(uintptr_t queue)
{
   assert(queue == vkctx.queue);
   assert_queue_locked();
   waits++;
   work_pending = 0;
   return mock_failure ? -4 : VK_SUCCESS;
}
static VkResult vkQueueSubmit(uintptr_t queue, unsigned n, const VkSubmitInfo *submit, uintptr_t fence)
{
   unsigned i;
   assert(queue == vkctx.queue && n == 1 && fence == VK_NULL_HANDLE);
   assert_queue_locked();
   submits++;
   for (i = 0; i < submit->waitSemaphoreCount; i++)
   {
      assert(submit->pWaitDstStageMask[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      assert(sem_state[submit->pWaitSemaphores[i]] == 1);
      sem_state[submit->pWaitSemaphores[i]] = 0;
   }
   for (i = 0; i < submit->signalSemaphoreCount; i++)
   {
      assert(!sem_state[submit->pSignalSemaphores[i]]);
      sem_state[submit->pSignalSemaphores[i]] = 1;
   }
   command_count += submit->commandBufferCount;
   work_pending = 1;
   return mock_failure ? -4 : VK_SUCCESS;
}
static void check_sync(const char *mode)
{
   struct retro_vulkan_image image = {0};
   VkSemaphore sem = 1;
   VkCommandBuffer cmds[2] = {10, 11};
   int before;
   benchmark = true;
   image.create_info.image = 7;
   image.create_info.format = 37;
   image.image_layout = 9;
   image.create_info.subresourceRange.aspectMask = 1;
   image.create_info.subresourceRange.levelCount = 1;
   image.create_info.subresourceRange.layerCount = 1;
   if (!strcmp(mode, "sync-overlap"))
   {
      vk_set_image(NULL, &image, 0, NULL, 3);
      vk_set_image(NULL, &image, 0, NULL, 3);
      abort();
   }
   if (!strcmp(mode, "sync-semaphore-bound"))
   { vk_set_image(NULL, &image, MAX_FRAME_SEMAPHORES + 1, &sem, 3); abort(); }
   if (!strcmp(mode, "sync-command-bound"))
   { vk_set_command_buffers(NULL, MAX_FRAME_COMMAND_BUFFERS + 1, cmds); abort(); }
   if (!strcmp(mode, "sync-failure"))
   { mock_failure = 1; vk_wait_sync_index(NULL); abort(); }
   assert(vk_get_sync_index(NULL) == 0 && vk_get_sync_index_mask(NULL) == 1);
   sem_state[1] = 1;
   vk_set_image(NULL, &image, 1, &sem, 3);
   image.create_info.image = 99;
   assert(pending_image == 7 && last_image == NULL);
   vk_set_signal_semaphore(NULL, 2);
   benchmark_video(RETRO_HW_FRAME_BUFFER_VALID);
   assert(submits == 1 && !sem_state[1] && sem_state[2]);
   assert(!image_pending && !frame_semaphore_count && !signal_semaphore);
   vk_wait_sync_index(NULL);
   assert(waits == 1 && !work_pending);
   benchmark_video(NULL);
   assert(submits == 1);
   vk_set_signal_semaphore(NULL, 3);
   benchmark_video(NULL);
   assert(submits == 2 && sem_state[3]);
   sem = 4;
   sem_state[4] = 1;
   before = submits;
   vk_set_command_buffers(NULL, 1, cmds);
   vk_set_command_buffers(NULL, 1, cmds + 1);
   vk_set_image(NULL, &image, 1, &sem, 3);
   assert(submits == before);
   benchmark_video(RETRO_HW_FRAME_BUFFER_VALID);
   assert(submits == before + 1 && sem_state[4] && command_count == 2);
   vk_wait_sync_index(NULL);
   sem = 5;
   sem_state[5] = 1;
   vk_set_image(NULL, &image, 1, &sem, 7);
   image.create_info.image = 100;
   benchmark_video(RETRO_HW_FRAME_BUFFER_VALID);
   assert(!sem_state[5] && barrier_count == 2);
   assert(barriers[0].srcQueueFamilyIndex == 7 && barriers[0].dstQueueFamilyIndex == 3);
   assert(barriers[1].srcQueueFamilyIndex == 3 && barriers[1].dstQueueFamilyIndex == 7);
   assert(barriers[0].image == 99 && barriers[1].image == 99);
   assert(barriers[0].oldLayout == 9 && barriers[1].newLayout == 9);
   assert(barriers[0].subresourceRange.levelCount == 1);
   vk_wait_sync_index(NULL);
   sem = 6;
   sem_state[6] = 1;
   vk_set_image(NULL, &image, 1, &sem, 3);
   before = submits;
   benchmark_video(NULL);
   assert(submits == before && sem_state[6] && !image_pending);
   work_pending = 1;
   vk_set_image(NULL, &image, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
   benchmark_video(RETRO_HW_FRAME_BUFFER_VALID);
   vk_wait_sync_index(NULL);
   assert(!work_pending);
   benchmark = false;
   before = waits;
   vk_wait_sync_index(NULL);
   vk_set_command_buffers(NULL, 2, cmds);
   vk_set_signal_semaphore(NULL, 7);
   vk_set_image(NULL, &image, 0, NULL, 3);
   assert(waits == before && !frame_command_count && !signal_semaphore && last_image == &image);
}
'''

MOCK_RUN = r'''
static VkResult vkDeviceWaitIdle(uintptr_t device)
{ (void)device; abort(); return VK_SUCCESS; }
static void mock_retro_run(void)
{
   int16_t samples[4] = {1, -2, 3, -4};
   VkSemaphore sem = 10;
   struct retro_vulkan_image image = {0};
   assert(!work_pending && !sem_state[10]);
   work_pending = 1;
   sem_state[10] = 1;
   image.create_info.image = 7;
   vk_set_image(NULL, &image, 1, &sem, vkctx.queue_family_index);
   video_cb(RETRO_HW_FRAME_BUFFER_VALID, 320, 240, 0);
   audio_batch_cb(samples, 2);
   audio_cb(5, -6);
   printf("mock core frame %d\n", cur_frame);
}
static int benchmark_main(int frames, uint32_t warmup)
{
   int i, bench = 0, bench_skip = 0, summary_fd = -1;
   uint64_t measured_start = 0, measured_end = 0;
   struct timespec t0;
   struct timing_stats core_times, frame_times;
   void (*retro_run_fn)(void) = mock_retro_run;
   benchmark = true;
   note_timing(60000.0 / 1001.0, 44100.0);
   hash_frames = true;
   timing_init(&core_times, frames - warmup);
   timing_init(&frame_times, frames - warmup);
'''

TEST_MAIN = r'''
int main(int argc, char **argv)
{
   struct timing_stats stats;
   uint32_t parsed = 123;
   int16_t samples[4] = {0, -1, 32767, -32768};
   const char *mode = argc > 1 ? argv[1] : "";
   if (!strcmp(mode, "config")) return config_main(argc - 1, argv + 1);
   if (!strcmp(mode, "benchmark")) return benchmark_main(atoi(argv[2]), (uint32_t)atoi(argv[3]));
   if (!strncmp(mode, "sync", 4)) { check_sync(mode); return 0; }
   if (!strcmp(mode, "parse"))
   {
      bool valid = decimal_u32(argv[2], INT_MAX, &parsed);
      printf("{\"valid\":%s,\"value\":%u}\n", valid ? "true" : "false", parsed);
      return 0;
   }
   if (!strcmp(mode, "timing-zero")) { timing_init(&stats, 0); abort(); }
   if (!strcmp(mode, "timing-bound")) { timing_init(&stats, MAX_TIMING_SAMPLES + 1); abort(); }
   if (!strcmp(mode, "timing-overflow"))
   { timing_init(&stats, 1); stats.total = UINT64_MAX; timing_add(&stats, 1); abort(); }
   if (!strcmp(mode, "timing-capacity"))
   { timing_init(&stats, 1); timing_add(&stats, 1); timing_add(&stats, 2); abort(); }
   if (!strcmp(mode, "clock-backwards")) { elapsed_ns(2, 1); abort(); }
   if (!strcmp(mode, "timing") || !strcmp(mode, "timing-single"))
   {
      unsigned i, count = !strcmp(mode, "timing-single") ? 1 : 100;
      timing_init(&stats, count);
      for (i = count; i; i--) timing_add(&stats, count == 1 ? 42 : i);
      timing_print(&stats);
      free(stats.values);
      assert(monotonic_ns() != 0);
      return 0;
   }
   if (!strcmp(mode, "timing-wide"))
   {
      timing_init(&stats, 3);
      timing_add(&stats, UINT64_C(5000000000));
      timing_add(&stats, 1);
      timing_add(&stats, UINT64_C(3000000000));
      timing_print(&stats);
      free(stats.values);
      return 0;
   }
   if (!strcmp(mode, "audio-overflow"))
   { total_audio_frames = UINT64_MAX; audio_cb(1, 2); abort(); }
   if (!strcmp(mode, "audio-null")) { audio_batch_cb(NULL, 1); abort(); }
   if (!strcmp(mode, "rate-fraction")) { wav_set_rate(44100.5); abort(); }
   if (!strcmp(mode, "rate-zero")) { wav_set_rate(0); abort(); }
   if (!strcmp(mode, "rate-nan")) { wav_set_rate(strtod("nan", NULL)); abort(); }
   if (!strcmp(mode, "rate-large")) { wav_set_rate(UINT32_MAX); abort(); }
   if (!strcmp(mode, "wav") || !strcmp(mode, "wav-overflow") || !strcmp(mode, "rate-change"))
   {
      audio_file = fopen(argv[2], "wb");
      if (!audio_file) fatal("cannot open test WAV");
      wav_header();
      wav_set_rate(argc > 3 ? strtod(argv[3], NULL) : 44100);
      if (!strcmp(mode, "wav-overflow"))
      { wav_frames = (UINT32_MAX - 36u) / 4u; wav_write(samples, 1); abort(); }
      audio_batch_cb(samples, 2);
      if (!strcmp(mode, "rate-change")) { wav_set_rate(48000); abort(); }
      measuring = true;
      audio_cb(-2, 1234);
      audio_batch_cb(NULL, 0);
      wav_close();
      printf("{\"total\":%" PRIu64 ",\"measured\":%" PRIu64 ",\"wav\":%" PRIu64 "}\n",
            total_audio_frames, measured_audio_frames, wav_frames);
      return 0;
   }
   return 99;
}
'''


def between(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def make_harness(source):
    helpers = between(source, "static bool benchmark;", "static uint64_t hash_bytes")
    hashes = between(source, "static uint64_t hash_bytes", "static uint64_t memory_hash")
    audio = between(source, "static size_t audio_batch_cb", "/* ---- vulkan bring-up")
    sync = between(source, "#define MAX_FRAME_SEMAPHORES", "/* ---- environment")
    video = between(source, "static void video_cb", "static void input_poll_cb")
    reserve = between(source, "   if (benchmark)\n   {\n      summary_fd = dup", "   core = dlopen")
    loop = between(source, "      if (benchmark) vk_wait_sync_index(NULL);", "            continue;\n         }")
    loop += "            continue;\n         }\n      }\n"
    summary = between(source, "   if (benchmark)\n   {\n      if (fflush(stdout)", "   return validation_errors")
    loop_check = MOCK_RUN + reserve + loop + r'''
   measuring = false;
   assert(!work_pending && !image_pending && !frame_command_count);
   assert(audio_hash == FNV64_OFFSET && audio_frames == 0);
''' + summary + "   return 0;\n}\n"
    config = between(source, "int main(int argc, char **argv)", "   /* defaults, overridable")
    config = config.replace("int main(", "static int config_main(", 1)
    config += r'''
   printf("{\"frames\":%d,\"warmup\":%u,\"capacity\":%zu}\n", frames, warmup, core_times.capacity);
   free(core_times.values);
   free(frame_times.values);
   return 0;
}
'''
    globals_ = r'''
#define FNV64_OFFSET UINT64_C(14695981039346656037)
#define FNV64_PRIME UINT64_C(1099511628211)
static uint64_t audio_hash = FNV64_OFFSET;
static size_t audio_frames;
static bool hash_frames;
static char sysdir[512] = "/tmp/vkhost_sys";
static char savedir[512] = "/tmp/vkhost_save";
'''
    return PREAMBLE + globals_ + helpers + hashes + audio + MOCK_VULKAN + sync + MOCK_FUNCTIONS + video + loop_check + config + TEST_MAIN


def run_checks(binary, directory):
    base_env = {key: value for key, value in os.environ.items() if not key.startswith("VKHOST_")}
    cases = 0

    def run(*args, env=None, expected=0):
        nonlocal cases
        result = subprocess.run([str(binary), *map(str, args)], env={**base_env, **(env or {})},
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        cases += 1
        return result

    for value, valid in [("0", True), ("0012", True), (str(2**31 - 1), True),
                         ("", False), ("-1", False), ("+1", False), (" 1", False),
                         ("1 ", False), ("1x", False), ("1\n", False), ("0x10", False),
                         (str(2**31), False), ("9" * 100, False)]:
        result = json.loads(run("parse", value).stdout)
        assert result["valid"] == valid, (value, result)
        if valid:
            assert result["value"] == int(value)

    stats = json.loads(run("timing").stdout)
    assert stats == {"sample_count": 100, "total": 5050, "mean": 50.5, "min": 1,
                     "max": 100, "p50": 50, "p95": 95, "p99": 99}, stats
    stats = json.loads(run("timing-single").stdout)
    assert stats == {"sample_count": 1, "total": 42, "mean": 42, "min": 42,
                     "max": 42, "p50": 42, "p95": 42, "p99": 42}, stats
    stats = json.loads(run("timing-wide").stdout)
    assert stats["total"] == 8000000001 and stats["p50"] == 3000000000 and stats["p99"] == 5000000000
    for mode in ("timing-zero", "timing-bound", "timing-overflow", "timing-capacity",
                 "clock-backwards", "audio-overflow", "audio-null", "rate-fraction",
                 "rate-zero", "rate-nan", "rate-large", "sync-overlap", "sync-semaphore-bound",
                 "sync-command-bound", "sync-failure"):
        run(mode, expected=6)
    run("sync")
    for frames, warmup in ((5, 2), (1, 0), (5, 4), (100, 10)):
        result = run("benchmark", frames, warmup)
        assert len(result.stdout.splitlines()) == 1
        assert result.stderr.count("mock core frame") == frames
        report = json.loads(result.stdout)
        measured = frames - warmup
        assert report["frames"] == frames and report["warmup_frames"] == warmup
        assert report["sample_count"] == report["measured_frames"] == measured
        assert report["audio_frames_total"] == frames * 3 and report["audio_frames_measured"] == measured * 3
        assert report["valid_video_frames"] == frames and report["audio_channels"] == 2
        assert abs(report["reported_fps"] - 60000 / 1001) < 1e-8
        assert report["reported_sample_rate"] == 44100 and report["timing_changes"] == 0
        assert report["sync"] == "queue_idle_per_frame" and report["percentiles"] == "nearest_rank"
        assert report["benchmark"] and not any(report[key] for key in ("validation", "readback", "capture"))
        core_stats, frame_stats = report["core_time_ns"], report["frame_time_ns"]
        assert report["total_core_wall_ns"] == core_stats["total"]
        assert report["measured_wall_ns"] >= frame_stats["total"] >= core_stats["total"]
        for stats in (core_stats, frame_stats):
            assert stats["sample_count"] == measured
            assert stats["min"] <= stats["p50"] <= stats["p95"] <= stats["p99"] <= stats["max"]
            assert abs(stats["mean"] - stats["total"] / measured) <= 0.001

    outdir = directory / "must-not-exist"
    config_args = ("config", "unused-core", "unused-content", "-", "5", outdir)
    bench = {"VKHOST_BENCHMARK": "1"}
    result = json.loads(run(*config_args, env=bench).stdout)
    assert result == {"frames": 5, "warmup": 0, "capacity": 5}
    result = json.loads(run(*config_args, env={**bench, "VKHOST_WARMUP_FRAMES": "4"}).stdout)
    assert result == {"frames": 5, "warmup": 4, "capacity": 1}
    result = json.loads(run("config", "unused-core", "unused-content", "-", "1000000", outdir, env=bench).stdout)
    assert result["capacity"] == 1000000
    result = json.loads(run("config", "unused-core", "unused-content", "-", str(2**31 - 1), outdir,
                            env={**bench, "VKHOST_WARMUP_FRAMES": str(2**31 - 2)}).stdout)
    assert result["capacity"] == 1 and result["warmup"] == 2**31 - 2
    for value in ("", "-1", "+1", " 1", "1x", "1\n", "5", "6", "9" * 100):
        run(*config_args, env={**bench, "VKHOST_WARMUP_FRAMES": value}, expected=6)
    for value in ("0", "-1", "1x", "1.5", "1000001", str(2**31), "9" * 100):
        run("config", "unused-core", "unused-content", "-", value, outdir, env=bench, expected=6)
    for name in ("VKHOST_AUDIO_PATH", "VKHOST_SAVE_STATE", "VKHOST_ROUNDTRIP_FRAME",
                 "VKHOST_RESET_FRAME", "VKHOST_RECREATE_FRAME", "VKHOST_BENCH", "VKHOST_BENCH_SKIP", "VKHOST_CORE_DUMP"):
        run(*config_args, env={**bench, name: "1"}, expected=2)
    run(*config_args, env={"VKHOST_BENCHMARK": "true"}, expected=6)
    run(*config_args, env={"VKHOST_WARMUP_FRAMES": "1"}, expected=6)
    run(*config_args, env={**bench, "VKHOST_HASHES": "1", "VKHOST_DUMP_RAW": "1",
                           "VKHOST_DUMP_INTERVAL": "1"})
    assert not outdir.exists()

    for rate in (44100, 48000):
        path = directory / f"audio-{rate}.wav"
        result = json.loads(run("wav", path, rate).stdout)
        assert result == {"total": 3, "measured": 1, "wav": 3}
        data = path.read_bytes()
        assert len(data) == 56 and data[:4] == b"RIFF" and data[8:16] == b"WAVEfmt "
        assert struct.unpack_from("<I", data, 4)[0] == len(data) - 8
        assert struct.unpack_from("<IHHIIHH", data, 16) == (16, 1, 2, rate, rate * 4, 4, 16)
        assert data[36:40] == b"data" and struct.unpack_from("<I", data, 40)[0] == 12
        assert struct.unpack_from("<6h", data, 44) == (0, -1, 32767, -32768, -2, 1234)
    run("wav-overflow", directory / "overflow.wav", expected=6)
    run("rate-change", directory / "rate-change.wav", expected=6)
    run("wav", directory / "missing" / "audio.wav", expected=6)
    if Path("/dev/full").exists():
        run("wav", "/dev/full", expected=6)
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    if not compiler or not shutil.which(compiler[0]):
        parser.error("a native C compiler is required; use --cc or CC")
    source = Path(__file__).with_name("vkhost.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="vkhost-check-") as temporary:
        directory = Path(temporary)
        harness = directory / "check.c"
        binary = directory / ("check.exe" if os.name == "nt" else "check")
        harness.write_text(make_harness(source), encoding="utf-8")
        subprocess.run([*compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                        "-Wno-missing-field-initializers", "-pthread", str(harness), "-o", str(binary)],
                       check=True, timeout=60)
        cases = run_checks(binary, directory)
    print(f"PASS: {cases} compiled helper/config/audio/mock-Vulkan checks; no real GPU or performance validation")


if __name__ == "__main__":
    main()
