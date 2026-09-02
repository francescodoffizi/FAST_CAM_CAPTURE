#pragma once
#include <stdint.h>

// Qualcomm QCarCam / AIS client API definitions for SA8155P (DiLink 5.0)

typedef void*  qcarcam_hndl_t;
typedef int    qcarcam_ret_t;

#define QCARCAM_RET_OK        0
#define QCARCAM_RET_FAILED    1
#define QCARCAM_RET_BADSTATE  3
#define QCARCAM_RET_TIMEOUT   6   // Confirmed from qcarcam_test disassembly

typedef enum {
    QCARCAM_FMT_UYVY_8  = 0x7080102, // SA8155P DiLink 5.0 UYVY format code
    QCARCAM_FMT_NV12    = 0x21,
} qcarcam_color_fmt_t;

// 80-byte buffer descriptor matching ais_s_buffers
typedef struct {
    uint32_t width;            // +0:  frame width (1920)
    uint32_t height;           // +4:  frame height (1300)
    uint32_t stride;           // +8:  row stride in bytes = (width*2 + 63) & ~63
    uint32_t size;             // +12: buffer size in bytes = stride * height
    int64_t  fd;               // +16: ION fd as int64
    void*    pVirtAddr;        // +24: virtual address (mmap of ION fd)
    uint64_t pPhysAddr;        // +32: physical address (0)
    uint32_t flags;            // +40: buffer flags (0)
    uint32_t buf_type;         // +44: 1 = regular buffer
    uint32_t _reserved[8];     // +48: padding to 80 bytes
} qcarcam_buffer_t;

// Layout for qcarcam_s_buffers
typedef struct {
    qcarcam_color_fmt_t  color_fmt;  // +0:  0x10 (UYVY)
    uint32_t             flags;      // +4:  0
    qcarcam_buffer_t*    pBuffers;   // +8:  pointer to buffer array
    uint32_t             n_buffers;  // +16: number of buffers (5)
} qcarcam_buffers_t;

// Frame metadata from qcarcam_get_frame (exact 48 bytes matching ais_get_frame)
typedef struct {
    uint32_t buf_index;        // +0:  buffer index
    uint32_t sequence_no;      // +4:  frame sequence number
    uint64_t timestamp_ns;     // +8:  timestamp in ns
    uint32_t flags;            // +16: flags
    uint32_t field_type;       // +20: field type
    uint8_t  _reserved[24];    // +24: padding to 48 bytes
} qcarcam_frame_info_t;

typedef void* (*pfn_qcarcam_initialize)(void*);
typedef int   (*pfn_qcarcam_uninitialize)(void);
typedef void* (*pfn_qcarcam_open)(uint32_t input_id);
typedef int   (*pfn_qcarcam_close)(void* hndl);
typedef int   (*pfn_qcarcam_s_buffers)(void* hndl, qcarcam_buffers_t* p_bufs);
typedef int   (*pfn_qcarcam_s_param)(void* hndl, int param_id, void* p_value);
typedef int   (*pfn_qcarcam_start)(void* hndl);
typedef int   (*pfn_qcarcam_stop)(void* hndl);
typedef int   (*pfn_qcarcam_get_frame)(void* hndl, qcarcam_frame_info_t* p_info, uint64_t timeout_ns, uint32_t flags);
typedef int   (*pfn_qcarcam_release_frame)(void* hndl, uint32_t buf_idx);
