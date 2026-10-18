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
  errno = 73;
  for (unsigned index = 0; index < 8; ++index) {
    resource_cooperate();
    assert(errno == 73);
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
  puts("PASS: cooperative stack switching, errno isolation, queued requests and abandoned active job cleanup");
}
