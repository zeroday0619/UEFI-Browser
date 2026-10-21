#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware_audio.h"
#include "audio-stream-types.h"
static Clip* active;
static uint64_t descriptor_memory;
static uint16_t mixer = 0x100, master = 0x200, previous_command, rate;
static unsigned current, remaining, last, control;
static uint64_t played;
static int child, defer_resume;
static int renderer_runtime_is_child(void) { return child; }
static uint8_t input8(uint16_t port) {
  return port == master + 0x14 ? current : control;
}
static uint16_t input16(uint16_t port) {
  return port == master + 0x18 ? remaining : rate;
}
static void command(uint16_t value) { (void)value; }
static void fetch(void) {
  Descriptor* descriptors = (void*)(uintptr_t)descriptor_memory;
  remaining = descriptors[current].samples;
}
static void output8(uint16_t port, uint8_t value) {
  if (port == master + 0x15) {
    if (control == 1 && !remaining && !defer_resume) { current = (current + 1) % 32; fetch(); }
    last = value;
  } else if (port == master + 0x1b) {
    control = value;
    if (value == 1) fetch();
  }
}
static void output16(uint16_t port, uint16_t value) {
  if (port == mixer + 0x2c) rate = value;
}
static void output32(uint16_t port, uint32_t value) { (void)port; (void)value; }
static void stop(void) { active = NULL; control = current = remaining = 0; }
static void apply_gain(Clip* clip, size_t first, size_t bytes) {
  assert(first <= clip->bytes && bytes <= clip->bytes - first);
  int16_t* output = (void*)(uintptr_t)clip->memory;
  for (size_t index = first / 2; index < (first + bytes) / 2; ++index)
    output[index] = (int64_t)clip->source[index] * clip->gain / 65536;
}
#include "../src/audio/ac97-stream.h"
static void advance(Clip* clip, unsigned frames) {
  Descriptor* descriptors = (void*)(uintptr_t)descriptor_memory;
  if (control == 1 && !remaining && current != last) {
    current = (current + 1) % 32; fetch();
  }
  while (frames && remaining) {
    unsigned count = remaining / 2;
    if (count > frames) count = frames;
    size_t offset = (uint32_t)(descriptors[current].address - (uint32_t)clip->memory);
    offset += (descriptors[current].samples - remaining) * 2;
    assert(offset + count * 4 <= clip->bytes);
    int16_t* samples = (void*)(uintptr_t)(clip->memory + offset);
    for (unsigned index = 0; index < count; ++index) {
      assert(samples[index * 2] == (int16_t)((played + index) % 10000));
      assert(samples[index * 2 + 1] == -samples[index * 2]);
    }
    played += count; frames -= count; remaining -= count * 2;
    if (!remaining && current != last) { current = (current + 1) % 32; fetch(); }
  }
}
static void drained(Clip* clip) {
  assert(clip->stream_head == clip->stream_tail);
  assert(clip->stream_published == clip->stream_tail);
  for (unsigned index=0;index<FIRMWARE_AUDIO_STREAM_SLOTS;++index)
    assert(!clip->stream_used[index]);
}
static long submit(Clip* clip, unsigned frames) {
  int16_t samples[8192];
  assert(frames <= 4096);
  for (unsigned index = 0; index < frames; ++index) {
    samples[index * 2] = (clip->stream_submitted + index) % 10000;
    samples[index * 2 + 1] = -samples[index * 2];
  }
  return stream_request(clip, AUDIO_STREAM_WRITE, clip->stream_submitted,
                        (uintptr_t)samples, frames * 4);
}
int main(void) {
  Descriptor descriptors[32] = {0};
  descriptor_memory = (uintptr_t)descriptors;
  Clip clip = {0}; clip.streaming = 1; clip.bytes = FIRMWARE_AUDIO_STREAM_SLOTS * 16384; clip.gain = 65536;
  clip.memory = (uintptr_t)calloc(1, clip.bytes); clip.source = calloc(1, clip.bytes);
  assert(clip.memory && clip.source);
  assert(stream_request(&clip, AUDIO_STREAM_WRITE, 1, 1, 4) == -EINVAL);
  assert(stream_request(&clip, AUDIO_STREAM_WRITE, 0, 0, 4) == -EINVAL);
  assert(stream_request(&clip, AUDIO_STREAM_WRITE, 0, 1, 16388) == -EINVAL);
  assert(stream_request(&clip, AUDIO_STREAM_WRITE, 0, 1, 3) == -EINVAL);
  assert(stream_request(&clip, AUDIO_START, 0, 48000, 0) == -EAGAIN);
  for (unsigned index = 0; index < FIRMWARE_AUDIO_STREAM_SLOTS; ++index) assert(submit(&clip, 128) == 512);
  assert(submit(&clip, 128) == -EAGAIN);
  assert(stream_request(&clip, AUDIO_START, 0, 48000, 0) == 0);
  unsigned saved_current = current, saved_remaining = remaining;
  current = 99;
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == -EIO);
  current = saved_current;
  remaining = 65535;
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == -EIO);
  remaining = saved_remaining;
  advance(&clip, 11 * 128 + 64);
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == (long)played);
  assert(stream_request(&clip, AUDIO_STOP, 0, 0, 0) == (long)played);
  assert(clip.stream_slots[0].frames == 64 && clip.stream_slots[0].offset == 256);
  assert(stream_request(&clip, AUDIO_START, 0, 48000, 0) == 0);
  for (unsigned index = 0; index < 300; ++index) {
    advance(&clip, 128);
    assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == (long)played);
    assert(submit(&clip, 128) == 512);
  }
  while (played < clip.stream_submitted) {
    advance(&clip, 128);
    firmware_audio_poll();
    assert(clip.stream_head <= clip.stream_published);
    assert(clip.stream_published <= clip.stream_tail);
    assert(clip.stream_published - clip.stream_head <= 31);
  }
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == (long)clip.stream_submitted);
  drained(&clip);
  defer_resume = 1;
  assert(submit(&clip, 128) == 512);
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == (long)played);
  advance(&clip, 128);
  child = 1;
  uint64_t old_head = clip.stream_head;
  firmware_audio_poll();
  assert(clip.stream_head == old_head);
  child = 0;
  firmware_audio_poll();
  drained(&clip);
  defer_resume = 0;
  assert(submit(&clip, 128) == 512); advance(&clip, 128);
  assert(stream_request(&clip, AUDIO_POSITION, 0, 0, 0) == (long)clip.stream_submitted);
  assert(stream_request(&clip, AUDIO_STREAM_RESET, 1, 0, 0) == -EINVAL);
  assert(stream_request(&clip, AUDIO_STREAM_RESET, 0, 0, 0) == 0);
  assert(!active && !clip.stream_submitted);
  drained(&clip);
  played = 0;
  assert(submit(&clip, 4096) == 16384);
  assert(stream_request(&clip, AUDIO_START, 0, 48000, 0) == 0);
  advance(&clip, 4096);
  assert(stream_request(&clip, AUDIO_STOP, 0, 0, 0) == 4096);
  free((void*)(uintptr_t)clip.memory); free(clip.source);
  puts("PASS: retained PCM bounds, BSP-only refill, hardware/software wraparound sample order, partial pause/resume, delayed underrun restart and reset");
}
