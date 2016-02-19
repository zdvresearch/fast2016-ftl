#ifndef LOG_H
#define LOG_H
#include "jasmine.h" // PAGES_PER_BLK
#include "ftl_parameters.h" // LOG_BLK_PER_BANK, ISOL_BLK_PER_BANK

#define get_log_lbn(log_lpn)                 ((log_lpn) / PAGES_PER_BLK)

#define getRWLpn(bank, ctrlBlock)           (ctrlBlock[bank].logLpn)

// Public Functions
void set_log_vbn (UINT32 const bank, UINT32 const log_lbn, UINT32 const vblock);
UINT32 get_log_vbn (UINT32 const bank, UINT32 const log_lbn);
void initLog();
chunkLocation findChunkLocation(const UINT32 chunkAddr);
UINT32 getLpnForCompletePage(const UINT32 bank, LogCtrlBlock * ctrlBlock);
void precacheLowPage(const UINT32 bank, LogCtrlBlock * ctrlBlock);

#endif
