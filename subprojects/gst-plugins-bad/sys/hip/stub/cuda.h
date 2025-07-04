/* CUDA stub header
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef gpointer CUcontext;
typedef gpointer CUgraphicsResource;
typedef gpointer CUstream;
typedef gpointer CUarray;
typedef gpointer CUmodule;
typedef gpointer CUfunction;
typedef gpointer CUmipmappedArray;
typedef gpointer CUevent;
typedef gpointer CUmemoryPool;
typedef gpointer CUexternalMemory;
typedef gpointer CUexternalSemaphore;

typedef guint64  CUtexObject;
typedef guintptr CUdeviceptr;
typedef gint CUdevice;

typedef enum
{
  CUDA_SUCCESS                              = 0,
  CUDA_ERROR_INVALID_VALUE                  = 1,
  CUDA_ERROR_OUT_OF_MEMORY                  = 2,
  CUDA_ERROR_NOT_INITIALIZED                = 3,
  CUDA_ERROR_DEINITIALIZED                  = 4,
  CUDA_ERROR_PROFILER_DISABLED              = 5,
  CUDA_ERROR_PROFILER_NOT_INITIALIZED       = 6,
  CUDA_ERROR_PROFILER_ALREADY_STARTED       = 7,
  CUDA_ERROR_PROFILER_ALREADY_STOPPED       = 8,
  CUDA_ERROR_STUB_LIBRARY                   = 34,
  CUDA_ERROR_DEVICE_UNAVAILABLE             = 46,
  CUDA_ERROR_NO_DEVICE                      = 100,
  CUDA_ERROR_INVALID_DEVICE                 = 101,
  CUDA_ERROR_DEVICE_NOT_LICENSED            = 102,
  CUDA_ERROR_INVALID_IMAGE                  = 200,
  CUDA_ERROR_INVALID_CONTEXT                = 201,
  CUDA_ERROR_CONTEXT_ALREADY_CURRENT        = 202,
  CUDA_ERROR_MAP_FAILED                     = 205,
  CUDA_ERROR_UNMAP_FAILED                   = 206,
  CUDA_ERROR_ARRAY_IS_MAPPED                = 207,
  CUDA_ERROR_ALREADY_MAPPED                 = 208,
  CUDA_ERROR_NO_BINARY_FOR_GPU              = 209,
  CUDA_ERROR_ALREADY_ACQUIRED               = 210,
  CUDA_ERROR_NOT_MAPPED                     = 211,
  CUDA_ERROR_NOT_MAPPED_AS_ARRAY            = 212,
  CUDA_ERROR_NOT_MAPPED_AS_POINTER          = 213,
  CUDA_ERROR_ECC_UNCORRECTABLE              = 214,
  CUDA_ERROR_UNSUPPORTED_LIMIT              = 215,
  CUDA_ERROR_CONTEXT_ALREADY_IN_USE         = 216,
  CUDA_ERROR_PEER_ACCESS_UNSUPPORTED        = 217,
  CUDA_ERROR_INVALID_PTX                    = 218,
  CUDA_ERROR_INVALID_GRAPHICS_CONTEXT       = 219,
  CUDA_ERROR_NVLINK_UNCORRECTABLE           = 220,
  CUDA_ERROR_JIT_COMPILER_NOT_FOUND         = 221,
  CUDA_ERROR_UNSUPPORTED_PTX_VERSION        = 222,
  CUDA_ERROR_JIT_COMPILATION_DISABLED       = 223,
  CUDA_ERROR_UNSUPPORTED_EXEC_AFFINITY      = 224,
  CUDA_ERROR_UNSUPPORTED_DEVSIDE_SYNC       = 225,
  CUDA_ERROR_INVALID_SOURCE                 = 300,
  CUDA_ERROR_FILE_NOT_FOUND                 = 301,
  CUDA_ERROR_SHARED_OBJECT_SYMBOL_NOT_FOUND = 302,
  CUDA_ERROR_SHARED_OBJECT_INIT_FAILED      = 303,
  CUDA_ERROR_OPERATING_SYSTEM               = 304,
  CUDA_ERROR_INVALID_HANDLE                 = 400,
  CUDA_ERROR_ILLEGAL_STATE                  = 401,
  CUDA_ERROR_LOSSY_QUERY                    = 402,
  CUDA_ERROR_NOT_FOUND                      = 500,
  CUDA_ERROR_NOT_READY                      = 600,
  CUDA_ERROR_ILLEGAL_ADDRESS                = 700,
  CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES        = 701,
  CUDA_ERROR_LAUNCH_TIMEOUT                 = 702,
  CUDA_ERROR_LAUNCH_INCOMPATIBLE_TEXTURING  = 703,
  CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED    = 704,
  CUDA_ERROR_PEER_ACCESS_NOT_ENABLED        = 705,
  CUDA_ERROR_PRIMARY_CONTEXT_ACTIVE         = 708,
  CUDA_ERROR_CONTEXT_IS_DESTROYED           = 709,
  CUDA_ERROR_ASSERT                         = 710,
  CUDA_ERROR_TOO_MANY_PEERS                 = 711,
  CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED = 712,
  CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED     = 713,
  CUDA_ERROR_HARDWARE_STACK_ERROR           = 714,
  CUDA_ERROR_ILLEGAL_INSTRUCTION            = 715,
  CUDA_ERROR_MISALIGNED_ADDRESS             = 716,
  CUDA_ERROR_INVALID_ADDRESS_SPACE          = 717,
  CUDA_ERROR_INVALID_PC                     = 718,
  CUDA_ERROR_LAUNCH_FAILED                  = 719,
  CUDA_ERROR_COOPERATIVE_LAUNCH_TOO_LARGE   = 720,
  CUDA_ERROR_NOT_PERMITTED                  = 800,
  CUDA_ERROR_NOT_SUPPORTED                  = 801,
  CUDA_ERROR_SYSTEM_NOT_READY               = 802,
  CUDA_ERROR_SYSTEM_DRIVER_MISMATCH         = 803,
  CUDA_ERROR_COMPAT_NOT_SUPPORTED_ON_DEVICE = 804,
  CUDA_ERROR_MPS_CONNECTION_FAILED          = 805,
  CUDA_ERROR_MPS_RPC_FAILURE                = 806,
  CUDA_ERROR_MPS_SERVER_NOT_READY           = 807,
  CUDA_ERROR_MPS_MAX_CLIENTS_REACHED        = 808,
  CUDA_ERROR_MPS_MAX_CONNECTIONS_REACHED    = 809,
  CUDA_ERROR_MPS_CLIENT_TERMINATED          = 810,
  CUDA_ERROR_CDP_NOT_SUPPORTED              = 811,
  CUDA_ERROR_CDP_VERSION_MISMATCH           = 812,
  CUDA_ERROR_STREAM_CAPTURE_UNSUPPORTED     = 900,
  CUDA_ERROR_STREAM_CAPTURE_INVALIDATED     = 901,
  CUDA_ERROR_STREAM_CAPTURE_MERGE           = 902,
  CUDA_ERROR_STREAM_CAPTURE_UNMATCHED       = 903,
  CUDA_ERROR_STREAM_CAPTURE_UNJOINED        = 904,
  CUDA_ERROR_STREAM_CAPTURE_ISOLATION       = 905,
  CUDA_ERROR_STREAM_CAPTURE_IMPLICIT        = 906,
  CUDA_ERROR_CAPTURED_EVENT                 = 907,
  CUDA_ERROR_STREAM_CAPTURE_WRONG_THREAD    = 908,
  CUDA_ERROR_TIMEOUT                        = 909,
  CUDA_ERROR_GRAPH_EXEC_UPDATE_FAILURE      = 910,
  CUDA_ERROR_EXTERNAL_DEVICE                = 911,
  CUDA_ERROR_INVALID_CLUSTER_SIZE           = 912,
  CUDA_ERROR_FUNCTION_NOT_LOADED            = 913,
  CUDA_ERROR_INVALID_RESOURCE_TYPE          = 914,
  CUDA_ERROR_INVALID_RESOURCE_CONFIGURATION = 915,
  CUDA_ERROR_UNKNOWN                        = 999
} CUresult;

typedef enum
{
  CU_MEMORYTYPE_HOST = 1,
  CU_MEMORYTYPE_DEVICE = 2,
  CU_MEMORYTYPE_ARRAY = 3,
  CU_MEMORYTYPE_UNIFIED = 4,
} CUmemorytype;

typedef enum
{
  CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT = 14,
  CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING = 41,
  CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75,
  CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76,
  CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED = 102,
  CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED = 103,
  CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_WIN32_HANDLE_SUPPORTED = 104,
  CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_WIN32_KMT_HANDLE_SUPPORTED = 105,
  CU_DEVICE_ATTRIBUTE_MEMORY_POOLS_SUPPORTED = 115,
} CUdevice_attribute;

typedef enum
{
  CU_GRAPHICS_REGISTER_FLAGS_NONE = 0x00,
  CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY = 0x01,
  CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD = 0x02,
  CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LOAD_STORE = 0x04,
  CU_GRAPHICS_REGISTER_FLAGS_TEXTURE_GATHER = 0x08,
} CUgraphicsRegisterFlags;

typedef enum
{
  CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE = 0x00,
  CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY = 0x01,
  CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD = 0x02,
} CUgraphicsMapResourceFlags;

typedef enum
{
  CU_STREAM_DEFAULT = 0x0,
  CU_STREAM_NON_BLOCKING = 0x1
} CUstream_flags;

typedef enum
{
  CU_TR_FILTER_MODE_POINT = 0,
  CU_TR_FILTER_MODE_LINEAR = 1
} CUfilter_mode;

typedef enum
{
  CU_TR_ADDRESS_MODE_WRAP = 0,
  CU_TR_ADDRESS_MODE_CLAMP = 1,
  CU_TR_ADDRESS_MODE_MIRROR = 2,
  CU_TR_ADDRESS_MODE_BORDER = 3
} CUaddress_mode;

typedef enum
{
  CU_RESOURCE_TYPE_ARRAY = 0,
  CU_RESOURCE_TYPE_MIPMAPPED_ARRAY = 1,
  CU_RESOURCE_TYPE_LINEAR = 2,
  CU_RESOURCE_TYPE_PITCH2D = 3
} CUresourcetype;

typedef enum
{
  CU_AD_FORMAT_UNSIGNED_INT8  = 1,
  CU_AD_FORMAT_UNSIGNED_INT16 = 2,
} CUarray_format;

typedef enum
{
  CU_RES_VIEW_FORMAT_NONE = 0,
} CUresourceViewFormat;

typedef enum
{
  CU_EVENT_DEFAULT = 0x0,
  CU_EVENT_BLOCKING_SYNC = 0x1,
  CU_EVENT_DISABLE_TIMING = 0x2,
  CU_EVENT_INTERPROCESS = 0x4,
} CUevent_flags;

typedef enum
{
  CU_LIMIT_STACK_SIZE = 0x0,
  CU_LIMIT_PRINTF_FIFO_SIZE = 0x1,
  CU_LIMIT_MALLOC_HEAP_SIZE = 0x2,
  CU_LIMIT_DEV_RUNTIME_SYNC_DEPTH = 0x3,
  CU_LIMIT_DEV_RUNTIME_PENDING_LAUNCH_COUNT = 0x4,
  CU_LIMIT_MAX_L2_FETCH_GRANULARITY = 0x5,
  CU_LIMIT_PERSISTING_L2_CACHE_SIZE = 0x6,
  CU_LIMIT_SHMEM_SIZE = 0x7,
  CU_LIMIT_CIG_ENABLED = 0x8,
  CU_LIMIT_CIG_SHMEM_FALLBACK_ENABLED = 0x9,
} CUlimit;

typedef struct
{
  gsize srcXInBytes;
  gsize srcY;
  CUmemorytype srcMemoryType;
  gconstpointer srcHost;
  CUdeviceptr srcDevice;
  CUarray srcArray;
  gsize srcPitch;

  gsize dstXInBytes;
  gsize dstY;
  CUmemorytype dstMemoryType;
  gpointer dstHost;
  CUdeviceptr dstDevice;
  CUarray dstArray;
  gsize dstPitch;

  gsize WidthInBytes;
  gsize Height;
} CUDA_MEMCPY2D;

typedef struct
{
  CUaddress_mode addressMode[3];
  CUfilter_mode filterMode;
  guint flags;
  guint maxAnisotropy;
  CUfilter_mode mipmapFilterMode;
  gfloat mipmapLevelBias;
  gfloat minMipmapLevelClamp;
  gfloat maxMipmapLevelClamp;
  gfloat borderColor[4];
  gint reserved[12];
} CUDA_TEXTURE_DESC;

typedef struct
{
  CUresourcetype resType;

  union {
    struct {
      CUarray hArray;
    } array;
    struct {
      CUmipmappedArray hMipmappedArray;
    } mipmap;
    struct {
      CUdeviceptr devPtr;
      CUarray_format format;
      guint numChannels;
      gsize sizeInBytes;
    } linear;
    struct {
      CUdeviceptr devPtr;
      CUarray_format format;
      guint numChannels;
      gsize width;
      gsize height;
      gsize pitchInBytes;
    } pitch2D;
    struct {
      gint reserved[32];
    } reserved;
  } res;

  guint flags;
} CUDA_RESOURCE_DESC;

typedef struct
{
  CUresourceViewFormat format;
  gsize width;
  gsize height;
  gsize depth;
  guint firstMipmapLevel;
  guint lastMipmapLevel;
  guint firstLayer;
  guint lastLayer;
  guint reserved[16];
} CUDA_RESOURCE_VIEW_DESC;

typedef enum
{
  CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS = 0x1
} CUipcMem_flags;

#define CU_IPC_HANDLE_SIZE 64
typedef struct
{
  char reserved[CU_IPC_HANDLE_SIZE];
} CUipcMemHandle;

typedef struct
{
  char reserved[CU_IPC_HANDLE_SIZE];
} CUipcEventHandle;

typedef unsigned long long CUmemGenericAllocationHandle;

typedef enum
{
  CU_MEM_HANDLE_TYPE_NONE = 0x0,
  CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR = 0x1,
  CU_MEM_HANDLE_TYPE_WIN32 = 0x2,
  CU_MEM_HANDLE_TYPE_WIN32_KMT = 0x4,
  CU_MEM_HANDLE_TYPE_MAX = 0x7FFFFFFF
} CUmemAllocationHandleType;

typedef enum
{
  CU_MEM_ACCESS_FLAGS_PROT_NONE = 0x0,
  CU_MEM_ACCESS_FLAGS_PROT_READ  = 0x1,
  CU_MEM_ACCESS_FLAGS_PROT_READWRITE = 0x3,
  CU_MEM_ACCESS_FLAGS_PROT_MAX = 0x7FFFFFFF
} CUmemAccess_flags;

typedef enum
{
  CU_MEM_LOCATION_TYPE_INVALID = 0x0,
  CU_MEM_LOCATION_TYPE_DEVICE = 0x1,
  CU_MEM_LOCATION_TYPE_MAX = 0x7FFFFFFF
} CUmemLocationType;

typedef enum CUmemAllocationType_enum {
  CU_MEM_ALLOCATION_TYPE_INVALID = 0x0,
  CU_MEM_ALLOCATION_TYPE_PINNED = 0x1,
  CU_MEM_ALLOCATION_TYPE_MAX = 0x7FFFFFFF
} CUmemAllocationType;

typedef enum
{
  CU_MEM_ALLOC_GRANULARITY_MINIMUM = 0x0,
  CU_MEM_ALLOC_GRANULARITY_RECOMMENDED = 0x1
} CUmemAllocationGranularity_flags;

typedef struct
{
  CUmemLocationType type;
  int id;
} CUmemLocation;

typedef struct
{
  unsigned char compressionType;
  unsigned char gpuDirectRDMACapable;
  unsigned short usage;
  unsigned char reserved[4];
} CUmemAllocationPropAllocFlags;

typedef struct
{
  CUmemAllocationType type;
  CUmemAllocationHandleType requestedHandleTypes;
  CUmemLocation location;
  void *win32HandleMetaData;
  CUmemAllocationPropAllocFlags allocFlags;
} CUmemAllocationProp;

typedef struct
{
  CUmemLocation location;
  CUmemAccess_flags flags;
} CUmemAccessDesc;

typedef struct
{
  CUmemAllocationType allocType;
  CUmemAllocationHandleType handleTypes;
  CUmemLocation location;
  void *win32SecurityAttributes;
  size_t maxSize;
  unsigned char reserved[56];
} CUmemPoolProps;

typedef enum
{
  CU_MEMPOOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES = 1,
  CU_MEMPOOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC,
  CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES,
  CU_MEMPOOL_ATTR_RELEASE_THRESHOLD,
  CU_MEMPOOL_ATTR_RESERVED_MEM_CURRENT,
  CU_MEMPOOL_ATTR_RESERVED_MEM_HIGH,
  CU_MEMPOOL_ATTR_USED_MEM_CURRENT,
  CU_MEMPOOL_ATTR_USED_MEM_HIGH,
} CUmemPool_attribute;

typedef struct
{
  unsigned long long offset;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_BUFFER_DESC;

typedef enum
{
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD = 1,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32 = 2,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT = 3,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP = 4,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE = 5,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE = 6,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE_KMT = 7,
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_NVSCIBUF = 8
} CUexternalMemoryHandleType;

/**
 * CUDA_EXTERNAL_MEMORY_HANDLE_DESC: (skip) (attributes doc.skip=true)
 */
