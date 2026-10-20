#define _GNU_SOURCE
#include "resource.h"
#ifdef UEFI_RUNTIME
#include "renderer_runtime.h"
#include "firmware_io.h"
static SystemTable *service_table;
#endif
#include "eevdf.h"
#include <time.h>
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
  EevdfEntity service;
  unsigned char *stack;
  SystemTable *table;
  char *url;
  char *method;
  unsigned char *body;
  size_t body_length;
  Header *headers;
  size_t header_count;
  ResourceResponse response;
  Status status;
  int finished, abandoned, started;
#ifdef UEFI_RUNTIME
  int remote;
  uint64_t remote_identifier;
  int remote_delivered;
  struct ResourceJob *remote_next;
#endif
  int saved_errno;
  struct ResourceJob *next;
};
#ifdef UEFI_RUNTIME
static ResourceJob *remote_jobs;
static uint64_t next_remote_identifier=1;
typedef struct RemoteResponse RemoteResponse;
struct RemoteResponse { ResourceResponse response, source; void *storage; size_t storage_size; RemoteResponse *next; };
static void remote_response_destroy(RemoteResponse *entry) {
  if (entry->storage) {
    renderer_runtime_export_free(entry->storage,entry->storage_size);
    resource_release(&entry->source);
  } else resource_release(&entry->response);
  free(entry);
}
static int remote_response_export(RemoteResponse *entry) {
  ResourceResponse *source=&entry->response;
  if (source->length>RESOURCE_BODY_LIMIT || source->header_count>1024) return 0;
  size_t size=source->header_count*sizeof(Header)+source->length+1;
  for (size_t index=0;index<source->header_count;++index) {
    size_t name=strnlen(source->headers[index].name,16384),value=strnlen(source->headers[index].value,65536);
    if (name==16384 || value==65536 || size>16*1024*1024-name-value-2) return 0;
    size+=name+value+2;
  }
  unsigned char *storage=renderer_runtime_export_allocate(size);
  if (!storage) return 0;
  entry->source=*source; entry->storage=storage; entry->storage_size=size;
  Header *headers=(void*)storage;
  unsigned char *position=storage+source->header_count*sizeof(Header);
  source->body=position;
  if (source->length) memcpy(position,entry->source.body,source->length);
  position+=source->length; *position++=0;
  for (size_t index=0;index<source->header_count;++index) {
    size_t length=strlen(entry->source.headers[index].name)+1;
    headers[index].name=(char*)position; memcpy(position,entry->source.headers[index].name,length); position+=length;
    length=strlen(entry->source.headers[index].value)+1;
    headers[index].value=(char*)position; memcpy(position,entry->source.headers[index].value,length); position+=length;
  }
  source->headers=headers;
  return 1;
}
static RemoteResponse *remote_responses;
static void remote_unlink(ResourceJob *job) {
  ResourceJob **position=&remote_jobs;
  while (*position && *position!=job) position=&(*position)->remote_next;
  if (*position) *position=job->remote_next;
}
#endif
static Context scheduler;
static ResourceJob *first, *last, *active;
static int executing;
static int scheduler_errno;
static unsigned job_count;
static uint64_t dispatches, maximum_slice, switches;
static ResourceJob *previous;
static uint64_t microseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static size_t runnable(ResourceJob **jobs, EevdfEntity **entities) {
  size_t count = 0;
  for (ResourceJob *job = first; job; job = job->next) {
    jobs[count] = job;
    entities[count++] = &job->service;
  }
  return count;
}
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
int resource_async_available(void) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return 1;
#endif
  return yield_handle != NULL;
}
Status resource_async_initialize(SystemTable *table) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return 0;
  service_table=table;
