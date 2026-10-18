#include "process.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#define PAGE 4096u
#define MAX_WORKERS 4
#define MAX_TABLES 160
#define ADDRESS_MASK 0x000ffffffffff000ULL
#define USER_BASE 0x0000600000000000ULL
#define USER_SOURCE (USER_BASE + 0x200000ULL)
#define USER_OUTPUT (USER_BASE + 0x8000000ULL)
#define NX (1ULL << 63)

typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } Descriptor;
typedef struct __attribute__((packed)) {
  uint16_t low, selector; uint8_t ist, attributes; uint16_t middle;
  uint32_t high, reserved;
} Gate;
typedef struct __attribute__((packed)) {
  uint32_t reserved; uint64_t rsp[3]; uint64_t reserved2;
  uint64_t ist[7]; uint64_t reserved3; uint16_t reserved4, bitmap;
} TaskState;
typedef struct {
  uint64_t kernel_rsp, vector, error, fault, entry, stack, argument, reserved;
  uint64_t code_selector, data_selector, returned_cs;
} Context;
typedef struct {
  uint64_t operation, source, output, width, rows, stride, red_first, address;
} Command;
typedef struct Mp Mp;
struct Mp {
  Status(UEFI_API *count)(Mp*, size_t*, size_t*);
  Status(UEFI_API *information)(Mp*, size_t, void*);
  Status(UEFI_API *start_all)(Mp*, void(UEFI_API *)(void*), uint8_t, void*, size_t, void*, size_t**);
  void *start_one, *switch_bsp, *enable;
  Status(UEFI_API *who)(Mp*, size_t*);
};
typedef struct {
  size_t processor;
  uint64_t *root;
  void *tables[MAX_TABLES];
  size_t table_count;
  void *code, *stack, *kernel_stack, *private_page;
  Command *command;
  uint64_t *gdt;
  Gate *idt;
  TaskState *task;
  Context context;
  uint64_t seen_cr3;
  uint16_t seen_cs;
  uint64_t completed;
  int retired;
} Worker;
static struct {
  SystemTable *table;
  Mp *mp;
  Worker workers[MAX_WORKERS];
  size_t count, source_pages, maximum_pixels;
  void *source, *barrier;
  uint64_t root;
  size_t frames, failures;
  uintptr_t output_address;
  size_t output_length;
  int disabled, persistent, testing;
  uint64_t epoch, stopping, service_epoch, last_frame;
  size_t restarts, idle_stops, serial_frames, burst_frames;
  void *event;
} pool;
extern const unsigned char process_program_start[], process_program_end[];
extern void *process_interrupt_table[256];
extern void process_enter(Context*);
_Static_assert(sizeof(TaskState) == 104, "x86-64 TSS layout");
_Static_assert(sizeof(Gate) == 16, "x86-64 interrupt gate layout");