typedef struct
{
  CUexternalMemoryHandleType type;
  union {
    int fd;
    struct {
      void *handle;
      const void *name;
    } win32;
    const void *nvSciBufObject;
  } handle;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_HANDLE_DESC;

typedef enum
{
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD = 1,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32 = 2,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT = 3,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE = 4,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE = 5,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_NVSCISYNC = 6,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX = 7,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX_KMT = 8,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD = 9,
  CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32 = 10
} CUexternalSemaphoreHandleType;

/**
 * CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC: (skip) (attributes doc.skip=true)
 */
typedef struct
{
  CUexternalSemaphoreHandleType type;
  union {
    int fd;
    struct {
      void *handle;
      const void *name;
    } win32;
    const void* nvSciSyncObj;
  } handle;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC;

/**
 * CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS: (skip) (attributes doc.skip=true)
 */
typedef struct
{
  struct {
    struct {
      unsigned long long value;
    } fence;
    union {
      void *fence;
      unsigned long long reserved;
    } nvSciSync;
    struct {
      unsigned long long key;
    } keyedMutex;
    unsigned int reserved[12];
  } params;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS;

/**
 * CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS: (skip) (attributes doc.skip=true)
 */
typedef struct
{
  struct {
    struct {
      unsigned long long value;
    } fence;
    union {
      void *fence;
      unsigned long long reserved;
    } nvSciSync;
    struct {
      unsigned long long key;
      unsigned int timeoutMs;
    } keyedMutex;
    unsigned int reserved[10];
  } params;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS;

typedef struct
{
  size_t Width;
  size_t Height;
  size_t Depth;
  CUarray_format Format;
  unsigned int NumChannels;
  unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;

typedef struct
{
  unsigned long long offset;
  CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
  unsigned int numLevels;
  unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC;

#define CUDA_VERSION 10000

#ifdef _WIN32
#define CUDAAPI __stdcall
#else
#define CUDAAPI
#endif

#define cuCtxCreate cuCtxCreate_v2
#define cuCtxDestroy cuCtxDestroy_v2
#define cuCtxPopCurrent cuCtxPopCurrent_v2
#define cuCtxPushCurrent cuCtxPushCurrent_v2
#define cuGraphicsResourceGetMappedPointer cuGraphicsResourceGetMappedPointer_v2
#define cuGraphicsResourceSetMapFlags cuGraphicsResourceSetMapFlags_v2

#define cuStreamDestroy cuStreamDestroy_v2

#define cuMemAlloc cuMemAlloc_v2
#define cuMemAllocPitch cuMemAllocPitch_v2
#define cuMemAllocHost  cuMemAllocHost_v2
#define cuMemcpy2D cuMemcpy2D_v2
#define cuMemcpy2DAsync cuMemcpy2DAsync_v2
#define cuMemcpyDtoD cuMemcpyDtoD_v2
#define cuMemcpyDtoDAsync cuMemcpyDtoDAsync_v2
#define cuMemcpyDtoH cuMemcpyDtoH_v2
#define cuMemcpyDtoHAsync cuMemcpyDtoHAsync_v2
#define cuMemcpyHtoD cuMemcpyHtoD_v2
#define cuMemcpyHtoDAsync cuMemcpyHtoDAsync_v2
#define cuMemFree cuMemFree_v2
#define cuMemsetD2D8 cuMemsetD2D8_v2
#define cuMemsetD2D16 cuMemsetD2D16_v2
#define cuMemsetD2D32 cuMemsetD2D32_v2

#define cuEventDestroy cuEventDestroy_v2

#define CU_TRSF_READ_AS_INTEGER 1

G_END_DECLS

