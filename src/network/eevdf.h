#ifndef UEFI_NETWORK_EEVDF_H
#define UEFI_NETWORK_EEVDF_H
#include <stdint.h>
#include <stddef.h>
/* Cooperative service uses weighted virtual runtime and eligible deadlines. */
typedef struct {
  uint64_t runtime, deadline, remaining;
  unsigned weight;
} EevdfEntity;
#define EEVDF_SLICE_US 500u
static inline uint64_t eevdf_time(EevdfEntity **entities, size_t count) {
  if (!count) return 0;
  uint64_t minimum = entities[0]->runtime;
  for (size_t index = 1; index < count; ++index)
    if (entities[index]->runtime < minimum) minimum = entities[index]->runtime;
  uint64_t total = 0, weight = 0;
  for (size_t index = 0; index < count; ++index) {
    total += (entities[index]->runtime - minimum) * entities[index]->weight;
    weight += entities[index]->weight;
  }
  return minimum + total / weight;
}
static inline void eevdf_initialize(EevdfEntity *entity, uint64_t time, unsigned weight) {
  entity->weight = weight;
  entity->runtime = time;
  entity->remaining = EEVDF_SLICE_US;
  entity->deadline = time + EEVDF_SLICE_US * 1024u / weight;
}
static inline size_t eevdf_pick(EevdfEntity **entities, size_t count) {
  uint64_t time = eevdf_time(entities, count);
  size_t selected = count;
  for (size_t index = 0; index < count; ++index) {
    if (entities[index]->runtime <= time &&
        (selected == count || entities[index]->deadline < entities[selected]->deadline))
      selected = index;
  }
  return selected;
}
static inline void eevdf_charge(EevdfEntity *entity, uint64_t microseconds) {
  if (!microseconds) microseconds = 1;
  entity->runtime += microseconds * 1024u / entity->weight;
  if (microseconds >= entity->remaining) {
    entity->remaining = EEVDF_SLICE_US;
    entity->deadline = entity->runtime + EEVDF_SLICE_US * 1024u / entity->weight;
  } else entity->remaining -= microseconds;
}
#endif