#endif
  return ((Status(UEFI_API *)(Handle*, Guid*, int, void*))table->boot->function[13])(
      &yield_handle, &yield_guid, 0, &yield_protocol);
}
static void release_job(ResourceJob *job) {
#ifdef UEFI_RUNTIME
  if (job->remote) remote_unlink(job);
#endif
  resource_release(&job->response);
  for (size_t index = 0; index < job->header_count; ++index) {
    free(job->headers[index].name); free(job->headers[index].value);
  }
  free(job->headers); free(job->url); free(job->method); free(job->body);
  if (job->stack) munmap(job->stack - 4096, 512 * 1024 + 8192);
  free(job);
  --job_count;
}
void resource_async_execute(ResourceJob *job) {
  ResourceRequestSpec request = {job->url, job->method, job->headers,
      job->header_count, job->body, job->body_length};
  job->status = resource_request(job->table, &request, &job->response);
  job->finished = 1;
#ifdef UEFI_RUNTIME
  if (job->remote) printf("NETWORK_SERVICE: completed status=%llx bytes=%zu\n",
      (unsigned long long)job->status,job->response.length);
#endif
  resource_cooperate();
  abort();
}
ResourceJob *resource_async_start_priority(SystemTable *table, const char *url,
                                const Header *headers, size_t count, unsigned weight) {
  ResourceRequestSpec request = {url, "GET", headers, count, NULL, 0};
  return resource_async_request(table, &request, weight);
}
ResourceJob *resource_async_request(SystemTable *table, const ResourceRequestSpec *request,
                                  unsigned weight) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return (ResourceJob*)renderer_runtime_request(
      RENDERER_NETWORK_REQUEST,(uintptr_t)request,weight,0,0,0);
#endif
  if (!request || !request->url || !resource_method_supported(request->method) ||
      request->body_length > RESOURCE_BODY_LIMIT ||
      (request->body_length && !request->body) ||
      (request->header_count && !request->headers)) return NULL;
  const char *url = request->url;
  const Header *headers = request->headers;
  size_t count = request->header_count;
  if (weight != 1 && weight != 4) return NULL;
  if (count > 64 || job_count >= 128) return NULL;
  ResourceJob *job = calloc(1, sizeof(*job));
  if (!job) return NULL;
  ++job_count;
  job->table = table;
  job->url = strdup(url);
  job->method = strdup(request->method);
  if (request->body_length) {
    job->body = malloc(request->body_length);
    if (job->body) memcpy(job->body, request->body, request->body_length);
  }
  job->body_length = request->body_length;
  void *mapping = mmap(NULL, 512 * 1024 + 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping != MAP_FAILED) {
    job->stack = (unsigned char*)mapping + 4096;
    if (mprotect(job->stack, 512 * 1024, PROT_READ | PROT_WRITE)) {
      munmap(mapping, 512 * 1024 + 8192); job->stack = NULL;
    }
  }
  job->headers = calloc(count ? count : 1, sizeof(Header));
  if (!job->url || !job->method || !job->stack || !job->headers ||
      (job->body_length && !job->body)) { release_job(job); return NULL; }
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
  ResourceJob *jobs[128];
  EevdfEntity *entities[128];
  size_t queued = runnable(jobs, entities);
  eevdf_initialize(&job->service, eevdf_time(entities, queued), weight);
  if (last) last->next = job; else first = job;
  last = job;
  return job;
}
ResourceJob *resource_async_start(SystemTable *table, const char *url,
                                const Header *headers, size_t count) {
  return resource_async_start_priority(table, url, headers, count, 1);
}
void resource_async_process(void) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return;
#endif
  if (executing) abort();
  ResourceJob *jobs[128];
  EevdfEntity *entities[128];
  size_t count = runnable(jobs, entities);
  if (!count) return;
  active = jobs[eevdf_pick(entities, count)];
  if (active != previous) ++switches;
  previous = active;
  if (active->abandoned && !active->started) active->finished = 1;
  if (!active->finished) {
    active->started = 1;
    scheduler_errno = errno;
    errno = active->saved_errno;
    uint64_t start = microseconds();
    executing = 1;
    resource_switch(&scheduler, &active->context);
    executing = 0;
    uint64_t elapsed = microseconds() - start;
    eevdf_charge(&active->service, elapsed);
    ++dispatches;
    if (elapsed > maximum_slice) maximum_slice = elapsed;
  }
  ResourceJob *completed = active;
  active = NULL;
  if (completed->finished) {
    ResourceJob **position = &first;
    ResourceJob *prior = NULL;
    while (*position != completed) { prior = *position; position = &prior->next; }
    *position = completed->next;
    if (last == completed) last = prior;
    completed->next = NULL;
    if (previous == completed) previous = NULL;
    if (completed->abandoned) release_job(completed);
  }
}
int resource_async_result(ResourceJob *job, Status *status, ResourceResponse *response) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return renderer_runtime_request(RENDERER_NETWORK_RESULT,
      (uintptr_t)job,(uintptr_t)status,(uintptr_t)response,0,0);
