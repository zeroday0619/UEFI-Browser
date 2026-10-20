/* Firmware receives broker-owned copies so renderer mutation cannot redirect its writes. */
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include "firmware_io.h"

static int broker_arguments(uint64_t address, long output[6]) {
  if (!renderer_runtime_buffer(address,6*sizeof(long),0)) return 0;
  memcpy(output,(const void*)address,6*sizeof(long));
  return 1;
}
static long broker_transfer(unsigned socket, long number, long arguments[6]) {
  size_t size=(size_t)arguments[2];
  if (arguments[2]<0) return -EINVAL;
  if (size>1024*1024) size=1024*1024;
  unsigned reading=number==SYS_read || number==SYS_recvfrom;
  uint64_t destination=arguments[1];
  if (!renderer_runtime_buffer(destination,size,reading)) return -EFAULT;
  void *buffer=malloc(size ? size : 1);
  if (!buffer) return -ENOMEM;
  if (!reading && size) memcpy(buffer,(const void*)destination,size);
  arguments[1]=(long)buffer; arguments[2]=size;
  long result=socket ? firmware_socket_service(1,number,arguments) :
                       firmware_file_service(1,number,arguments);
  if (reading && result>0 && (size_t)result<=size)
    memcpy((void*)destination,buffer,result);
  free(buffer);
  return result;
}
static long broker_file(long number, uint64_t address) {
  long arguments[6];
  if (!broker_arguments(address,arguments)) return -EFAULT;
  switch (number) {
    case SYS_open: case SYS_openat: case SYS_unlink: case SYS_mkdir: {
      unsigned index=number==SYS_openat ? 1 : 0;
      char path[1034];
      if (!renderer_runtime_string(path,sizeof(path),arguments[index])) return -EFAULT;
      arguments[index]=(long)path;
      return firmware_file_service(1,number,arguments);
    }
    case SYS_read: case SYS_write: return broker_transfer(0,number,arguments);
    case SYS_fstat: {
      uint64_t destination=arguments[1];
      if (!renderer_runtime_buffer(destination,sizeof(struct stat),1)) return -EFAULT;
      struct stat value={0}; arguments[1]=(long)&value;
      long result=firmware_file_service(1,number,arguments);
      if (!result) memcpy((void*)destination,&value,sizeof(value));
      return result;
    }
    case SYS_close: case SYS_fcntl: case SYS_lseek: case SYS_fsync:
    case SYS_fdatasync: case SYS_ftruncate:
      return firmware_file_service(1,number,arguments);
    default: return -EOPNOTSUPP;
  }
}
static long broker_socket(long number, uint64_t address) {
  long arguments[6];
  if (!broker_arguments(address,arguments)) return -EFAULT;
  switch (number) {
    case SYS_socket: case SYS_close: case SYS_fcntl:
      return firmware_socket_service(1,number,arguments);
    case SYS_read: case SYS_write: case SYS_sendto: case SYS_recvfrom:
      return broker_transfer(1,number,arguments);
    case SYS_connect: {
      struct sockaddr_in destination;
      if (arguments[2]!=(long)sizeof(destination)) return -EINVAL;
      if (!renderer_runtime_buffer(arguments[1],sizeof(destination),0)) return -EFAULT;
      memcpy(&destination,(const void*)arguments[1],sizeof(destination));
      arguments[1]=(long)&destination;
      return firmware_socket_service(1,number,arguments);
    }
    case SYS_poll: {
      struct pollfd items[64];
      if (arguments[1]<0 || arguments[1]>64) return -EINVAL;
      size_t size=(size_t)arguments[1]*sizeof(items[0]);
      uint64_t destination=arguments[0];
      if (!renderer_runtime_buffer(destination,size,1)) return -EFAULT;
      if (size) memcpy(items,(const void*)destination,size);
      arguments[0]=(long)items;
      long result=firmware_socket_service(1,number,arguments);
      if (result>=0 && size) memcpy((void*)destination,items,size);
      return result;
    }
    case SYS_getsockopt: case SYS_getpeername: case SYS_getsockname: {
      unsigned index=number==SYS_getsockopt ? 3 : 1;
      uint64_t destination=arguments[index], length_address=arguments[index+1];
      socklen_t length;
      if (!renderer_runtime_buffer(length_address,sizeof(length),1)) return -EFAULT;
      memcpy(&length,(const void*)length_address,sizeof(length));
      unsigned char buffer[sizeof(struct sockaddr_in)]={0};
      size_t capacity=number==SYS_getsockopt ? sizeof(int) : sizeof(buffer);
      if (length<capacity) capacity=length;
      if (!renderer_runtime_buffer(destination,capacity,1)) return -EFAULT;
      length=capacity;
      arguments[index]=(long)buffer; arguments[index+1]=(long)&length;
      long result=firmware_socket_service(1,number,arguments);
      if (!result) {
        if (length<capacity) capacity=length;
        if (capacity) memcpy((void*)destination,buffer,capacity);
        memcpy((void*)length_address,&length,sizeof(length));
      }
      return result;
    }
    default: return -EOPNOTSUPP;
  }
}
static long broker_dns(unsigned operation, long handle, uint64_t address) {
  if (!operation) {
    char hostname[254];
    if (!renderer_runtime_string(hostname,sizeof(hostname),address)) return -EFAULT;
    return firmware_dns_service(1,operation,handle,hostname);
  }
  if (operation==2) return firmware_dns_service(1,operation,handle,NULL);
  if (operation!=1) return -EINVAL;
  unsigned char output[4];
  if (!renderer_runtime_buffer(address,sizeof(output),1)) return -EFAULT;
  long result=firmware_dns_service(1,operation,handle,output);
  if (result==1) memcpy((void*)address,output,sizeof(output));
  return result;
}
static long broker_audio(const uint64_t arguments[5]) {
  if (arguments[0]<AUDIO_CREATE || arguments[0]>AUDIO_CLOSE) return -EINVAL;
  if (arguments[0]!=AUDIO_UPLOAD)
    return firmware_audio_service(1,arguments[0],arguments[1],arguments[2],arguments[3],arguments[4]);
  unsigned char buffer[16384];
  if (arguments[4]>sizeof(buffer)) return -EINVAL;
  if (!renderer_runtime_buffer(arguments[3],arguments[4],0)) return -EFAULT;
  if (arguments[4]) memcpy(buffer,(const void*)arguments[3],arguments[4]);
  return firmware_audio_service(1,AUDIO_UPLOAD,arguments[1],arguments[2],(uintptr_t)buffer,arguments[4]);
}
static Status broker_variable(unsigned operation, const uint64_t arguments[5]) {
  Character name[64];
  Guid identifier;
  if (!renderer_runtime_buffer(arguments[1],sizeof(identifier),0)) return ERROR(15);
  memcpy(&identifier,(const void*)arguments[1],sizeof(identifier));
  size_t length;
  for (length=0; length<64; ++length) {
    if (arguments[0]>UINT64_MAX-length*2 ||
        !renderer_runtime_buffer(arguments[0]+length*2,2,0)) return ERROR(15);
    memcpy(&name[length],(const void*)(arguments[0]+length*2),2);
    if (!name[length]) break;
  }
  if (length==64) return ERROR(15);
  static const Guid settings={0x6156d542,0x0a46,0x4ad7,{0x9a,0x16,0xb2,0xc0,0x39,0x68,0xa1,0x4e}};
  static const Guid trust={0xfd2340d0,0x3dab,0x4349,{0xa6,0xc7,0x3b,0x4f,0x12,0xb4,0x8e,0xae}};
  const char *expected=!memcmp(&identifier,&settings,sizeof(identifier)) ? "UefiBrowserSettings" :
      !memcmp(&identifier,&trust,sizeof(identifier)) ? "TlsCaCertificate" : "";
  if (strlen(expected)!=length || !length) return ERROR(15);
  for (size_t index=0; index<length; ++index) if (name[index]!=(unsigned char)expected[index]) return ERROR(15);
  void **services=(void**)((unsigned char*)firmware->runtime+sizeof(TableHeader));
  if (operation==SET_VARIABLE) {
    uint32_t value[6];
    if (expected[0]!='U' || arguments[2]!=7 || arguments[3]!=sizeof(value) ||
        !renderer_runtime_buffer(arguments[4],sizeof(value),0)) return ERROR(15);
    memcpy(value,(const void*)arguments[4],sizeof(value));
    if (value[0]!=1 || value[1]>1 || (value[2]!=14 && value[2]!=16 && value[2]!=20) ||
        value[3]>1 || value[4]>1 || value[5]>1) return ERROR(2);
    return ((Status(UEFI_API *)(Character*,Guid*,uint32_t,size_t,void*))services[8])(
        name,&identifier,7,sizeof(value),value);
  }
  size_t size;
  if (!renderer_runtime_buffer(arguments[3],sizeof(size),1) ||
      (arguments[2] && !renderer_runtime_buffer(arguments[2],sizeof(uint32_t),1))) return ERROR(15);
  memcpy(&size,(const void*)arguments[3],sizeof(size));
  if (size>2*1024*1024 || !renderer_runtime_buffer(arguments[4],size,1)) return ERROR(15);
  size_t capacity=size;
  void *buffer=size ? malloc(size) : NULL;
  if (size && !buffer) return ERROR(9);
  uint32_t attributes=0;
  Status result=((Status(UEFI_API *)(Character*,Guid*,uint32_t*,size_t*,void*))services[6])(
      name,&identifier,&attributes,&size,buffer);
  if (!result && size<=capacity && size) memcpy((void*)arguments[4],buffer,size);
  memcpy((void*)arguments[3],&size,sizeof(size));
  if (!result && arguments[2]) memcpy((void*)arguments[2],&attributes,sizeof(attributes));
  free(buffer);
  return result;
}
