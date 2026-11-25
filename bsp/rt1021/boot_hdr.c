// NXP i.MX RT1021 FlexSPI NOR XIP Boot Headers
// Places:
//   - FlexSPI Configuration Block (FCB) at 0x60000400 (.boot_hdr.conf)
//   - Image Vector Table (IVT)          at 0x60001000 (.boot_hdr.ivt)
//   - Boot Data                         at 0x60001020 (.boot_hdr.boot_data)

#include <stdint.h>

extern const uint32_t g_pfnVectors[];

// ---------------------------------------------------------------------------
// 1. FlexSPI NOR Configuration Block (512 bytes at 0x60000400)
//    Configured for MIMXRT1020-EVK 8 MB QSPI NOR (IS25LP064A / standard QSPI)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t tag;                   // 0x000: "FCFB" (0x42464346)
    uint32_t version;               // 0x004: 0x56010400 (V1.4.0)
    uint32_t reserved0;             // 0x008
    uint8_t  readSampleClkSrc;      // 0x00C: 0 = internal loopback, 1 = loopback from DQS pad
    uint8_t  csHoldTime;            // 0x00D: 3
    uint8_t  csSetupTime;           // 0x00E: 3
    uint8_t  columnAddressWidth;    // 0x00F: 0 (NOR Flash)
    uint8_t  deviceModeCfgEnable;   // 0x010: 0
    uint8_t  deviceModeType;        // 0x011: 0
    uint16_t waitTimeCfgCommands;   // 0x012: 0
    uint32_t deviceModeSeq;         // 0x014: 0
    uint32_t deviceModeArg;         // 0x018: 0
    uint8_t  configCmdEnable;       // 0x01C: 0
    uint8_t  configModeType[3];     // 0x01D
    uint32_t configCmdSeqs[3];      // 0x020
    uint32_t reserved1;             // 0x02C
    uint32_t configCmdArgs[3];      // 0x030
    uint32_t reserved2;             // 0x03C
    uint32_t controllerMiscOption;  // 0x040
    uint8_t  deviceType;            // 0x044: 1 = Serial NOR
    uint8_t  sflashPadType;         // 0x045: 4 = Quad Pads
    uint8_t  serialClkFreq;         // 0x046: 3 = 60 MHz (safe across QSPI NOR variants)
    uint8_t  lutCustomSeqEnable;    // 0x047: 0
    uint32_t reserved3[2];          // 0x048
    uint32_t sflashA1Size;          // 0x050: 8 MB (0x00800000)
    uint32_t sflashA2Size;          // 0x054: 0
    uint32_t sflashB1Size;          // 0x058: 0
    uint32_t sflashB2Size;          // 0x05C: 0
    uint32_t csPadSettingOverride;  // 0x060
    uint32_t sclkPadSettingOverride;// 0x064
    uint32_t dataPadSettingOverride;// 0x068
    uint32_t dqsPadSettingOverride; // 0x06C
    uint32_t timeoutInMs;           // 0x070
    uint32_t commandInterval;       // 0x074
    uint16_t dataValidTime[2];      // 0x078
    uint16_t busyOffset;            // 0x07C
    uint16_t busyBitPolarity;       // 0x07E
    uint32_t lookupTable[64];       // 0x080..0x17F (FlexSPI LUT sequences)
    uint32_t lutCustomSeq[12];      // 0x180..0x1AF
    uint32_t reserved4[4];          // 0x1B0..0x1BF
    // Serial NOR specific fields (0x1C0..0x1FF)
    uint32_t pageSize;              // 0x1C0: 256 bytes
    uint32_t sectorSize;            // 0x1C4: 4096 bytes
    uint8_t  ipcmdSerialClkFreq;    // 0x1C8: 1 (30 MHz)
    uint8_t  isUniformBlockSize;    // 0x1C9: 0
    uint8_t  reserved5[2];          // 0x1CA
    uint8_t  serialNorType;         // 0x1CC: 0 (XPI)
    uint8_t  needExitNoCmdMode;     // 0x1CD: 0
    uint8_t  halfClkForNonReadCmd;  // 0x1CE: 0
    uint8_t  needRestoreNoCmdMode;  // 0x1CF: 0
    uint32_t blockSize;             // 0x1D0: 64 KB
    uint32_t reserved6[11];         // 0x1D4..0x1FF
} rt1021_flexspi_nor_config_t;

