#ifndef UEFI_RENDERER_PAGES_H
#define UEFI_RENDERER_PAGES_H

/* Renderer commitments reuse 1 MiB physical batches to bound service round trips. */
typedef struct PageBatch {
  struct PageBatch *next;
  uint64_t address, used[4];
} PageBatch;
static PageBatch *page_batches;
static void *renderer_page(void) {
  PageBatch *batch=page_batches;
  for (; batch; batch=batch->next)
    for (unsigned word=0; word<4; ++word) if (batch->used[word]!=UINT64_MAX) goto found;
  batch=calloc(1,sizeof(*batch));
  if (!batch) return NULL;
  if (((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))
       platform_firmware()->boot->function[2])(0,2,256,&batch->address)) { free(batch); return NULL; }
  batch->next=page_batches; page_batches=batch;
found:
  for (unsigned word=0; word<4; ++word) if (batch->used[word]!=UINT64_MAX) {
    unsigned bit=__builtin_ctzll(~batch->used[word]);
    batch->used[word]|=1ULL<<bit;
    void *page=(void*)(batch->address+(word*64+bit)*PAGE_SIZE);
    memset(page,0,PAGE_SIZE);
    return page;
  }
  return NULL;
}
static void renderer_page_release(uint64_t address) {
  for (PageBatch **position=&page_batches; *position; position=&(*position)->next) {
    PageBatch *batch=*position;
    if (address<batch->address || address-batch->address>=256*PAGE_SIZE) continue;
    size_t page=(address-batch->address)/PAGE_SIZE;
    uint64_t bit=1ULL<<(page%64);
    if (!(batch->used[page/64]&bit)) abort();
    batch->used[page/64]&=~bit;
    /* Empty batches must release broker quota after a temporary commitment peak. */
    if (!(batch->used[0] | batch->used[1] | batch->used[2] | batch->used[3])) {
      if (((Status(UEFI_API *)(uint64_t,size_t))platform_firmware()->boot->function[3])(
              batch->address,256)) abort();
      *position=batch->next;
      free(batch);
    }
    return;
  }
  abort();
}
static void renderer_pages_release(void) {
  while (page_batches) {
    PageBatch *batch=page_batches;
    for (unsigned word=0; word<4; ++word) if (batch->used[word]) abort();
    if (((Status(UEFI_API *)(uint64_t,size_t))platform_firmware()->boot->function[3])(batch->address,256)) abort();
    page_batches=batch->next;
    free(batch);
  }
}

#endif