static void *pages(size_t count) {
  uint64_t address = 0;
  Status status = ((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))
      pool.table->boot->function[2])(0,2,count,&address);
  if (status) return NULL;
  memset((void*)address,0,count*PAGE);
  return (void*)address;
}
static void release(void *address, size_t count) {
  if (address) ((Status(UEFI_API *)(uint64_t,size_t))pool.table->boot->function[3])((uintptr_t)address,count);
}
static uint64_t *new_table(Worker *worker) {
  if (worker->table_count == MAX_TABLES) return NULL;
  void *result = pages(1);
  if (result) worker->tables[worker->table_count++] = result;
  return result;
}
static int map(Worker *worker, uint64_t address, uint64_t physical, size_t count, uint64_t flags) {
  for (size_t page = 0; page < count; ++page, address += PAGE, physical += PAGE) {
    uint64_t *table = worker->root;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
      uint64_t *entry = &table[(address >> shift) & 511];
      if (!*entry) {
        uint64_t *child = new_table(worker);
        if (!child) return 0;
        *entry = (uintptr_t)child | 7;
      }
      if ((*entry & 0x80) || !(*entry & 4)) return 0;
      table = (uint64_t*)(*entry & ADDRESS_MASK);
    }
    table[(address >> 12) & 511] = physical | flags | 5;
  }
  return 1;
}
static uint64_t cache_flags(uint64_t address) {
  uint64_t *table = (uint64_t*)(pool.root & ADDRESS_MASK);
  for (unsigned shift = 39; shift >= 12; shift -= 9) {
    uint64_t entry = table[(address >> shift) & 511];
    if (!(entry & 1)) return 0x18;
    if (shift == 12) return entry & 0x98;
    if (entry & 0x80) return (entry & 0x18) | ((entry >> 5) & 0x80);
    table = (uint64_t*)(entry & ADDRESS_MASK);
  }
  return 0x18;
}
static int initialize_worker(Worker *worker) {
  worker->root = new_table(worker);
  if (!worker->root) return 0;
  uint64_t *source = (uint64_t*)(pool.root & ADDRESS_MASK);
  if (source[USER_BASE >> 39]) return 0;
  // Existing firmware mappings remain supervisor-only in each child root.
  for (size_t index = 0; index < 512; ++index) worker->root[index] = source[index] & ~4ULL;
  worker->code = pages(1);
  worker->stack = pages(2);
  worker->kernel_stack = pages(4);
  worker->private_page = pages(1);
  worker->command = pages(1);
  worker->gdt = pages(16);
  worker->idt = pages(1);
  worker->task = pages(1);
  if (!worker->code || !worker->stack || !worker->kernel_stack || !worker->private_page ||
      !worker->command || !worker->gdt || !worker->idt || !worker->task) return 0;
  size_t length = process_program_end - process_program_start;
  if (length > PAGE) return 0;
  memcpy(worker->code,process_program_start,length);
  worker->context.entry = USER_BASE;
  worker->context.stack = USER_BASE + 4*PAGE;
  worker->context.argument = USER_BASE + PAGE;
  *(uint64_t*)worker->private_page = worker->processor * 100;
  return map(worker,USER_BASE,(uintptr_t)worker->code,1,0) &&
      map(worker,USER_BASE+PAGE,(uintptr_t)worker->command,1,NX) &&
      map(worker,USER_BASE+2*PAGE,(uintptr_t)worker->stack,2,NX|2) &&
      map(worker,USER_BASE+5*PAGE,(uintptr_t)worker->private_page,1,NX|2) &&
      map(worker,USER_BASE+6*PAGE,(uintptr_t)pool.barrier,1,NX|2) &&
      map(worker,USER_SOURCE,(uintptr_t)pool.source,pool.source_pages,NX);
}
static void UEFI_API execute_worker(void *argument) {
  (void)argument;
  size_t processor = 0;
  if (pool.mp->who(pool.mp,&processor)) return;
  Worker *worker = NULL;
  for (size_t index = 0; index < pool.count; ++index)
    if (pool.workers[index].processor == processor) worker = &pool.workers[index];
  if (!worker) return;
  Descriptor previous_gdt, previous_idt;
  uint16_t previous_task, previous_ss, code_selector;
  uint64_t previous_root, flags;
  __asm__ volatile("sgdt %0; sidt %1; str %2; mov %%ss,%3; mov %%cs,%4; mov %%cr3,%5; pushfq; pop %6; cli"
      : "=m"(previous_gdt),"=m"(previous_idt),"=r"(previous_task),"=r"(previous_ss),
        "=r"(code_selector),"=r"(previous_root),"=r"(flags) :: "memory");
  worker->context.reserved = previous_task;
  size_t descriptor_index = ((size_t)previous_gdt.limit + 8) / 8;
  if (descriptor_index > 8187 || (previous_task & 4)) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
    return;
  }
  // APs use no libc, TLS, allocations or firmware services after this point.
  for (size_t index = 0; index < descriptor_index; ++index)
    worker->gdt[index] = ((const uint64_t*)previous_gdt.base)[index];
  worker->gdt[descriptor_index] = 0x00affa000000ffffULL;
  worker->gdt[descriptor_index+1] = 0x00cff2000000ffffULL;
  uintptr_t task_address = (uintptr_t)worker->task;
  worker->gdt[descriptor_index+2] = 103 | ((task_address & 0xffffff) << 16) |
      (0x89ULL << 40) | ((task_address & 0xff000000) << 32);
  worker->gdt[descriptor_index+3] = task_address >> 32;
  worker->context.code_selector = descriptor_index*8 + 3;
  worker->context.data_selector = (descriptor_index+1)*8 + 3;
  uint64_t stack_top = (uintptr_t)worker->kernel_stack + 4*PAGE - 16;
  *(Context**)stack_top = &worker->context;
  worker->task->rsp[0] = stack_top;
  worker->task->bitmap = sizeof(TaskState);
  for (size_t index = 0; index < 256; ++index) {
    uintptr_t entry = (uintptr_t)process_interrupt_table[index];
    Gate *gate = &worker->idt[index];
    gate->low = entry;
    gate->middle = entry >> 16;
    gate->high = entry >> 32;
    gate->selector = code_selector;
    gate->attributes = index == 128 ? 0xee : 0x8e;
    gate->ist = 0;
    gate->reserved = 0;
  }
  Descriptor next_gdt = {(descriptor_index+4)*8-1,(uintptr_t)worker->gdt};
  Descriptor next_idt = {PAGE-1,(uintptr_t)worker->idt};
  uint16_t next_task = (descriptor_index+2)*8;
  uintptr_t root = (uintptr_t)worker->root;
  __asm__ volatile("lgdt %0; lidt %1; ltr %2; mov %3,%%cr3"
      :: "m"(next_gdt),"m"(next_idt),"r"(next_task),"r"(root) : "memory");
  worker->seen_cr3 = root;
  process_enter(&worker->context);
  worker->seen_cs = worker->context.returned_cs;
  // Reload the original TSS through the private GDT before restoring firmware tables.
  if (previous_task) {
    worker->gdt[previous_task/8] &= ~(2ULL << 40);
    __asm__ volatile("ltr %0" :: "r"(previous_task) : "memory");
  }
  __asm__ volatile("mov %0,%%cr3; lgdt %1; lidt %2; mov %3,%%ss; push %4; popfq"
      :: "r"(previous_root),"m"(previous_gdt),"m"(previous_idt),"r"(previous_ss),"r"(flags) : "memory");
}
static void UEFI_API worker_service(void *argument) {
  (void)argument;
  size_t processor = 0;
  if (pool.mp->who(pool.mp,&processor)) return;
  Worker *worker = NULL;
  for (size_t index = 0; index < pool.count; ++index)
    if (pool.workers[index].processor == processor) worker = &pool.workers[index];
  if (!worker) return;
  uint64_t observed = pool.service_epoch;
  while (!__atomic_load_n(&pool.stopping,__ATOMIC_ACQUIRE)) {
    uint64_t epoch = __atomic_load_n(&pool.epoch,__ATOMIC_ACQUIRE);
    if (epoch == observed) { __asm__ volatile("pause"); continue; }
    execute_worker(NULL);
    observed = epoch;
    __atomic_store_n(&worker->completed,epoch,__ATOMIC_RELEASE);
  }
}
static uint64_t milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
  return (uint64_t)now.tv_sec*1000 + now.tv_nsec/1000000;
}
static int start_service(void) {
  __atomic_store_n(&pool.stopping,0,__ATOMIC_RELEASE);
  pool.service_epoch = pool.epoch;
  Status status = pool.mp->start_all(pool.mp,worker_service,0,pool.event,0,NULL,NULL);
  if (status) return 0;
  pool.persistent = 1;
  pool.last_frame = milliseconds();
  ++pool.restarts;
  return 1;
}
static void join_service(void) {
  if (!pool.persistent) return;
  __atomic_store_n(&pool.stopping,1,__ATOMIC_RELEASE);
  size_t attempts = 0;
  while (((Status(UEFI_API *)(void*))pool.table->boot->function[12])(pool.event)) {
    ((Status(UEFI_API *)(size_t))pool.table->boot->function[28])(100);
    if (++attempts == 10000) {
      puts("FAIL: process worker shutdown did not complete; retaining live mappings");
      abort();
    }
  }
  pool.persistent = 0;
}
void render_processes_idle(void) {
  if (!pool.event) return;
  uint64_t elapsed = milliseconds()-pool.last_frame;
  if (pool.persistent) {
    if (__atomic_load_n(&pool.stopping,__ATOMIC_ACQUIRE)) {
      // Firmware completion is polled once so input processing never waits for AP retirement.
      if (!((Status(UEFI_API *)(void*))pool.table->boot->function[12])(pool.event))
        pool.persistent = 0;
    } else if (pool.disabled || elapsed >= 250) {
      __atomic_store_n(&pool.stopping,1,__ATOMIC_RELEASE);
      pool.burst_frames = 0;
      ++pool.idle_stops;
    }
    return;
  }
  // Isolated updates use BSP output. A sustained burst amortizes firmware AP startup.
  // Startup occurs outside the presentation cursor lock after input has been serviced.
  if (!pool.disabled && elapsed < 100 && pool.burst_frames >= 4 && !start_service()) pool.disabled = 1;
}
static int dispatch(uint64_t expected_vector) {
  for (size_t index = 0; index < pool.count; ++index) pool.workers[index].context.vector = UINT64_MAX;
  Status status = 0;
  if (pool.persistent) {
    uint64_t epoch = __atomic_add_fetch(&pool.epoch,1,__ATOMIC_RELEASE);
    // Worker instructions are bounded and faults return through the supervisor.
    size_t attempts = 0;
    for (;;) {
      size_t completed = 0;
      for (size_t index = 0; index < pool.count; ++index)
        completed += __atomic_load_n(&pool.workers[index].completed,__ATOMIC_ACQUIRE) == epoch;
      if (completed == pool.count) break;
      if (++attempts == 100000000) {
        puts("FAIL: process workers did not complete; retaining live mappings");
        abort();
      }
      __asm__ volatile("pause");
    }
  } else status = pool.mp->start_all(pool.mp,execute_worker,0,NULL,500000,NULL,NULL);
  if (status) {
    printf("PROCESS: dispatch failed status=%llx\n",(unsigned long long)status);
    return 0;
  }
  for (size_t index = 0; index < pool.count; ++index) {
    Worker *worker = &pool.workers[index];
    if (worker->context.vector != expected_vector) {
      printf("PROCESS: worker=%zu vector=%llu error=%llx address=%llx\n",index,
          (unsigned long long)worker->context.vector,(unsigned long long)worker->context.error,
          (unsigned long long)worker->context.fault);
      return 0;
    }
  }
  return 1;
}
int render_processes_initialize(SystemTable *table, size_t maximum_pixels) {
  if (pool.count || !maximum_pixels || maximum_pixels > 4096*4096) return 0;
  pool.table = table;
  Guid guid = {0x3fdda605,0xa76e,0x4f46,{0xad,0x29,0x12,0xf4,0x53,0x1b,0x3d,0x08}};
  Status status = ((Status(UEFI_API *)(Guid*,void*,void**))table->boot->function[37])(&guid,NULL,(void**)&pool.mp);
  size_t total = 0, enabled = 0;
  if (status || pool.mp->count(pool.mp,&total,&enabled) || enabled < 2) {
    puts("PROCESS: no application processors; BSP presentation retained");
    return 0;
  }
  uint64_t control;
  __asm__ volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(pool.root),"=r"(control));
  if (control & (1ULL << 12)) return 0;
  pool.maximum_pixels = maximum_pixels;
  pool.source_pages = (maximum_pixels*4 + PAGE-1)/PAGE;
  pool.source = pages(pool.source_pages);
  pool.barrier = pages(1);
  if (!pool.source || !pool.barrier) goto failed;
  for (size_t processor = 0; processor < total && pool.count < MAX_WORKERS; ++processor) {
    struct { uint64_t identifier; uint32_t status; uint32_t location[9]; } information;
    if (pool.mp->information(pool.mp,processor,&information) || (information.status & 3) != 2) continue;
    Worker *worker = &pool.workers[pool.count++];
    worker->processor = processor;
    if (!initialize_worker(worker)) goto failed;
  }
  if (!pool.count) goto failed;
  for (size_t index = 0; index < pool.count; ++index) {
    pool.workers[index].command->operation = 2;
    pool.workers[index].command->address = USER_BASE+5*PAGE;
  }
  if (!dispatch(128)) goto failed;
  for (size_t index = 0; index < pool.count; ++index) {
    Worker *worker = &pool.workers[index];
    if (*(uint64_t*)worker->private_page != worker->processor*100+1 ||
        worker->seen_cr3 != (uintptr_t)worker->root || (worker->seen_cs & 3) != 3) goto failed;
    printf("PROCESS: pid=%zu cpu=%zu cr3=%llx private=%llu cpl=3 previous_tr=%llu\n",index+2,worker->processor,
        (unsigned long long)worker->seen_cr3,(unsigned long long)*(uint64_t*)worker->private_page,(unsigned long long)worker->context.reserved);
    worker->command->operation = 1;
    worker->command->address = (uintptr_t)&pool.maximum_pixels;
  }
  if (!dispatch(14) || pool.maximum_pixels != maximum_pixels) goto failed;
  for (size_t index = 0; index < pool.count; ++index) {
    if ((pool.workers[index].context.error & 7) != 7) goto failed;
    pool.workers[index].command->operation = 3;
    pool.workers[index].command->address = USER_BASE+6*PAGE;
    pool.workers[index].command->width = pool.count;
  }
  if (!dispatch(128) || *(uint64_t*)pool.barrier != pool.count) goto failed;
  printf("PASS: %zu AP workers reached a concurrent user-mode barrier\n",pool.count);
  puts("PASS: isolated worker address spaces, private state and supervisor-write fault recovery");
  uint32_t input[63];
  uint32_t *output = pages(1);
  if (!output) goto failed;
  pool.testing = 1;
  int output_valid = 1;
  for (size_t pixel = 0; pixel < 63; ++pixel) input[pixel] = 0x00123400u + pixel;
  for (int red_first = 0; red_first <= 1; ++red_first) {
    memset(output,0xa5,PAGE);
    if (!render_processes_present(input,output+1,9,7,13,red_first)) output_valid = 0;
    for (size_t pixel = 0; pixel < PAGE/4; ++pixel) {
      uint32_t expected = 0xa5a5a5a5;
      if (pixel >= 1 && (pixel-1)/13 < 7 && (pixel-1)%13 < 9) {
        expected = input[(pixel-1)/13*9+(pixel-1)%13];
        if (red_first) expected = ((expected & 255) << 16) | (expected & 0xff00) | ((expected >> 16) & 255);
      }
      if (output[pixel] != expected) output_valid = 0;
    }
  }
  release(output,1);
  pool.testing = 0;
  pool.frames = 0;
  pool.output_address = 0;
  if (!output_valid) goto failed;
  puts("PASS: process output RGB/BGR conversion, row partitioning, odd width, stride and guards");
  status = ((Status(UEFI_API *)(uint32_t,size_t,void*,void*,void**))table->boot->function[7])(0,0,NULL,NULL,&pool.event);
  if (status) goto failed;
  if (!start_service()) goto failed;
  puts("PROCESS: persistent AP services started with acquire/release mailboxes");
  return pool.count;