// FlexSPI LUT instruction encoding macro
#define FLEXSPI_LUT_SEQ(cmd0, pad0, op0, cmd1, pad1, op1) \
    (uint32_t)((((uint32_t)(cmd0) & 0x3FU) << 10) | (((uint32_t)(pad0) & 0x3U) << 8) | ((uint32_t)(op0) & 0xFFU) | \
               (((uint32_t)(cmd1) & 0x3FU) << 26) | (((uint32_t)(pad1) & 0x3U) << 24) | (((uint32_t)(op1) & 0xFFU) << 16))

#define CMD_SDR         0x01U
#define RADDR_SDR       0x02U
#define DUMMY_SDR       0x0CU
#define READ_SDR        0x09U
#define STOP_EXE        0x00U
#define PADS_1          0x00U
#define PADS_4          0x02U

__attribute__((section(".boot_hdr.conf"), used))
const rt1021_flexspi_nor_config_t g_flexspi_nor_config = {
    .tag                = 0x42464346UL, // "FCFB"
    .version            = 0x56010400UL, // V1.4.0
    .readSampleClkSrc   = 1U,           // Loopback from DQS pad (internal dummy)
    .csHoldTime         = 3U,
    .csSetupTime        = 3U,
    .deviceType         = 1U,           // Serial NOR
    .sflashPadType      = 4U,           // QuadSPI
    .serialClkFreq      = 3U,           // 60 MHz
    .sflashA1Size       = 8UL * 1024UL * 1024UL, // 8 MB QSPI NOR
    .lookupTable = {
        // Sequence 0: Fast Read Quad I/O (0xEB, 24-bit addr, 6 dummy cycles)
        [0] = FLEXSPI_LUT_SEQ(CMD_SDR,   PADS_1, 0xEB, RADDR_SDR, PADS_4, 0x18),
        [1] = FLEXSPI_LUT_SEQ(DUMMY_SDR, PADS_4, 0x06, READ_SDR,  PADS_4, 0x04),
        [2] = FLEXSPI_LUT_SEQ(STOP_EXE,  PADS_1, 0x00, STOP_EXE,  PADS_1, 0x00),
    },
    .pageSize           = 256UL,
    .sectorSize         = 4096UL,
    .ipcmdSerialClkFreq = 1U,
    .blockSize          = 64UL * 1024UL,
};

// ---------------------------------------------------------------------------
// 2. Image Vector Table (IVT @ 0x60001000) & Boot Data (@ 0x60001020)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t start;
    uint32_t size;
    uint32_t plugin;
} rt1021_boot_data_t;

typedef struct {
    uint32_t hdr;
    uint32_t entry;
    uint32_t reserved1;
    uint32_t dcd;
    uint32_t boot_data;
    uint32_t self;
    uint32_t csf;
    uint32_t reserved2;
} rt1021_ivt_t;

__attribute__((section(".boot_hdr.boot_data"), used))
const rt1021_boot_data_t g_boot_data = {
    .start  = 0x60000000UL,
    .size   = 8UL * 1024UL * 1024UL,
    .plugin = 0UL,
};

__attribute__((section(".boot_hdr.ivt"), used))
const rt1021_ivt_t g_ivt = {
    .hdr       = 0x402000D1UL,              // Tag=0xD1, Len=32 (0x0020 BE), Ver=0x40
    .entry     = (uint32_t)(uintptr_t)g_pfnVectors, // XIP vector table at 0x60002000
    .reserved1 = 0UL,
    .dcd       = 0UL,
    .boot_data = (uint32_t)(uintptr_t)&g_boot_data,
    .self      = (uint32_t)(uintptr_t)&g_ivt,
    .csf       = 0UL,
    .reserved2 = 0UL,
};
