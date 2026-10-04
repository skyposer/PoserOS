#include "ff.h"
#include "diskio.h"

#define FS_LBA_OFFSET  (64u * 1024u * 1024u / 512u)

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
        return RES_PARERR;
    if (!count)
        return RES_PARERR;

    while (count--) {
        hdk_diskReadSector((u32)sector + FS_LBA_OFFSET, buff);
        sector++;
        buff += 512;
    }

    return RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
        return RES_PARERR;
    if (!count)
        return RES_PARERR;

    while (count--) {
        hdk_diskWriteSector((u32)sector + FS_LBA_OFFSET, buff);
        sector++;
        buff += 512;
    }

    return RES_OK;
}

#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
        return RES_PARERR;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT:
        *(DWORD *)buff = diskLbaNum;
        return RES_OK;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}