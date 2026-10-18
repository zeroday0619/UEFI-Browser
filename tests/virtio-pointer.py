#!/usr/bin/env python3
"""Compile the patched driver's report queue with host protocol stubs."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project.parents[1] / 'work/edk2-wheel/OvmfPkg/VirtioInputDxe/VirtioMouse.c').read_text()
def function(name):
    start = source.index(name + ' (')
    start = source.rfind('\n', 0, start - 1) + 1
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define IN
#define OUT
#define VOID void
#define EFIAPI
#define STATIC static
#define BOOLEAN uint8_t
#define INT32 int32_t
#define INT64 int64_t
#define EFI_STATUS int
#define EFI_TPL int
#define TRUE 1
#define FALSE 0
#define TPL_NOTIFY 16
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_INVALID_PARAMETER 2
#define MAX_INT32 INT32_MAX
#define MIN_INT32 INT32_MIN
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ASSERT assert
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CopyMem memcpy
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define BTN_LEFT 272
#define BTN_RIGHT 273
#define KEY_PRESSED 1
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
typedef struct {int32_t RelativeMovementX,RelativeMovementY,RelativeMovementZ;uint8_t LeftButton,RightButton;} EFI_SIMPLE_POINTER_STATE;
typedef struct {int unused;} EFI_SIMPLE_POINTER_PROTOCOL;
typedef struct {uint16_t Type,Code;uint32_t Value;} VIRTIO_INPUT_EVENT;
typedef struct {EFI_SIMPLE_POINTER_PROTOCOL SimplePointer;EFI_SIMPLE_POINTER_STATE PointerState;uint8_t PointerReady,PointerPending;size_t PointerHead,PointerCount;EFI_SIMPLE_POINTER_STATE PointerReports[256];} VIRTIO_INPUT_DEV;
#define VIRTIO_INPUT_FROM_POINTER_THIS(p) ((VIRTIO_INPUT_DEV *)(p))
static int raise_tpl(int value){return value;}
static void restore_tpl(int value){(void)value;}
static struct {int (*RaiseTPL)(int);void (*RestoreTPL)(int);} services={raise_tpl,restore_tpl},*gBS=&services;
'''
# EFIAPI precedes GetState, so its extracted declaration needs the return type.
code = prefix + function('VirtioMouseHandleEvent') + '\nEFI_STATUS\n' + function('VirtioMouseGetState') + r'''
static void event(VIRTIO_INPUT_DEV *device,int type,int code,int value){VIRTIO_INPUT_EVENT sample={type,code,(uint32_t)value};VirtioMouseHandleEvent(device,&sample);}
int main(void){
 VIRTIO_INPUT_DEV device={0}; EFI_SIMPLE_POINTER_STATE state;
 for(int index=0;index<128;index++){
  event(&device,EV_KEY,BTN_LEFT,1);event(&device,EV_SYN,0,0);
  event(&device,EV_KEY,BTN_LEFT,0);event(&device,EV_SYN,0,0);
 }
 assert(device.PointerCount==256);
 for(int index=0;index<256;index++){
  assert(VirtioMouseGetState(&device.SimplePointer,&state)==EFI_SUCCESS);
  assert(state.LeftButton==(index%2==0));
 }
 assert(VirtioMouseGetState(&device.SimplePointer,&state)==EFI_NOT_READY);
 event(&device,EV_REL,REL_X,17);event(&device,EV_REL,REL_Y,-4);event(&device,EV_REL,REL_WHEEL,2);event(&device,EV_SYN,0,0);
 assert(VirtioMouseGetState(&device.SimplePointer,&state)==EFI_SUCCESS);
 assert(state.RelativeMovementX==17 && state.RelativeMovementY==-4 && state.RelativeMovementZ==2);
 event(&device,EV_REL,REL_WHEEL,INT32_MAX);event(&device,EV_REL,REL_WHEEL,1);event(&device,EV_SYN,0,0);
 assert(VirtioMouseGetState(&device.SimplePointer,&state)==EFI_SUCCESS && state.RelativeMovementZ==INT32_MAX);
 assert(!device.PointerReady);
 puts("PASS: 128 queued clicks retain every edge; motion, wheel, wraparound and saturation remain correct");
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'pointer.c'
    binary = Path(directory) / 'pointer'
    test.write_text(code)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
