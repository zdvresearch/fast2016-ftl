#include "ftl_metadata.h"

//----------------------------------
// FTL metadata (maintain in SRAM)
//----------------------------------
//misc_metadata g_misc_meta[NUM_BANKS];
//ftl_statistics g_ftl_statistics[NUM_BANKS];
// volatile metadata
//UINT32 g_bad_blk_count[NUM_BANKS];
UINT32 g_bsp_isr_flag[NUM_BANKS];

//BOOL32 g_gc_flag[NUM_BANKS];
UINT8 g_mem_to_clr[PAGES_PER_BLK / 8];
UINT8 g_mem_to_set[PAGES_PER_BLK / 8];

// SATA read/write buffer pointer id
UINT32 g_ftl_read_buf_id;
UINT32 g_ftl_write_buf_id;

//UINT32 chunkPtr[NUM_BANKS];
//logBufMetaT logBufMeta[NUM_BANKS];

heapData heapDataFirstUsage;
heapData heapDataSecondUsage;
heapData heapDataCold;

listData cleanListDataWrite;

UINT32 userSecWrites = 0;
UINT32 totSecWrites = 0;

LogCtrlBlock coldLogCtrl[NUM_BANKS];
LogCtrlBlock hotLogCtrl[NUM_BANKS];

UINT32 free_list_head[NUM_BANKS];
UINT32 free_list_tail[NUM_BANKS];


BOOL8 gcState[NUM_BANKS];
UINT32 victimLbn[NUM_BANKS];

/*******************************
 * Configuration parameters
 ******************************/
//UINT32 hotFirstAccumulated[NUM_BANKS] = {56,56,56,56,56,56,56,56,56,56,56,56,56,56,56,56}; // half when 32GB 28 OP
//UINT32 hotFirstAccumulated[NUM_BANKS] = {28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28}; // quarter when 32GB 28 OP
//UINT32 hotFirstAccumulated[NUM_BANKS] = {17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17}; // half when 32GB 7 OP
UINT32 hotFirstAccumulated[NUM_BANKS] = {8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8}; // quarter when 32GB 7 OP
UINT32 cleanBlksAfterGcHot=2;
UINT32 secondHotFactorNum = 1;
UINT32 secondHotFactorDen = 1;
UINT32 lbaHotThreshold = INVALID;
UINT32 nSectsHotThreshold = 128;
UINT32 CleanBlksBackgroundGcThreshold = 1;
UINT32 nValidChunksInPageToReuseThreshold = 0;
#if WOMCanFail
float successRateWOM = 100.0;
#endif
UINT32 adaptiveWindowSize = 5;
UINT32 adaptiveWindow[NUM_BANKS][5];
UINT32 adaptiveStepUp[NUM_BANKS];
UINT32 adaptiveStepDown[NUM_BANKS];
UINT32 nStepUps[NUM_BANKS];
UINT32 nStepDowns[NUM_BANKS];
UINT32 maxStepUps = 7;
UINT32 maxStepDowns = 7;
UINT32 initStepUp = 1;
UINT32 initStepDown = 1;
