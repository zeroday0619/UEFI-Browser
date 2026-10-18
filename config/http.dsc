[Defines]
  PLATFORM_NAME = UefiBrowserHttp
  PLATFORM_GUID = 8d88c833-22cf-47b6-b7ad-3bc54ce67b64
  PLATFORM_VERSION = 1.0
  DSC_SPECIFICATION = 0x00010005
  OUTPUT_DIRECTORY = Build/UefiBrowserHttp
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS = RELEASE
  SKUID_IDENTIFIER = DEFAULT

[LibraryClasses]
  StackCheckLib|MdePkg/Library/StackCheckLib/StackCheckLib.inf
  StackCheckFailureHookLib|MdePkg/Library/StackCheckFailureHookLibNull/StackCheckFailureHookLibNull.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLibDevicePathProtocol/UefiDevicePathLibDevicePathProtocol.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  VirtioLib|OvmfPkg/Library/VirtioLib/VirtioLib.inf

  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  HttpLib|NetworkPkg/Library/DxeHttpLib/DxeHttpLib.inf
  DpcLib|NetworkPkg/Library/DxeDpcLib/DxeDpcLib.inf

[PcdsFixedAtBuild]
  gEfiNetworkPkgTokenSpaceGuid.PcdAllowHttpConnections|TRUE

[Components]
  NetworkPkg/HttpDxe/HttpDxe.inf
