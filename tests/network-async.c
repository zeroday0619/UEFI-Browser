#include "resource.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned requests;
Status resource_get_with_headers(SystemTable *table, const char *url,
    const Header *headers, size_t count, ResourceResponse *response) {
  (void)table; (void)headers; (void)count;
  ++requests;
  int expected_errno = 70 + (int)strlen(url);
  errno = expected_errno;
  unsigned iterations = !strcmp(url, "slow") ? 200 : 8;
  for (unsigned index = 0; index < iterations; ++index) {
    resource_cooperate();
    assert(errno == expected_errno);
  }
  response->status_code = 200;
  response->body = (unsigned char*)strdup(url);
  response->length = strlen(url);
  return 0;
}
void resource_release(ResourceResponse *response) {
  free(response->body);
  memset(response, 0, sizeof(*response));
}
int main(void) {
  ResourceJob *first = resource_async_start(NULL, "first", NULL, 0);
  ResourceJob *second = resource_async_start(NULL, "second", NULL, 0);
  assert(first && second);
  errno = 41;
  resource_async_process();
  assert(errno == 41);
  resource_async_release(first);
  for (unsigned index = 0; index < 32; ++index) {
    resource_async_process();
    assert(errno == 41);
  }
  ResourceResponse response = {0};
  Status status;
  assert(resource_async_result(second, &status, &response));
  assert(!status && response.status_code == 200 && !strcmp((char*)response.body, "second"));
  resource_async_release(second);
  resource_release(&response);
  assert(requests == 2);
  ResourceJob *slow = resource_async_start(NULL, "slow", NULL, 0);
  ResourceJob *fast = resource_async_start_priority(NULL, "fast", NULL, 0, 4);
  ResourceJob *cancelled = resource_async_start(NULL, "cancelled", NULL, 0);
  assert(slow && fast && cancelled);
  resource_async_release(cancelled);
  unsigned steps = 0;
  while (!resource_async_result(fast, &status, &response) && steps++ < 100)
    resource_async_process();
  assert(steps < 100 && status == 0);
  resource_release(&response);
  assert(!resource_async_result(slow, &status, &response));
  resource_async_release(fast);
  resource_async_release(slow);
  for (unsigned index = 0; index < 300; ++index) resource_async_process();
  assert(requests == 4);
  assert(!resource_async_start_priority(NULL, "invalid", NULL, 0, 0));
  puts("PASS: cooperative stack switching, errno isolation, concurrent completion, priority, queued cancellation and abandoned active job cleanup");
}
