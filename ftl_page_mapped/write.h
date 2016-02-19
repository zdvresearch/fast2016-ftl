#ifndef WRITE_H
#define WRITE_H

void writeChunkOnLogBlockDuringGC( const UINT32 bank,
                                   const UINT32 dataLpn,
                                   const UINT32 dataChunkOffset,
                                   const UINT32 chunkOffsetInBuf,
                                   const UINT32 bufAddr);

void writeToLogBlk (LogCtrlBlock * ctrlBlock,
                    UINT32 const dataLpn,
                    UINT32 const sectOffset,
                    UINT32 const nSects);

void updateChunkPtr();
void updateChunkPtrRecycledPage();

UINT32 chooseNewBank(UINT32 lpn);

#endif
