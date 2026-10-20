#define _GNU_SOURCE
#include "uefi.h"
#include "renderer_runtime.h"
#include "firmware_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FILE_BASE 64
#define FILE_COUNT 64
#define FILE_CREATE (1ULL << 63)
#define FILE_DIRECTORY 16
#define CALL(index,type) ((type)table->boot->function[index])
extern SystemTable *platform_firmware(void);
typedef struct File File;
struct File {
  uint64_t revision;
  Status(UEFI_API *open)(File *,File **,Character *,uint64_t,uint64_t);
  Status(UEFI_API *close)(File *);
  Status(UEFI_API *remove)(File *);
  Status(UEFI_API *read)(File *,size_t *,void *);
  Status(UEFI_API *write)(File *,size_t *,void *);
  Status(UEFI_API *position)(File *,uint64_t *);
  Status(UEFI_API *seek)(File *,uint64_t);
  Status(UEFI_API *info)(File *,Guid *,size_t *,void *);
  Status(UEFI_API *set_info)(File *,Guid *,size_t,void *);
  Status(UEFI_API *flush)(File *);
};
typedef struct { uint64_t size, length, physical; unsigned char times[48]; uint64_t attributes; Character name[512]; } FileInfo;
_Static_assert(offsetof(FileInfo,name) == 80, "EFI_FILE_INFO ABI");
typedef struct { uint32_t revision; Handle parent; SystemTable *system; Handle device; } LoadedImage;
typedef struct { uint64_t revision; Status(UEFI_API *open)(void *,File **); } FileSystem;
typedef struct { File *file; unsigned owner; int flags; } Descriptor;
static Descriptor descriptors[FILE_COUNT];
static Guid loaded_image = {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static Guid file_system = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static Guid file_info = {0x9576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static Handle device;
static File *profile;
static int status_error(Status status) {
  if (!status) return 0;
  switch (status & ~(1ULL << 63)) {
    case 2: return EINVAL;
    case 3: return EOPNOTSUPP;
    case 4: case 5: return EOVERFLOW;
    case 8: return EROFS;
    case 9: return ENOMEM;
    case 10: return EUCLEAN;
    case 11: return ENOSPC;
    case 14: return ENOENT;
    case 15: return EACCES;
    default: return EIO;
  }
}
void firmware_file_initialize(void *image) {
  if (renderer_runtime_is_child()) return;
  SystemTable *table = platform_firmware(); LoadedImage *loaded = NULL;
  if (!CALL(16,Status(UEFI_API *)(Handle,Guid *,void **))(image,&loaded_image,(void **)&loaded)) device = loaded->device;
}
int firmware_file_descriptor(long descriptor) { return descriptor >= FILE_BASE && descriptor < FILE_BASE+FILE_COUNT; }
static Descriptor *lookup(unsigned owner,long descriptor) {
  if (!firmware_file_descriptor(descriptor)) return NULL;
  Descriptor *entry = &descriptors[descriptor-FILE_BASE];
  return entry->file && entry->owner == owner ? entry : NULL;
}
static int open_profile(void) {
  if (profile) return 0;
  if (!device) return -ENODEV;
  SystemTable *table = platform_firmware(); FileSystem *system = NULL;
  Status status = CALL(16,Status(UEFI_API *)(Handle,Guid *,void **))(device,&file_system,(void **)&system);
  File *directory = NULL;
  if (!status) status = system->open(system,&directory);
  const char *components[] = {"EFI","UEFIBrowser","Profile"};
  for (size_t index = 0; !status && index < 3; ++index) {
    Character name[32]; size_t length = strlen(components[index]);
    for (size_t character = 0; character <= length; ++character) name[character] = components[index][character];
    File *next = NULL;
    status = directory->open(directory,&next,name,FILE_CREATE|3,FILE_DIRECTORY);
    directory->close(directory); directory = next;
  }
  if (status) { if (directory) directory->close(directory); return -status_error(status); }
  profile = directory; return 0;
}
static int path_name(const char *path,Character name[512]) {
  if (!path || strncmp(path,"/profile/",9)) return -EACCES;
  path += 9;
  if (!*path || strlen(path) > 1024) return -ENAMETOOLONG;
  size_t offset = 0;
  const unsigned char *input = (const void *)path;
  while (*input) {
    if (offset >= 509) return -ENAMETOOLONG;
    uint32_t code = *input++;
    if (code >= 0x80) {
      unsigned extra; uint32_t minimum;
      if (code >= 0xc2 && code <= 0xdf) { extra=1; minimum=0x80; code &= 31; }
      else if (code >= 0xe0 && code <= 0xef) { extra=2; minimum=0x800; code &= 15; }
      else if (code >= 0xf0 && code <= 0xf4) { extra=3; minimum=0x10000; code &= 7; }
      else return -EILSEQ;
      for (unsigned index=0;index<extra;++index) {
        if ((*input & 0xc0) != 0x80) return -EILSEQ;
        code=(code<<6)|(*input++ & 63);
      }
      if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return -EILSEQ;
    }
    if (code < 32 || code == 127 || code == '\\' || code == ':' || code == '*' || code == '?' || code == '"' || code == '<' || code == '>' || code == '|') return -EINVAL;
    if (code == '/') code = '\\';
    if (code > 0xffff) { code-=0x10000; name[offset++]=0xd800+(code>>10); code=0xdc00+(code&1023); }
    name[offset++]=code;
  }
  name[offset]=0;
  size_t start=0;
  for (size_t index=0;index<=offset;++index) if (!name[index] || name[index]=='\\') {
    size_t length=index-start;
    if (!length || (length==1 && name[start]=='.') || (length==2 && name[start]=='.' && name[start+1]=='.') ||
        name[index-1]=='.' || name[index-1]==' ') return -EINVAL;
    start=index+1;
  }
  return 0;
}
static long open_file(unsigned owner,const char *path,int flags) {
  Character name[512]; int error=path_name(path,name); if (error) return error;
  if (flags & ~(O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC|O_APPEND|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW|O_LARGEFILE)) return -EOPNOTSUPP;
  if ((flags&O_ACCMODE)==3 || ((flags&(O_TRUNC|O_CREAT)) && (flags&O_ACCMODE)==O_RDONLY)) return -EINVAL;
  error=open_profile(); if (error) return error;
  int index; for(index=0;index<FILE_COUNT && descriptors[index].file;++index) {}
  if(index==FILE_COUNT) return -EMFILE;
  File *file=NULL;
  if ((flags&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL)) {
    Status status=profile->open(profile,&file,name,1,0);
    if (!status) { file->close(file); return -EEXIST; }
    if (status!=ERROR(14)) return -status_error(status);
  }
  uint64_t mode=(flags&O_ACCMODE)==O_RDONLY ? 1 : 3;
  if(flags&O_CREAT) mode|=FILE_CREATE;
  Status status=profile->open(profile,&file,name,mode,0);
  if(status) return -status_error(status);
  FileInfo info={0}; size_t size=sizeof(info);
  status=file->info(file,&file_info,&size,&info);
  if (!status && ((flags&O_DIRECTORY)!=0) != ((info.attributes&FILE_DIRECTORY)!=0)) {
    if ((flags&O_DIRECTORY) || (flags&O_ACCMODE)!=O_RDONLY) { file->close(file); return flags&O_DIRECTORY ? -ENOTDIR : -EISDIR; }
  }
  if(!status && flags&O_TRUNC) { info.length=0; status=file->set_info(file,&file_info,size,&info); }
  if(status) { file->close(file); return -status_error(status); }
  descriptors[index]=(Descriptor){file,owner,flags};
  return FILE_BASE+index;
}
long firmware_file_service(unsigned owner,long number,const long arguments[6]) {
  long first=arguments[0],second=arguments[1],third=arguments[2];
  if(number==SYS_open) return open_file(owner,(const char *)first,second);
  if(number==SYS_openat) {
    if(first!=AT_FDCWD) return -EOPNOTSUPP;
    return open_file(owner,(const char *)second,third);
  }
  if(number==SYS_unlink || number==SYS_mkdir) {
    Character name[512]; int error=path_name((const char *)first,name); if(error) return error;
    error=open_profile(); if(error) return error;
    File *file=NULL;
    if (number==SYS_mkdir) {
      Status exists=profile->open(profile,&file,name,1,0);
      if (!exists) { file->close(file); return -EEXIST; }
      if (exists!=ERROR(14)) return -status_error(exists);
    }
    Status status=profile->open(profile,&file,name,number==SYS_mkdir ? FILE_CREATE|3 : 3,number==SYS_mkdir ? FILE_DIRECTORY : 0);
    if(status) return -status_error(status);
    if(number==SYS_unlink) {
      FileInfo info={0};size_t size=sizeof(info); status=file->info(file,&file_info,&size,&info);
      if(status || (info.attributes&FILE_DIRECTORY)) { file->close(file); return status ? -status_error(status) : -EISDIR; }
      return -status_error(file->remove(file));
    }
    return -status_error(file->close(file));
  }
  Descriptor *entry=lookup(owner,first); if(!entry) return -EBADF;
  File *file=entry->file;
  if(number==SYS_close) { Status status=file->close(file); if(!status) memset(entry,0,sizeof(*entry)); return -status_error(status); }
  if(number==SYS_fcntl) {
    if(second==F_GETFL) return entry->flags;
    if(second==F_GETFD) return entry->flags&O_CLOEXEC ? FD_CLOEXEC : 0;
    if(second==F_SETFD) { entry->flags=(entry->flags&~O_CLOEXEC)|(third&FD_CLOEXEC ? O_CLOEXEC : 0); return 0; }
    return -EOPNOTSUPP;
  }
  if(number==SYS_read || number==SYS_write) {
    if(third<0 || (third && !second)) return -EINVAL;
    if(!third) return 0;
    if(number==SYS_write && (entry->flags&O_ACCMODE)==O_RDONLY) return -EBADF;
    if(number==SYS_read && (entry->flags&O_ACCMODE)==O_WRONLY) return -EBADF;
    if(number==SYS_write && entry->flags&O_APPEND) { Status status=file->seek(file,UINT64_MAX); if(status) return -status_error(status); }
    size_t size=third>1024*1024 ? 1024*1024 : third;
    Status status=number==SYS_read ? file->read(file,&size,(void *)second) : file->write(file,&size,(void *)second);
    return status ? -status_error(status) : (long)size;
  }
  if(number==SYS_fsync || number==SYS_fdatasync) return -status_error(file->flush(file));
  FileInfo info={0}; size_t size=sizeof(info);
  Status status=file->info(file,&file_info,&size,&info); if(status) return -status_error(status);
  if(number==SYS_fstat) {
    if(!second) return -EINVAL;
    struct stat *value=(void *)second; memset(value,0,sizeof(*value));
    value->st_mode=(info.attributes&FILE_DIRECTORY ? S_IFDIR|0700 : S_IFREG|0600);
    value->st_nlink=1; value->st_size=info.length; value->st_blksize=4096; value->st_blocks=(info.physical+511)/512;
    value->st_dev=1; value->st_ino=first;
    return 0;
  }
  if(number==SYS_ftruncate) {
    if(second<0) return -EINVAL;
    if((entry->flags&O_ACCMODE)==O_RDONLY) return -EBADF;
    info.length=second; return -status_error(file->set_info(file,&file_info,size,&info));
  }
  if(number==SYS_lseek) {
    uint64_t base=0;
    if(third==SEEK_CUR) status=file->position(file,&base);
    else if(third==SEEK_END) base=info.length;
    else if(third!=SEEK_SET) return -EINVAL;
    if(status) return -status_error(status);
    if(base>LONG_MAX || (second<0 && (uint64_t)(-(second+1))+1>base) || (second>0 && base>(uint64_t)LONG_MAX-second)) return -EINVAL;
    uint64_t offset=base+second;
    status=file->seek(file,offset); return status ? -status_error(status) : (long)offset;
  }
  return -EOPNOTSUPP;
}
long firmware_file_syscall(long number,long first,long second,long third,long fourth,long fifth,long sixth) {
  long arguments[6]={first,second,third,fourth,fifth,sixth};
  if(renderer_runtime_is_child()) return (long)renderer_runtime_request(RENDERER_FILE_REQUEST,number,(uintptr_t)arguments,0,0,0);
  return firmware_file_service(0,number,arguments);
}
void firmware_file_reset(unsigned owner) {
  for(int index=0;index<FILE_COUNT;++index) if(descriptors[index].file && descriptors[index].owner==owner) {
    descriptors[index].file->close(descriptors[index].file); memset(&descriptors[index],0,sizeof(descriptors[index]));
  }
  if(!owner && profile) { profile->close(profile); profile=NULL; }
}
