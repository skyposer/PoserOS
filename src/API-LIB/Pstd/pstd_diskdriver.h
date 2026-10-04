#ifndef PSTD_DISKDRIVER_H
#define PSTD_DISKDRIVER_H

#include "pstd_int.h"
#include "pstd_port.h"

#define ATA_DATA    0x1F0
#define ATA_ERROR   0x1F1
#define ATA_FEATURE 0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_COMMAND 0x1F7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DSC  0x10
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

u32 diskLbaNum = 0;

static void init_disk(){
    diskLbaNum = *((u32*)0x8008);
    
}

static void hdk_waitBsy(void)
{
    while (readp8(ATA_STATUS) & ATA_SR_BSY)
        ;
}

static void hdk_waitDrq(void)
{
    while (!(readp8(ATA_STATUS) & ATA_SR_DRQ))
        ;
}

static void hdk_diskReadSector(u32 lba, u8 *buf)
{
    u32 i;

    hdk_waitBsy();

    writep8(ATA_DRIVE,   0xE0 | ((lba >> 24) & 0x0F));
    writep8(ATA_SECCNT,  1);
    writep8(ATA_LBA0,    (u8)(lba));
    writep8(ATA_LBA1,    (u8)(lba >> 8));
    writep8(ATA_LBA2,    (u8)(lba >> 16));
    writep8(ATA_COMMAND, 0x20);

    hdk_waitBsy();
    hdk_waitDrq();

    for (i = 0; i < 256; i++) {
        u16 w = readp16(ATA_DATA);
        buf[i * 2]     = (u8)(w);
        buf[i * 2 + 1] = (u8)(w >> 8);
    }
}

static void hdk_diskWriteSector(u32 lba, const u8 *buf)
{
    u32 i;

    hdk_waitBsy();

    writep8(ATA_DRIVE,   0xE0 | ((lba >> 24) & 0x0F));
    writep8(ATA_SECCNT,  1);
    writep8(ATA_LBA0,    (u8)(lba));
    writep8(ATA_LBA1,    (u8)(lba >> 8));
    writep8(ATA_LBA2,    (u8)(lba >> 16));
    writep8(ATA_COMMAND, 0x30);

    hdk_waitBsy();
    hdk_waitDrq();

    for (i = 0; i < 256; i++) {
        u16 w = (u16)buf[i * 2] | ((u16)buf[i * 2 + 1] << 8);
        writep16(ATA_DATA, w);
    }
}

#endif