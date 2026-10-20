#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware_audio.h"
#include "renderer_runtime.h"

/* PCI bus mastering stays on the BSP; renderers upload bounded PCM copies. */
extern SystemTable* platform_firmware(void);
typedef struct {
  uint32_t address, samples;
} Descriptor;
typedef struct {
  uint64_t memory;
  size_t pages, bytes, uploaded, start;
  unsigned owner, rate, gain;
  int16_t* source;
  long identifier;
} Clip;
static Clip clips[8];
static long next_identifier = 1;
static Clip* active;
static uint16_t mixer, master;
static uint32_t device;
static uint16_t previous_command;
static uint64_t descriptor_memory;
static unsigned descriptor_count;
static size_t descriptor_offsets[32];
static uint16_t descriptor_samples[32];

static uint8_t input8(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
  return value;
}
static uint16_t input16(uint16_t port) {
  uint16_t value;
  __asm__ volatile("inw %1,%0" : "=a"(value) : "Nd"(port));
  return value;
}
static uint32_t input32(uint16_t port) {
  uint32_t value;
  __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port));
  return value;
}
static void output8(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port) : "memory");
}
static void output16(uint16_t port, uint16_t value) {
  __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port) : "memory");
}
static void output32(uint16_t port, uint32_t value) {
  __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port) : "memory");
}
static uint32_t configuration(uint32_t address) {
  uint64_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
  output32(0xcf8, address);
  uint32_t result = input32(0xcfc);
  __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
  return result;
}
static void command(uint16_t value) {
  uint64_t flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
  output32(0xcf8, device + 4);
  output16(0xcfc, value);
  __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}
