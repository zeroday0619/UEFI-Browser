#define _GNU_SOURCE
#include "resource.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>

typedef struct {
  uint64_t saved[7];
  uint32_t mxcsr;
  uint16_t control, padding;
} Context;
extern void resource_switch(Context *, Context *);
extern void resource_enter(void);
__asm__(
".text\n"
".global resource_switch\nresource_switch:\n"
"mov %rbx,0(%rdi)\nmov %rbp,8(%rdi)\nmov %r12,16(%rdi)\n"
"mov %r13,24(%rdi)\nmov %r14,32(%rdi)\nmov %r15,40(%rdi)\nmov %rsp,48(%rdi)\n"
"stmxcsr 56(%rdi)\nfnstcw 60(%rdi)\n"
"mov 0(%rsi),%rbx\nmov 8(%rsi),%rbp\nmov 16(%rsi),%r12\n"
"mov 24(%rsi),%r13\nmov 32(%rsi),%r14\nmov 40(%rsi),%r15\nmov 48(%rsi),%rsp\n"
"ldmxcsr 56(%rsi)\nfldcw 60(%rsi)\nret\n"
".global resource_enter\nresource_enter:\nmov %r12,%rdi\ncall resource_async_execute\nud2\n");
struct ResourceJob {
  Context context;
  unsigned char *stack;
  SystemTable *table;
  char *url;
  Header *headers;
  size_t header_count;
  ResourceResponse response;
  Status status;
  int finished, abandoned, started;
  int saved_errno;
  struct ResourceJob *next;
};
static Context scheduler;
static ResourceJob *first, *last, *active;
static int executing;
static int scheduler_errno;
static unsigned job_count;
void resource_cooperate(void) {
  if (executing) {
    active->saved_errno = errno;
    errno = scheduler_errno;
    executing = 0;
    resource_switch(&active->context, &scheduler);
    executing = 1;
  }
}
static void UEFI_API firmware_yield(void) { resource_cooperate(); }
static Guid yield_guid = {0x38bed393,0x2ced,0x41da,{0x8d,0x01,0x78,0x72,0xbc,0x62,0xde,0x24}};
static struct { void (UEFI_API *yield)(void); } yield_protocol = {firmware_yield};
static Handle yield_handle;
int resource_async_available(void) { return yield_handle != NULL; }
Status resource_async_initialize(SystemTable *table) {
  return ((Status(UEFI_API *)(Handle*, Guid*, int, void*))table->boot->function[13])(
      &yield_handle, &yield_guid, 0, &yield_protocol);
}
static void release_job(ResourceJob *job) {
  resource_release(&job->response);
  for (size_t index = 0; index < job->header_count; ++index) {
    free(job->headers[index].name); free(job->headers[index].value);
  }
  free(job->headers); free(job->url);
  if (job->stack) munmap(job->stack - 4096, 512 * 1024 + 8192);
  free(job);
  --job_count;
}
void resource_async_execute(ResourceJob *job) {
  job->status = resource_get_with_headers(job->table, job->url,
      job->headers, job->header_count, &job->response);
  job->finished = 1;
  resource_cooperate();
  abort();
}
ResourceJob *resource_async_start(SystemTable *table, const char *url,
                                const Header *headers, size_t count) {
  if (count > 64 || job_count >= 128) return NULL;
  ResourceJob *job = calloc(1, sizeof(*job));
  if (!job) return NULL;
  ++job_count;
  job->table = table;
  job->url = strdup(url);
  void *mapping = mmap(NULL, 512 * 1024 + 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping != MAP_FAILED) {
    job->stack = (unsigned char*)mapping + 4096;
    if (mprotect(job->stack, 512 * 1024, PROT_READ | PROT_WRITE)) {
      munmap(mapping, 512 * 1024 + 8192); job->stack = NULL;
    }
  }
  job->headers = calloc(count ? count : 1, sizeof(Header));
  if (!job->url || !job->stack || !job->headers) { release_job(job); return NULL; }
  for (size_t index = 0; index < count; ++index) {
    job->headers[index].name = strdup(headers[index].name);
    job->headers[index].value = strdup(headers[index].value);
    ++job->header_count;
    if (!job->headers[index].name || !job->headers[index].value) { release_job(job); return NULL; }
  }
  uintptr_t top = ((uintptr_t)job->stack + 512 * 1024) & ~(uintptr_t)15;
  *(uintptr_t*)(top - 8) = (uintptr_t)resource_enter;
  job->context.saved[2] = (uintptr_t)job;
  job->context.saved[6] = top - 8;
  job->context.mxcsr = 0x1f80;
  job->context.control = 0x37f;
  if (last) last->next = job; else first = job;
  last = job;
  return job;
}
void resource_async_process(void) {
  if (executing) abort();
  if (!active) {
    active = first;
    if (!active) return;
    first = first->next;
    if (!first) last = NULL;
  }
  if (active->abandoned && !active->started) active->finished = 1;
  if (!active->finished) {
    active->started = 1;
    scheduler_errno = errno;
    errno = active->saved_errno;
    executing = 1;
    resource_switch(&scheduler, &active->context);
    executing = 0;
  }
  if (active->finished) {
    ResourceJob *completed = active;
    active = NULL;
    if (completed->abandoned) release_job(completed);
  }
}
int resource_async_result(ResourceJob *job, Status *status, ResourceResponse *response) {
  if (!job->finished) return 0;
  *status = job->status;
  *response = job->response;
  memset(&job->response, 0, sizeof(job->response));
  return 1;
}
void resource_async_release(ResourceJob *job) {
  if (!job) return;
  if (job->finished && job != active) release_job(job);
  else job->abandoned = 1;
}
void resource_async_shutdown(SystemTable *table) {
  // A suspended firmware token retains its stack until cancellation or completion.
  while (active || first) resource_async_process();
  ((Status(UEFI_API *)(Handle, Guid*, void*))table->boot->function[15])(
      yield_handle, &yield_guid, &yield_protocol);
  yield_handle = NULL;
}
