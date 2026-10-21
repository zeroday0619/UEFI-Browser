/* The BSP owns descriptor metadata and copies every submitted PCM block. */
static long stream_position(Clip* clip) {
  if (active != clip || clip->stream_head == clip->stream_published)
    return clip->stream_position;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    unsigned current = input8(master + 0x14);
    unsigned remaining = input16(master + 0x18);
    if (current != input8(master + 0x14)) continue;
    if (current >= 32) return -EIO;
    uint64_t sequence = clip->stream_head +
                        ((current + 32 - (clip->stream_head % 32)) % 32);
    if (sequence >= clip->stream_published) {
      /* CIV can still name the exhausted last slot just after LVI is extended. */
      if (!remaining && clip->stream_head && current == (clip->stream_head-1)%32)
        return clip->stream_position;
      return -EIO;
    }
    StreamSlot* slot = &clip->stream_slots[sequence % FIRMWARE_AUDIO_STREAM_SLOTS];
    if (remaining > slot->frames * 2) return -EIO;
    uint64_t frame = slot->first + slot->frames - (remaining + 1) / 2;
    uint64_t completed = sequence + (remaining == 0);
    while (clip->stream_head < completed) {
      StreamSlot* retired = &clip->stream_slots[clip->stream_head++ % FIRMWARE_AUDIO_STREAM_SLOTS];
      clip->stream_used[retired->storage] = 0;
    }
    if (frame < clip->stream_position || frame > clip->stream_submitted)
      return -EIO;
    clip->stream_position = frame;
    return frame;
  }
  return -EAGAIN;
}
static void stream_descriptor(Clip* clip, uint64_t sequence) {
  StreamSlot* slot = &clip->stream_slots[sequence % FIRMWARE_AUDIO_STREAM_SLOTS];
  Descriptor* descriptors = (void*)(uintptr_t)descriptor_memory;
  descriptors[sequence % 32] = (Descriptor){
      (uint32_t)(clip->memory + slot->storage * 16384 + slot->offset),
      slot->frames * 2};
}
static void stream_publish(Clip* clip) {
  uint64_t previous = clip->stream_published;
  /* One hardware slot remains free so CIV cannot alias a full queue rotation. */
  while (clip->stream_published < clip->stream_tail &&
         clip->stream_published - clip->stream_head < 31)
    stream_descriptor(clip, clip->stream_published++);
  if (clip->stream_published != previous) {
    __asm__ volatile("mfence" ::: "memory");
    output8(master + 0x15, (clip->stream_published - 1) % 32);
  }
}
void firmware_audio_poll(void) {
  /* The BSP services retained PCM even while its renderer cannot run a task. */
  if (renderer_runtime_is_child() || !active || !active->streaming) return;
  if (stream_position(active) >= 0) stream_publish(active);
}
static long stream_pause(Clip* clip) {
  if (active == clip) {
    command((previous_command | 1) & ~4);
    output8(master + 0x1b, 0);
    long result = stream_position(clip);
    stop();
    if (result < 0) return result;
  }
  /* A reset restarts CIV at zero. Retain PCM storage and rotate metadata only. */
  StreamSlot pending[FIRMWARE_AUDIO_STREAM_SLOTS];
  size_t count = 0;
  for (uint64_t index = clip->stream_head; index < clip->stream_tail; ++index) {
    StreamSlot slot = clip->stream_slots[index % FIRMWARE_AUDIO_STREAM_SLOTS];
    if (clip->stream_position > slot.first) {
      unsigned consumed = clip->stream_position - slot.first;
      if (consumed > slot.frames) return -EIO;
      slot.offset += consumed * 4;
      slot.frames -= consumed;
      slot.first += consumed;
    }
    if (slot.frames) pending[count++] = slot;
    else clip->stream_used[slot.storage] = 0;
  }
  memcpy(clip->stream_slots, pending, count * sizeof(StreamSlot));
  clip->stream_head = 0;
  clip->stream_tail = count;
  clip->stream_published = 0;
  return clip->stream_position;
}
static long stream_request(Clip* clip, unsigned operation, uint64_t first,
                           uint64_t second, uint64_t third) {
  switch (operation) {
    case AUDIO_POSITION: {
      long result = stream_position(clip);
      if (result >= 0 && active == clip) stream_publish(clip);
      return result;
    }
    case AUDIO_STOP:
      return stream_pause(clip);
    case AUDIO_STREAM_RESET:
      if (first || second || third) return -EINVAL;
      if (active == clip) stop();
      clip->stream_head = clip->stream_tail = clip->stream_submitted = 0;
      clip->stream_position = clip->stream_published = 0;
      memset(clip->stream_used, 0, sizeof(clip->stream_used));
      return 0;
    case AUDIO_STREAM_WRITE: {
      if (!third || third > 16384 || third % 4 || !second ||
          first != clip->stream_submitted || first > LONG_MAX - third / 4)
        return -EINVAL;
      long result = stream_position(clip);
      if (result < 0) return result;
      if (active == clip) stream_publish(clip);
      if (clip->stream_tail - clip->stream_head >= FIRMWARE_AUDIO_STREAM_SLOTS) return -EAGAIN;
      unsigned storage = 0;
      while (storage < FIRMWARE_AUDIO_STREAM_SLOTS && clip->stream_used[storage]) ++storage;
      if (storage == FIRMWARE_AUDIO_STREAM_SLOTS) return -EIO;
      size_t offset = storage * 16384;
      memcpy((unsigned char*)clip->source + offset,
             (const void*)(uintptr_t)second, third);
      apply_gain(clip, offset, third);
      uint64_t sequence = clip->stream_tail++;
      clip->stream_slots[sequence % FIRMWARE_AUDIO_STREAM_SLOTS] =
          (StreamSlot){first, storage, 0, third / 4};
      clip->stream_used[storage] = 1;
      clip->stream_submitted += third / 4;
      if (active == clip) stream_publish(clip);
      return third;
    }
    case AUDIO_START: {
      if (first || third || second < 8000 || second > 48000) return -EINVAL;
      if (active && active != clip) return -EBUSY;
      if (active == clip) {
        output16(mixer + 0x2c, second);
        return input16(mixer + 0x2c) == second ? 0 : -EIO;
      }
      if (clip->stream_head == clip->stream_tail) return -EAGAIN;
      long result = stream_pause(clip);
      if (result < 0) return result;
      stop();
      if (input8(master + 0x1b) & 2) return -EIO;
      clip->stream_published = clip->stream_tail < 31 ? clip->stream_tail : 31;
      for (uint64_t index = 0; index < clip->stream_published; ++index)
        stream_descriptor(clip, index);
      output16(mixer + 0x2c, second);
      if (input16(mixer + 0x2c) != second) return -EIO;
      output16(mixer + 0x18, 0);
      output32(master + 0x10, descriptor_memory);
      output16(master + 0x16, 0x1c);
      output8(master + 0x15, clip->stream_published - 1);
      active = clip;
      __asm__ volatile("mfence" ::: "memory");
      command(previous_command | 5);
      output8(master + 0x1b, 1);
      return 0;
    }
    case AUDIO_VOLUME:
      if (first > 65536) return -EINVAL;
      clip->gain = first;
      for (uint64_t index = clip->stream_head; index < clip->stream_tail; ++index) {
        StreamSlot* slot = &clip->stream_slots[index % FIRMWARE_AUDIO_STREAM_SLOTS];
        apply_gain(clip, slot->storage * 16384 + slot->offset, slot->frames * 4);
      }
      return 0;
    default:
      return -EINVAL;
  }
}