static void apply_gain(Clip* clip, size_t first, size_t bytes) {
  volatile int16_t* destination = (void*)(uintptr_t)clip->memory;
  for (size_t index = first / 2; index < (first + bytes) / 2; ++index)
    destination[index] = (int64_t)clip->source[index] * clip->gain / 65536;
  __asm__ volatile("mfence" ::: "memory");
}
static void stall(size_t duration) {
  ((Status(UEFI_API*)(size_t))platform_firmware()->boot->function[28])(
      duration);
}
static int allocate(size_t pages, uint64_t* address) {
  uint64_t candidate = 0xffffffff;
  Status result = ((Status(UEFI_API*)(uint32_t, uint32_t, size_t,
                                      uint64_t*))platform_firmware()
                       ->boot->function[2])(1, 4, pages, &candidate);
  *address = result ? 0 : candidate;
  return result ? -ENOMEM : 0;
}
static void release(uint64_t address, size_t pages) {
  ((Status(UEFI_API*)(uint64_t, size_t))platform_firmware()->boot->function[3])(
      address, pages);
}
static int initialize(void) {
  if (master)
    return 0;
  for (unsigned slot = 0; slot < 32; ++slot)
    for (unsigned function = 0; function < 8; ++function) {
      uint32_t address = 0x80000000u | slot << 11 | function << 8;
      if (configuration(address) != 0x24158086u)
        continue;
      uint32_t first = configuration(address + 0x10),
               second = configuration(address + 0x14);
      if (!(first & 1) || !(second & 1) || (first & ~3u) > 0xffff ||
          (second & ~3u) > 0xffff || !(first & ~3u) || !(second & ~3u))
        continue;
      if (allocate(1, &descriptor_memory))
        return -ENOMEM;
      device = address;
      previous_command = configuration(address + 4);
      command(previous_command | 5);
      mixer = first & ~3u;
      master = second & ~3u;
      output32(master + 0x2c, 2);
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (input32(master + 0x30) & 0x100)
          break;
        stall(1000);
      }
      if (!(input32(master + 0x30) & 0x100)) {
        command(previous_command);
        release(descriptor_memory, 1);
        descriptor_memory = 0;
        mixer = master = 0;
        return -ENODEV;
      }
      output16(mixer, 0);
      output16(mixer + 2, 0);
      output16(mixer + 0x18, 0);
      output16(mixer + 0x2a, input16(mixer + 0x2a) | 1);
      printf("AUDIO: AC97 PCI=%x mixer=%x bus_master=%x DMA=%llx\n", device,
             mixer, master, (unsigned long long)descriptor_memory);
      return 0;
    }
  return -ENODEV;
}
static void stop(void) {
  if (!master)
    return;
  /* Disable bus mastering before releasing or replacing any DMA buffer. */
  command((previous_command | 1) & ~4);
  output8(master + 0x1b, 0);
  output8(master + 0x1b, 2);
  for (unsigned attempt = 0; attempt < 100 && (input8(master + 0x1b) & 2);
       ++attempt)
    stall(10);
  active = NULL;
}
static long position(Clip* clip) {
  if (active != clip)
    return clip->start / 4;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    unsigned current = input8(master + 0x14);
    unsigned remaining = input16(master + 0x18);
    if (current != input8(master + 0x14))
      continue;
    if (current >= descriptor_count || remaining > descriptor_samples[current])
      return -EIO;
    size_t bytes = descriptor_offsets[current] +
                   (descriptor_samples[current] - remaining) * 2;
    if (bytes > clip->bytes)
      return -EIO;
    return bytes / 4;
  }
  return -EAGAIN;
}
long firmware_audio_service(unsigned owner,
                            unsigned operation,
                            long handle,
                            uint64_t first,
                            uint64_t second,
                            uint64_t third) {
  if (owner > 1)
    return -EINVAL;
  if (operation == AUDIO_CREATE) {
    if (!first || first > FIRMWARE_AUDIO_LIMIT || first % 4 || second < 8000 ||
        second > 48000)
      return -EINVAL;
    int result = initialize();
    if (result)
      return result;
    for (unsigned index = 0; index < 8; ++index)
      if (!clips[index].identifier) {
        Clip* clip = &clips[index];
        size_t pages = (first + 4095) / 4096;
        if (allocate(pages, &clip->memory))
          return -ENOMEM;
        clip->source = malloc(first);
        if (!clip->source) {
          release(clip->memory, pages);
          memset(clip, 0, sizeof(*clip));
          return -ENOMEM;
        }
        clip->gain = 65536;
        clip->pages = pages;
        clip->bytes = first;
        clip->rate = second;
        clip->owner = owner;
        if (next_identifier == LONG_MAX) {
          release(clip->memory, pages);
          free(clip->source);
          memset(clip, 0, sizeof(*clip));
          return -EMFILE;
        }
        clip->identifier = next_identifier++;
        return clip->identifier;
      }
    return -EMFILE;
  }
  Clip* clip = NULL;
  for (unsigned index = 0; index < 8; ++index)
    if (clips[index].identifier == handle && handle > 0 &&
        clips[index].owner == owner) {
      clip = &clips[index];
      break;
    }
  if (!clip)
    return -EBADF;
  switch (operation) {
    case AUDIO_UPLOAD:
      if (active == clip || first != clip->uploaded || third > 16384 ||
          third > clip->bytes - clip->uploaded || third % 4 ||
          (third && !second))
        return -EINVAL;
      memcpy((unsigned char*)clip->source + first,
             (const void*)(uintptr_t)second, third);
      apply_gain(clip, first, third);
      clip->uploaded += third;
      return third;
    case AUDIO_START: {
      if (clip->uploaded != clip->bytes || first > clip->bytes / 4 ||
          second < 8000 || second > 48000)
        return -EINVAL;
      if (active && active != clip)
        return -EBUSY;
      stop();
      if (input8(master + 0x1b) & 2)
        return -EIO;
      clip->start = first * 4;
      if (clip->start == clip->bytes)
        return 0;
      Descriptor* descriptors = (void*)(uintptr_t)descriptor_memory;
      size_t offset = clip->start;
      descriptor_count = 0;
      while (offset < clip->bytes) {
        size_t bytes = clip->bytes - offset;
        if (bytes > 131068)
          bytes = 131068;
        unsigned index = descriptor_count++;
        descriptors[index] = (Descriptor){(uint32_t)(clip->memory + offset),
                                          (uint32_t)(bytes / 2)};
        descriptor_offsets[index] = offset;
        descriptor_samples[index] = bytes / 2;
        offset += bytes;
      }
      output16(mixer + 0x2c, second);
      if (input16(mixer + 0x2c) != second)
        return -EIO;
      output16(mixer + 0x18, 0);
      output32(master + 0x10, descriptor_memory);
      output16(master + 0x16, 0x1c);
      output8(master + 0x15, descriptor_count - 1);
      active = clip;
      __asm__ volatile("mfence" ::: "memory");
      command(previous_command | 5);
      output8(master + 0x1b, 1);
      printf(
          "AUDIO: start owner=%u handle=%ld frame=%llu rate=%llu "
          "descriptors=%u\n",
          owner, handle, (unsigned long long)first, (unsigned long long)second,
          descriptor_count);
      return 0;
    }
    case AUDIO_POSITION:
      return position(clip);
    case AUDIO_STOP: {
      long frame = position(clip);
      if (active == clip)
        stop();
      if (frame >= 0)
        clip->start = (size_t)frame * 4;
      return frame;
    }
    case AUDIO_VOLUME:
      if (first > 65536)
        return -EINVAL;
      if (clip->gain != first) {
        clip->gain = first;
        apply_gain(clip, 0, clip->uploaded);
      }
      return 0;
    case AUDIO_CLOSE:
      if (active == clip)
        stop();
      release(clip->memory, clip->pages);
      free(clip->source);
      memset(clip, 0, sizeof(*clip));
      return 0;
    default:
      return -EINVAL;
  }
}
long firmware_audio_request(unsigned operation,
                            long handle,
                            uint64_t first,
                            uint64_t second,
                            uint64_t third) {
  if (!renderer_runtime_is_child())
    return firmware_audio_service(0, operation, handle, first, second, third);
  /* The loader's AP stack is shared with the BSP, unlike private image data. */
  if (operation == AUDIO_UPLOAD) {
    if (third > 16384)
      return -EINVAL;
    unsigned char buffer[16384];
    memcpy(buffer, (const void*)(uintptr_t)second, third);
    return (long)renderer_runtime_request(RENDERER_AUDIO_REQUEST, operation,
                                          handle, first, (uintptr_t)buffer,
                                          third);
  }
  return (long)renderer_runtime_request(RENDERER_AUDIO_REQUEST, operation,
                                        handle, first, second, third);
}
void firmware_audio_reset(unsigned owner) {
  for (unsigned index = 0; index < 8; ++index)
    if (clips[index].identifier && clips[index].owner == owner)
      firmware_audio_service(owner, AUDIO_CLOSE, clips[index].identifier, 0, 0,
                             0);
  if (!owner && descriptor_memory) {
    stop();
    output16(mixer + 0x18, 0x8000);
    command(previous_command & ~4);
    release(descriptor_memory, 1);
    descriptor_memory = 0;
    mixer = master = 0;
    printf("AUDIO: DMA stopped and device buffers released\n");
  }
}