#endif
  if (!job->finished) return 0;
  *status = job->status;
  *response = job->response;
  memset(&job->response, 0, sizeof(job->response));
  return 1;
}
void resource_async_release(ResourceJob *job) {
  if (!job) return;
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) { renderer_runtime_request(RENDERER_NETWORK_RELEASE,(uintptr_t)job,0,0,0,0); return; }
#endif
  if (job->finished && job != active) release_job(job);
  else job->abandoned = 1;
}
void resource_async_shutdown(SystemTable *table) {
#ifdef UEFI_RUNTIME
  if (renderer_runtime_is_child()) return;
#endif
  // A suspended firmware token retains its stack until cancellation or completion.
  while (active || first) resource_async_process();
  ((Status(UEFI_API *)(Handle, Guid*, void*))table->boot->function[15])(
      yield_handle, &yield_guid, &yield_protocol);
  yield_handle = NULL;
  printf("SCHEDULER: EEVDF dispatches=%llu switches=%llu maximum_slice_us=%llu\n",
      (unsigned long long)dispatches, (unsigned long long)switches, (unsigned long long)maximum_slice);
}

#ifdef UEFI_RUNTIME
void renderer_network_reset(void) {
  firmware_socket_reset(1);
  firmware_file_reset(1);
  firmware_dns_reset(1);
  size_t jobs=0, responses=0;
  while (remote_jobs) {
    ResourceJob *job=remote_jobs; remote_jobs=job->remote_next;
    job->remote=0; job->remote_next=NULL;
    resource_async_release(job); ++jobs;
  }
  while (remote_responses) {
    RemoteResponse *entry=remote_responses; remote_responses=entry->next;
    remote_response_destroy(entry); ++responses;
  }
  printf("NETWORK_SERVICE: detached renderer jobs=%zu responses=%zu\n",jobs,responses);
}
static ResourceJob *remote_lookup(uint64_t identifier) {
  for (ResourceJob *job=remote_jobs; job; job=job->remote_next)
    if (job->remote_identifier==identifier) return job;
  return NULL;
}
static uint64_t remote_request(const ResourceRequestSpec *source, unsigned weight, unsigned legacy) {
  ResourceRequestSpec request=*source;
  if (request.header_count>64 || request.body_length>RESOURCE_BODY_LIMIT ||
      !renderer_runtime_buffer((uintptr_t)request.headers,request.header_count*sizeof(Header),0) ||
      !renderer_runtime_buffer((uintptr_t)request.body,request.body_length,0)) return 0;
  char url[RESOURCE_URL_LIMIT], method[16];
  if (!renderer_runtime_string(url,sizeof(url),(uintptr_t)request.url)) return 0;
  if (legacy) memcpy(method,"GET",4);
  else if (!renderer_runtime_string(method,sizeof(method),(uintptr_t)request.method)) return 0;
  Header source_headers[64], headers[64]={0};
  if (request.header_count) memcpy(source_headers,request.headers,request.header_count*sizeof(Header));
  void *body=NULL;
  uint64_t identifier=0;
  for (size_t index=0; index<request.header_count; ++index) {
    headers[index].name=malloc(1024); headers[index].value=malloc(16384);
    if (!headers[index].name || !headers[index].value ||
        !renderer_runtime_string(headers[index].name,1024,(uintptr_t)source_headers[index].name) ||
        !renderer_runtime_string(headers[index].value,16384,(uintptr_t)source_headers[index].value)) goto cleanup;
  }
  if (request.body_length) {
    body=malloc(request.body_length);
    if (!body) goto cleanup;
    memcpy(body,request.body,request.body_length);
  }
  if (!next_remote_identifier || next_remote_identifier>INTPTR_MAX) goto cleanup;
  request.url=url; request.method=method; request.headers=headers; request.body=body;
  ResourceJob *job=resource_async_request(service_table,&request,weight);
  if (job) {
    identifier=next_remote_identifier++;
    job->remote_identifier=identifier;
    job->remote=1; job->remote_next=remote_jobs; remote_jobs=job;
  }
cleanup:
  for (size_t index=0; index<request.header_count; ++index) {
    free(headers[index].name); free(headers[index].value);
  }
  free(body);
  return identifier;
}
uint64_t renderer_network_service(unsigned operation, uint64_t first, uint64_t second,
    uint64_t third, uint64_t fourth, uint64_t fifth) {
  (void)fifth;
  switch (operation) {
    case RENDERER_NETWORK_REQUEST: {
      ResourceRequestSpec request;
      if (!renderer_runtime_buffer(first,sizeof(request),0)) return 0;
      memcpy(&request,(const void*)first,sizeof(request));
      return remote_request(&request,second,0);
    }
    case RENDERER_NETWORK_START: {
      /* Legacy requests have no method pointer to validate in the renderer. */
      ResourceRequestSpec request={(const char*)first,NULL,(const Header*)second,third,NULL,0};
      return remote_request(&request,fourth,1);
    }
    case RENDERER_NETWORK_RESULT: {
      ResourceJob *job=remote_lookup(first);
      if (!job || !job->finished || job->remote_delivered ||
          !renderer_runtime_buffer(second,sizeof(Status),1) ||
          !renderer_runtime_buffer(third,sizeof(ResourceResponse),1)) return 0;
      RemoteResponse *entry=calloc(1,sizeof(*entry));
      if (!entry) return 0;
      Status status;
      int result=resource_async_result(job,&status,&entry->response);
      if (renderer_runtime_sandboxed() && !remote_response_export(entry)) {
        resource_release(&entry->response); memset(&entry->response,0,sizeof(entry->response)); status=ERROR(9);
      }
      memcpy((void*)second,&status,sizeof(status));
      memcpy((void*)third,&entry->response,sizeof(entry->response));
      job->remote_delivered=1;
      entry->next=remote_responses; remote_responses=entry;
      return result;
    }
    case RENDERER_NETWORK_RELEASE: {
      ResourceJob *job=remote_lookup(first);
      if (!job) return ERROR(2);
      remote_unlink(job); job->remote=0; job->remote_next=NULL;
      resource_async_release(job); return 0;
    }
    case RENDERER_RESPONSE_RELEASE: {
      ResourceResponse response;
      if (!renderer_runtime_buffer(first,sizeof(response),1)) return ERROR(15);
      memcpy(&response,(const void*)first,sizeof(response));
      RemoteResponse **position=&remote_responses;
      while (*position && ((*position)->response.body!=response.body || (*position)->response.headers!=response.headers)) position=&(*position)->next;
      if (!*position) return ERROR(2);
      RemoteResponse *entry=*position; *position=entry->next;
      remote_response_destroy(entry);
      memset((void*)first,0,sizeof(response));
      return 0;
    }
    default: return ERROR(3);
  }
}
#endif
