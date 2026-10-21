#include "firmware_media.h"
#include "renderer_sandbox.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include "firmware_audio.h"

#ifdef ENABLE_MEDIA_SERVICE
typedef struct OpusDecoder OpusDecoder;
extern int opus_decoder_get_size(int channels);
extern int opus_decoder_init(OpusDecoder *, int sample_rate, int channels);
extern int opus_decode_float(OpusDecoder *, const unsigned char *, int,
                             float *, int, int);
enum { MEDIA_PACKET_LIMIT = 65536, MEDIA_FRAME_LIMIT = 5760 };
typedef struct {
  uint32_t pending, version, operation, channels, bytes;
  int32_t result;
  uint32_t frames, cpl, initialized, private_verified, denied_capabilities;
  unsigned char packet[MEDIA_PACKET_LIMIT];
  float pcm[MEDIA_FRAME_LIMIT * 2];
  FirmwareOpusCommand command;
  uint32_t slot;
} MediaChannel;
static volatile unsigned private_data = 17;
static _Thread_local unsigned private_thread = 19;
static unsigned char decoder_storage[131072] __attribute__((aligned(64)));
static unsigned char packet_storage[MEDIA_PACKET_LIMIT];
static float pcm_storage[MEDIA_FRAME_LIMIT * 2];

#include "media_opus_child.h"

void firmware_media_child(RendererBootstrap *bootstrap) {
  MediaChannel *message = bootstrap->application;
  if (!bootstrap->sandbox || !message || private_data != 17 || private_thread != 19)
    __builtin_trap();
  private_data = 59;
  private_thread = 61;
  uint16_t selector;
  __asm__ volatile("mov %%cs,%0" : "=r"(selector));
  message->cpl = selector & 3;
  message->private_verified = 1;
  const unsigned denied[] = {RENDERER_SYSTEM_THREAD, RENDERER_SYSTEM_MAP,
      RENDERER_SYSTEM_PROTECT, RENDERER_SYSTEM_BROKER, RENDERER_SYSTEM_JIT_SCOPE};
  message->denied_capabilities = 0;
  for (unsigned index = 0; index < sizeof(denied)/sizeof(denied[0]); ++index)
    if (renderer_system_call(denied[index],0,0,0,0,0) == (uint64_t)-1)
      ++message->denied_capabilities;
  __atomic_store_n(&message->initialized, 1, __ATOMIC_RELEASE);
  int channels = 0;
  for (;;) {
    if (!__atomic_load_n(&message->pending, __ATOMIC_ACQUIRE)) {
      __asm__ volatile("pause");
      continue;
    }
    const unsigned operation = message->operation;
    const unsigned bytes = message->bytes;
    const unsigned requested_channels = message->channels;
    int result = -22;
    message->frames = 0;
    if (message->version != 1) {
      result = -22;
    } else if (operation >= FIRMWARE_OPUS_CREATE && operation <= FIRMWARE_OPUS_CLOSE) {
      result = decode_command(message);
    } else if (operation == 1 && !bytes &&
               requested_channels >= 1 && requested_channels <= 2) {
      int size = opus_decoder_get_size(requested_channels);
      if (size > 0 && (size_t)size <= sizeof(decoder_storage)) {
        result = opus_decoder_init((OpusDecoder *)decoder_storage, 48000, requested_channels);
        channels = result == 0 ? requested_channels : 0;
      }
    } else if (operation == 2 && channels && requested_channels == (unsigned)channels &&
               bytes && bytes <= MEDIA_PACKET_LIMIT) {
      /* Decode uses a private copy so message storage is never retained by the codec. */
      memcpy(packet_storage, message->packet, bytes);
      result = opus_decode_float((OpusDecoder *)decoder_storage, packet_storage,
          bytes, pcm_storage, MEDIA_FRAME_LIMIT, 0);
      if (result > 0 && result <= MEDIA_FRAME_LIMIT) {
        memcpy(message->pcm, pcm_storage, (size_t)result * channels * sizeof(float));
        message->frames = result;
        result = 0;
      }
    } else if (operation == 3 && !bytes) {
      renderer_system_call(RENDERER_SYSTEM_EXIT, 0, 0, 0, 0, 0);
      __builtin_trap();
#ifdef ENABLE_MEDIA_SERVICE_PROBE
    } else if (operation == 254) {
      /* Only the diagnostic image deliberately attempts supervisor access. */
      private_data = *(volatile unsigned *)(uintptr_t)bootstrap->supervisor_address;
#endif
    }
    message->result = result;
    __atomic_store_n(&message->pending, 0, __ATOMIC_RELEASE);
  }
}