failed:
  puts("PROCESS: initialization failed; BSP presentation retained");
  render_processes_shutdown();
  return 0;
}
int render_processes_present(const void *pixels, void *framebuffer,
                             size_t width, size_t height, size_t stride, int red_first) {
  if (pool.disabled || !pool.count || !width || !height || width > 4096 || height > 4096 ||
      stride < width || stride > 8192 || width*height > pool.maximum_pixels) return 0;
  uint64_t now = milliseconds();
  pool.burst_frames = now-pool.last_frame < 100 ? pool.burst_frames+1 : 1;
  pool.last_frame = now;
  if (pool.event && (!pool.persistent || __atomic_load_n(&pool.stopping,__ATOMIC_ACQUIRE))) {
    ++pool.serial_frames;
    return 0;
  }
  size_t length = stride*height*4;
  uintptr_t physical = (uintptr_t)framebuffer & ~(uintptr_t)(PAGE-1);
  size_t offset = (uintptr_t)framebuffer - physical;
  size_t count = (offset + length + PAGE-1)/PAGE;
  memcpy(pool.source,pixels,width*height*4);
  for (size_t index = 0; index < pool.count; ++index) {
    Worker *worker = &pool.workers[index];
    if (pool.output_address != (uintptr_t)framebuffer || pool.output_length != length) {
      for (size_t page = 0; page < count; ++page)
        if (!map(worker,USER_OUTPUT+page*PAGE,physical+page*PAGE,1,NX|2|cache_flags(physical+page*PAGE))) {
          pool.disabled = 1;
          return 0;
        }
    }
    size_t start = height*index/pool.count, end = height*(index+1)/pool.count;
    *worker->command = (Command){0,USER_SOURCE+start*width*4,
        USER_OUTPUT+offset+start*stride*4,width,end-start,stride,red_first,0};
  }
  pool.output_address = (uintptr_t)framebuffer;
  pool.output_length = length;
  if (!dispatch(128)) { ++pool.failures; pool.disabled = 1; return 0; }
  ++pool.frames;
  if (pool.frames == 1 && !pool.testing) printf("PROCESS: first frame presented by %zu isolated AP workers\n",pool.count);
  return 1;
}
static void UEFI_API retire_worker(void *argument) {
  (void)argument;
  size_t processor = 0;
  if (pool.mp->who(pool.mp,&processor)) return;
  for (size_t index = 0; index < pool.count; ++index)
    if (pool.workers[index].processor == processor && pool.workers[index].seen_cr3)
      for (;;) __asm__ volatile("pause");
}
static void UEFI_API verify_retired_worker(void *argument) {
  (void)argument;
  size_t processor = 0;
  if (pool.mp->who(pool.mp,&processor)) return;
  uint16_t task;
  uintptr_t root;
  Descriptor gdt, idt;
  __asm__ volatile("str %0; mov %%cr3,%1; sgdt %2; sidt %3" : "=r"(task),"=r"(root),"=m"(gdt),"=m"(idt));
  for (size_t index = 0; index < pool.count; ++index) {
    Worker *worker = &pool.workers[index];
    if (worker->processor == processor)
      worker->retired = task == 0 && (root & ADDRESS_MASK) != (uintptr_t)worker->root &&
          gdt.base != (uintptr_t)worker->gdt && idt.base != (uintptr_t)worker->idt;
  }
}
void render_processes_shutdown(void) {
  if (!pool.table) return;
  join_service();
  int entered = 0;
  for (size_t index = 0; index < pool.count; ++index) entered |= pool.workers[index].seen_cr3 != 0;
  if (entered) {
    // A null firmware TR cannot be restored with LTR. MP timeout termination
    // resets those APs before their temporary TSS and descriptor pages are freed.
    Status status = pool.mp->start_all(pool.mp,retire_worker,0,NULL,1000,NULL,NULL);
    if (status != ERROR(18) || pool.mp->start_all(pool.mp,verify_retired_worker,0,NULL,500000,NULL,NULL)) abort();
    for (size_t index = 0; index < pool.count; ++index)
      if (!pool.workers[index].retired) abort();
    puts("PASS: worker CPU reset and firmware descriptor restoration before release");
  }
  if (pool.event) ((Status(UEFI_API *)(void*))pool.table->boot->function[11])(pool.event);
  if (pool.count) printf("PROCESS: frames=%zu failures=%zu workers=%zu starts=%zu idle_stops=%zu\n",pool.frames,pool.failures,pool.count,pool.restarts,pool.idle_stops);
  if (pool.count) printf("PROCESS: idle fallback frames=%zu\n",pool.serial_frames);
  for (size_t index = 0; index < pool.count; ++index) {
    Worker *worker = &pool.workers[index];
    release(worker->code,1); release(worker->stack,2); release(worker->kernel_stack,4);
    release(worker->private_page,1); release(worker->command,1); release(worker->gdt,16);
    release(worker->idt,1); release(worker->task,1);
    for (size_t table = 0; table < worker->table_count; ++table) release(worker->tables[table],1);
  }
  release(pool.source,pool.source_pages);
  release(pool.barrier,1);
  memset(&pool,0,sizeof(pool));
}
