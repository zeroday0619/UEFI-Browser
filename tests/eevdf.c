#include "eevdf.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  EevdfEntity first, second;
  EevdfEntity *entities[] = {&first, &second};
  eevdf_initialize(&first, 0, 1);
  eevdf_initialize(&second, 0, 4);
  assert(eevdf_pick(entities, 2) == 1);
  unsigned service[2] = {0, 0};
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    size_t selected = eevdf_pick(entities, 2);
    assert(selected < 2);
    assert(entities[selected]->runtime <= eevdf_time(entities, 2));
    service[selected] += 50;
    eevdf_charge(entities[selected], 50);
  }
  assert(service[1] >= service[0] * 4 - 500);
  assert(service[1] <= service[0] * 4 + 500);
  /* An earlier deadline must not make a task with negative lag eligible. */
  first.runtime = 100;
  first.deadline = 1;
  second.runtime = 0;
  second.deadline = 500;
  assert(eevdf_pick(entities, 2) == 1);
  EevdfEntity newcomer;
  eevdf_initialize(&newcomer, eevdf_time(entities, 2), 1);
  assert(newcomer.runtime == 20);
  eevdf_charge(&newcomer, 2000);
  assert(newcomer.deadline > newcomer.runtime && newcomer.remaining == EEVDF_SLICE_US);
  puts("PASS: eligibility, earliest deadline, 4:1 service fairness, admission and slice overrun");
}