static RendererBootstrap *service;
static RendererRuntimeChannel runtime_channel;
static MediaChannel *channel;
static unsigned probe_stage, packets, probe_cycles;
static uint64_t deadline, startup_deadline;
static int service_joined;
static Status service_result;
static Status media_join(void) {
  if (!service_joined) {
    service_result = service->join();
    service_joined = service_result != ERROR(6);
  }
  return service_result;
}
static uint64_t milliseconds(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (uint64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static void submit(unsigned operation, unsigned bytes) {
  channel->version = 1;
  channel->operation = operation;
  channel->channels = 2;
  channel->bytes = bytes;
  __atomic_store_n(&channel->pending, 1, __ATOMIC_RELEASE);
}
static int initialize(void) {
  if (channel) return 1;
  service = renderer_runtime_media_bootstrap();
  if (!service || !service->sandbox || !renderer_runtime_available()) return 0;
  channel = renderer_runtime_shared_allocate(sizeof(*channel));
  if (!channel) return 0;
  memset(&runtime_channel, 0, sizeof(runtime_channel));
  runtime_channel.application = channel;
  runtime_channel.application_size = sizeof(*channel);
  private_data = 41;
  private_thread = 43;
  service_joined = 0;
  startup_deadline = milliseconds() + 10000;
  Status result = service->start(&runtime_channel);
  if (result) {
    renderer_runtime_shared_free(channel, sizeof(*channel));
    channel = NULL;
    printf("MEDIA SERVICE: start failed status=%llx\n", (unsigned long long)result);
    return 0;
  }
  printf("MEDIA SERVICE: started separate AP with private runtime and CPL3 page tables\n");
  return 1;
}
static void release(void) {
  renderer_runtime_shared_free(channel, sizeof(*channel));
  channel = NULL;
  probe_stage = 0;
}
void firmware_media_shutdown(void) {
  if (!channel) return;
  uint64_t end = milliseconds() + 10000;
  while (media_join() == ERROR(6)) {
    /* Startup may return before the AP installs its protected exception table. */
    service->terminate();
    if (milliseconds() >= end) {
      puts("FAIL: media process did not quiesce; live mappings retained");
      abort();
    }
  }
  release();
}
#include "media_opus_broker.h"
#ifdef ENABLE_MEDIA_SERVICE_PROBE
#include "media_probe_packet.h"
int firmware_media_probe(void) {
  if (probe_stage) return 1;
  if (transaction_ticket) return 0;
  for (unsigned index = 0; index < MEDIA_DECODER_SLOTS; ++index)
    if (streams[index].handle) return 0;
  if (!probe_cycles) {
    if (!initialize()) return 0;
    uint64_t started = milliseconds();
    firmware_media_shutdown();
    printf("MEDIA SERVICE: immediate shutdown completed elapsed_ms=%llu\n",
           (unsigned long long)(milliseconds() - started));
  }
  if (!initialize()) return 0;
  packets = 0;
  probe_stage = 1;
  deadline = milliseconds() + 10000;
  puts("MEDIA SERVICE: probe requested");
  return 1;
}
void firmware_media_poll(void) {
  production_poll();
  queue_poll();
  if (!probe_stage) return;
  if (milliseconds() >= deadline) {
    puts("FAIL: media service probe timed out");
    firmware_media_shutdown();
    return;
  }
  if (probe_stage == 5) {
    Status result = media_join();
    if (result == ERROR(6)) return;
    if (result != ERROR(21) || service->fault[0] != 14 || (service->fault[3] & 3) != 3 ||
        (service->fault[1] & 7) != 5) {
      puts("FAIL: media service supervisor access was not trapped");
    } else {
      printf("MEDIA SERVICE: probe passed cycle=%u decoded_packets=%u cpl=3 private_data=1 private_tls=1 supervisor_fault=14\n",
             ++probe_cycles, packets);
      printf("MEDIA SERVICE: denied_capabilities=5 fault_error=%llx fault_address=%llx\n",
          (unsigned long long)service->fault[1], (unsigned long long)service->fault[4]);
    }
    release();
    return;
  }
  if (!__atomic_load_n(&channel->initialized, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&channel->pending, __ATOMIC_ACQUIRE)) return;
  if (channel->cpl != 3 || !channel->private_verified || channel->denied_capabilities != 5 ||
      private_data != 41 || private_thread != 43) {
    puts("FAIL: media service private runtime verification failed");
    firmware_media_shutdown();
    return;
  }
  if (probe_stage == 1) {
    submit(1, 0);
    probe_stage = 2;
  } else if (probe_stage == 2 || probe_stage == 3) {
    if (channel->result || (probe_stage == 3 && channel->frames != 960)) {
      puts("FAIL: media service Opus decoding failed");
      firmware_media_shutdown();
      return;
    }
    if (probe_stage == 3) {
      double energy = 0;
      for (unsigned index = 0; index < 1920; ++index)
        energy += (double)channel->pcm[index] * channel->pcm[index];
      if (!(energy > 1 && energy < 1920)) {
        puts("FAIL: media service decoded PCM energy rejected");
        firmware_media_shutdown();
        return;
      }
      ++packets;
    }
    if (packets < 100) {
      memcpy(channel->packet, media_probe_packet, sizeof(media_probe_packet));
      submit(2, sizeof(media_probe_packet));
      probe_stage = 3;
    } else {
      submit(2, MEDIA_PACKET_LIMIT + 1);
      probe_stage = 4;
    }
  } else if (probe_stage == 4) {
    if (channel->result != -22 || channel->frames) {
      puts("FAIL: media service packet limit was not enforced");
      firmware_media_shutdown();
      return;
    }
    submit(254, 0);
    probe_stage = 5;
  }
}
#else
int firmware_media_probe(void) { return 0; }
void firmware_media_poll(void) { production_poll(); queue_poll(); }
#endif
#else
void firmware_media_child(RendererBootstrap *bootstrap) { (void)bootstrap; __builtin_trap(); }
void firmware_media_shutdown(void) {}
void firmware_media_reset(void) {}
long firmware_media_broker(const uint64_t arguments[5]) { (void)arguments; return -ENOSYS; }
int firmware_media_probe(void) { return 0; }
void firmware_media_poll(void) {}
#endif

/* Stack copies remain valid until the supervisor has consumed the request. */
static long opus_exchange(FirmwareOpusCommand *command, const unsigned char *packet,
                          float *output, size_t samples) {
  if (!renderer_runtime_is_child()) return -ENOSYS;
  unsigned char compressed[FIRMWARE_OPUS_PACKET_LIMIT];
  float decoded[FIRMWARE_OPUS_FRAME_LIMIT * 2] = {0};
  if (command->bytes > sizeof(compressed) || samples > FIRMWARE_OPUS_FRAME_LIMIT * 2)
    return -EINVAL;
  if (command->bytes) memcpy(compressed, packet, command->bytes);
  long ticket;
  do {
    ticket = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 1,
        (uintptr_t)command, command->bytes ? (uintptr_t)compressed : 0, 0, 0);
  } while (ticket == -EAGAIN);
  if (ticket <= 0) return ticket;
  long result;
  do {
    result = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
        output ? (uintptr_t)decoded : 0, samples, 0);
  } while (result == -EAGAIN);
  if (result > 0 && output) {
    /* The broker copies only a bounded frame result; this copy never uses a remote length. */
    memcpy(output, decoded, samples * sizeof(float));
  }
  return result;
}
long firmware_opus_create(unsigned channels, unsigned streams, unsigned coupled,
                         const unsigned char *mapping, int gain, unsigned phase_disabled) {
  if (channels < 1 || channels > 2 || !mapping) return -EINVAL;
  FirmwareOpusCommand command = {.operation = FIRMWARE_OPUS_CREATE,
      .channels = channels, .streams = streams, .coupled = coupled,
      .phase_disabled = phase_disabled, .gain = gain};
  for (unsigned index = 0; index < channels; ++index) command.mapping[index] = mapping[index];
  return opus_exchange(&command, NULL, NULL, 0);
}
long firmware_opus_decode(long handle, const unsigned char *packet, size_t bytes,
                         float *output, size_t samples) {
  if (!bytes || bytes > FIRMWARE_OPUS_PACKET_LIMIT || !packet || !output) return -EINVAL;
  FirmwareOpusCommand command = {.operation = FIRMWARE_OPUS_DECODE,
      .handle = handle, .bytes = bytes};
  return opus_exchange(&command, packet, output, samples);
}
long firmware_opus_reset(long handle) {
  FirmwareOpusCommand command = {.operation = FIRMWARE_OPUS_RESET, .handle = handle};
  return opus_exchange(&command, NULL, NULL, 0);
}
void firmware_opus_close(long handle) {
  FirmwareOpusCommand command = {.operation = FIRMWARE_OPUS_CLOSE, .handle = handle};
  if (handle > 0) (void)opus_exchange(&command, NULL, NULL, 0);
}

long firmware_opus_stream_attach(long handle, long audio, unsigned discard, int64_t origin) {
  long result;
  do { result = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 3, handle, audio, discard, origin); }
  while (result == -EAGAIN);
  return result;
}
long firmware_opus_stream_enqueue(long handle, const FirmwareOpusPacket *packet, const unsigned char *data) {
  FirmwareOpusPacket copy = *packet;
  unsigned char bytes[FIRMWARE_OPUS_PACKET_LIMIT];
  if (copy.bytes > sizeof(bytes)) return -EINVAL;
  if (copy.bytes) memcpy(bytes, data, copy.bytes);
  return (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 4, handle,
      (uintptr_t)&copy, copy.bytes ? (uintptr_t)bytes : 0, 0);
}
long firmware_opus_stream_status(long handle, FirmwareOpusStreamStatus *status) {
  FirmwareOpusStreamStatus copy = {0};
  long result = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 5, handle, (uintptr_t)&copy, 0, 0);
  if (!result) *status = copy;
  return result;
}
