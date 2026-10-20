#include "renderer_runtime.h"
#include "firmware_io.h"
#include "firmware_audio.h"
#include "../network/resource.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

enum {
  ALLOCATE_POOL=1, FREE_POOL, ALLOCATE_PAGES, FREE_PAGES, MEMORY_MAP,
  GET_TIME, GET_VARIABLE, SET_VARIABLE, ENTROPY, PROTECT, LOG, STALL
};
static volatile uint64_t sentinel=0x6b8c1eab00274d91ULL;
#define VERIFY(test) do { \
  if (!(test)) { printf("FAIL: broker probe line=%u\n",__LINE__); return 1; } \
  ++checks; \
} while (0)
int run_renderer_broker_probe(void) {
  if (!renderer_runtime_is_child()) return 0;
  unsigned checks=0;
  uint64_t pool=0, pages=0;
  VERIFY(!renderer_runtime_request(ALLOCATE_POOL,2,256,(uintptr_t)&pool,0,0) && pool);
  VERIFY(!renderer_runtime_request(ALLOCATE_PAGES,0,2,1,(uintptr_t)&pages,0) && pages);
  VERIFY(renderer_runtime_request(FREE_POOL,pages,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(FREE_PAGES,pool,1,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(FREE_PAGES,pages,2,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(FREE_POOL,pool+1,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(FREE_POOL,(uintptr_t)&sentinel,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(ALLOCATE_POOL,2,16,1,0,0)==ERROR(15));
  VERIFY(renderer_runtime_request(ALLOCATE_POOL,2,UINT64_MAX,(uintptr_t)&pool,0,0)==ERROR(9));
  VERIFY(renderer_runtime_request(ALLOCATE_POOL,6,16,(uintptr_t)&pool,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(ALLOCATE_PAGES,2,2,1,(uintptr_t)&pages,0)==ERROR(2));
  VERIFY(renderer_runtime_request(ENTROPY,pool+255,2,0,0,0)==0);
  VERIFY(renderer_runtime_request(ENTROPY,UINT64_MAX-1,4,0,0,0)==0);
  VERIFY(renderer_runtime_request(ENTROPY,(uintptr_t)&sentinel,8,0,0,0)==0);
  VERIFY(renderer_runtime_request(ENTROPY,pool,256,0,0,0)==1);
  VERIFY((int64_t)renderer_runtime_request(PROTECT,(uintptr_t)&sentinel,4096,PROT_READ|PROT_WRITE,0,0)==-1);
  VERIFY((int64_t)renderer_runtime_request(PROTECT,pool,4096,PROT_READ,0,0)==-1);
  VERIFY((int64_t)renderer_runtime_request(PROTECT,pages,4096,PROT_READ|PROT_WRITE|PROT_EXEC,0,0)==-1);
  VERIFY(renderer_runtime_request(PROTECT,pages,4096,PROT_NONE,0,0)==0);
  VERIFY(renderer_runtime_request(ENTROPY,pages,1,0,0,0)==0);
  VERIFY(renderer_runtime_request(FREE_PAGES,pages,1,0,0,0)==ERROR(15));
  VERIFY(renderer_runtime_request(PROTECT,pages,4096,PROT_READ|PROT_WRITE,0,0)==0);
  VERIFY(renderer_runtime_request(MEMORY_MAP,0,0,0,0,0)==ERROR(15));
  VERIFY(renderer_runtime_request(GET_TIME,1,0,0,0,0)==ERROR(15));
  VERIFY((int64_t)renderer_runtime_request(LOG,1,8,0,0,0)==-EFAULT);
  VERIFY((int64_t)renderer_runtime_request(RENDERER_FILE_REQUEST,SYS_open,1,0,0,0)==-EFAULT);
  long arguments[6]={1,0,0,0,0,0};
  VERIFY((int64_t)renderer_runtime_request(RENDERER_FILE_REQUEST,SYS_open,(uintptr_t)arguments,0,0,0)==-EFAULT);
  VERIFY((int64_t)renderer_runtime_request(RENDERER_SOCKET_REQUEST,SYS_poll,1,0,0,0)==-EFAULT);
  arguments[1]=1;
  VERIFY((int64_t)renderer_runtime_request(RENDERER_SOCKET_REQUEST,SYS_poll,(uintptr_t)arguments,0,0,0)==-EFAULT);
  VERIFY((int64_t)renderer_runtime_request(RENDERER_DNS_REQUEST,0,0,1,0,0)==-EFAULT);
  VERIFY((int64_t)renderer_runtime_request(RENDERER_AUDIO_REQUEST,AUDIO_UPLOAD,1,0,1,4)==-EFAULT);
  VERIFY((int64_t)renderer_runtime_request(RENDERER_AUDIO_REQUEST,(1ULL<<32)|AUDIO_UPLOAD,1,0,1,4)==-EINVAL);
  Character name[]={'B','o','o','t','O','r','d','e','r',0};
  Guid identifier={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
  VERIFY(renderer_runtime_request(SET_VARIABLE,(uintptr_t)name,(uintptr_t)&identifier,7,0,0)==ERROR(15));
  size_t length=0;
  VERIFY(renderer_runtime_request(GET_VARIABLE,(uintptr_t)name,(uintptr_t)&identifier,0,(uintptr_t)&length,0)==ERROR(15));
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_REQUEST,1,4,0,0,0)==0);
  ResourceRequestSpec request={"http://10.0.2.2:8765/","GET",(Header*)1,1,NULL,0};
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_REQUEST,(uintptr_t)&request,4,0,0,0)==0);
  Status status=0;
  ResourceResponse response={0};
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RESULT,1,(uintptr_t)&status,(uintptr_t)&response,0,0)==0);
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RELEASE,1,0,0,0,0)==ERROR(2));
  response.body=(void*)1; response.headers=(void*)1; response.header_count=UINT64_MAX;
  VERIFY(renderer_runtime_request(RENDERER_RESPONSE_RELEASE,(uintptr_t)&response,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(RENDERER_RESPONSE_RELEASE,1,0,0,0,0)==ERROR(15));
  request=(ResourceRequestSpec){"http://10.0.2.2:8765/network/echo","GET",NULL,0,NULL,0};
  uint64_t job=renderer_runtime_request(RENDERER_NETWORK_REQUEST,(uintptr_t)&request,4,0,0,0);
  VERIFY(job && job<4096);
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RESULT,job,1,(uintptr_t)&response,0,0)==0);
  time_t deadline=time(NULL)+20;
  unsigned completed=0;
  while (time(NULL)<deadline) {
    if (renderer_runtime_request(RENDERER_NETWORK_RESULT,job,(uintptr_t)&status,(uintptr_t)&response,0,0)) {
      completed=1; break;
    }
    struct timespec delay={0,1000000};
    nanosleep(&delay,NULL);
  }
  VERIFY(completed && status==0 && response.body && response.length);
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RESULT,job,(uintptr_t)&status,(uintptr_t)&response,0,0)==0);
  /* A forged count must never become the trusted allocator's traversal bound. */
  response.header_count=UINT64_MAX;
  VERIFY(!renderer_runtime_request(RENDERER_RESPONSE_RELEASE,(uintptr_t)&response,0,0,0,0));
  VERIFY(!response.body && !response.headers && !response.header_count);
  VERIFY(renderer_runtime_request(RENDERER_RESPONSE_RELEASE,(uintptr_t)&response,0,0,0,0)==ERROR(2));
  VERIFY(!renderer_runtime_request(RENDERER_NETWORK_RELEASE,job,0,0,0,0));
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RELEASE,job,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RESULT,job,(uintptr_t)&status,(uintptr_t)&response,0,0)==0);
  uint64_t replacement=renderer_runtime_request(RENDERER_NETWORK_REQUEST,(uintptr_t)&request,4,0,0,0);
  VERIFY(replacement>job && replacement<4096);
  VERIFY(!renderer_runtime_request(RENDERER_NETWORK_RELEASE,replacement,0,0,0,0));
  VERIFY(renderer_runtime_request(RENDERER_NETWORK_RESULT,replacement,(uintptr_t)&status,(uintptr_t)&response,0,0)==0);
  VERIFY(!renderer_runtime_request(FREE_POOL,pool,0,0,0,0));
  VERIFY(renderer_runtime_request(FREE_POOL,pool,0,0,0,0)==ERROR(2));
  VERIFY(renderer_runtime_request(ENTROPY,pool,1,0,0,0)==0);
  VERIFY(!renderer_runtime_request(FREE_PAGES,pages,1,0,0,0));
  VERIFY(renderer_runtime_request(FREE_PAGES,pages,1,0,0,0)==ERROR(2));
  VERIFY(sentinel==0x6b8c1eab00274d91ULL);
  printf("BROKER PROBE: passed checks=%u; renderer CPL0 isolation remains incomplete\n",checks);
  fflush(stdout);
  return 0;
}
