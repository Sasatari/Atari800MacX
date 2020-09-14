/*
 * side2.c - Emulation of the SIDE2 cartridge
 *
 * Copyright (C) 2008-2012 Avery Lee
 * Copyright (C) 2020 Mark Grebe
 *
*/

#include "atari.h"
#include "side2.h"
#include "cartridge.h"
#include "cpu.h"
#include "ide.h"
#include "log.h"
#include "memory.h"
#include "rtcds1305.h"
#include "util.h"
#include "pia.h"
#include <stdlib.h>

int SIDE2_enabled = TRUE;

static UBYTE side2_rom[0x80000];
#ifdef ATARI800MACX
char side2_rom_filename[FILENAME_MAX] = "/Users/markg/Atari800MacX/Altirra-3.20/side2.rom"; //Util_FILENAME_NOT_SET;
char side2_nvram_filename[FILENAME_MAX] = "/Users/markg/Atari800MacX/Altirra-3.20/side2.nvram"; //Util_FILENAME_NOT_SET;
char side2_compact_flash_filename[FILENAME_MAX] = "/Users/markg/Atari800MacX/Altirra-3.20/side2.nvram"; //Util_FILENAME_NOT_SET;
#else
static char side_rom_filename[FILENAME_MAX] = Util_FILENAME_NOT_SET;
static char side2_nvram_filename[FILENAME_MAX] = Util_FILENAME_NOT_SET;
static char side2_compact_flash_filename[FILENAME_MAX] = Util_FILENAME_NOT_SET;
#endif

static int Block_Device = 0;
static int IDE_Enabled = FALSE;
static int IDE_Removed = TRUE;
static int IDE_Reset = FALSE;
static int SDX_Bank_Register = 0;
static int SDX_Enabled = FALSE;
static int Top_Bank_Register = 0;
static int Top_Enable = FALSE;
static int Top_Left_Enable = FALSE;
static int Top_Right_Enable = FALSE;
static void *rtc;

static void LoadNVRAM();
static void Reset_Cart_Bank(void);
static void SaveNVRAM();
static void Set_SDX_Bank(int bank, int topEnable);
static void Set_Top_Bank(int bank, int topLeftEnable, int topRightEnable);
static void Update_IDE_Reset(void);

#ifdef ATARI800MACX
void init_side2(void)
#else
static void init_side2(void)
#endif
{
    Log_print("Side2 enabled");
    if (!Atari800_LoadImage(side2_rom_filename, side2_rom, 0x80000)) {
        SIDE2_enabled = FALSE;
        Log_print("Couldn't load Side2 ROM image");
        return;
    }
    else {
        Log_print("loaded Side2 rom image");
    }
}

int SIDE2_Initialise(int *argc, char *argv[])
{
    init_side2();
    rtc = CDS1305_Init();
    LoadNVRAM();

    return TRUE;
}

void SIDE2_Exit(void)
{
    SaveNVRAM();
    CDS1305_Exit(rtc);
}

UBYTE SIDE2_D5GetByte(UWORD addr, int no_side_effects)
{
    int result = 0xff;
    
    switch(addr) {
        case 0xD5E1:    // SDX bank register
            result = SDX_Bank_Register;
            break;
        case 0xD5E2:    // DS1305 RTC
            result =  CDS1305_ReadState(rtc) ? 0x08 : 0x00;
            break;
        case 0xD5E4:    // top cartridge bank switching
            result = Top_Bank_Register;
            break;
        case 0xD5F0:
        case 0xD5F1:
        case 0xD5F2:
        case 0xD5F3:
        case 0xD5F4:
        case 0xD5F5:
        case 0xD5F6:
        case 0xD5F7:
            result = IDE_GetByte(addr, FALSE);
            break;
        case 0xD5F8:
            return 0x32;
            break;
        case 0xD5F9:
            result = IDE_Removed ? 1 : 0;
            break;
        case 0xD5FC:
            result = SDX_Enabled ? 'S' : ' ';
            break;
        case 0xD5FD:
            result = 'I';
            break;
        case 0xD5FE:
            result = 'D';
            break;
        case 0xD5FF:
            result = 'E';
            break;
        default:
            break;
    }

    return result;
}

void SIDE2_D5PutByte(UWORD addr, UBYTE byte)
{
    switch(addr) {
        case 0xD5E1:
            if (SDX_Bank_Register != byte) {
                SDX_Bank_Register = byte;
                Set_SDX_Bank(byte & 0x80 ? -1 : (byte & 0x3f), !(byte & 0x40));
            }
            break;

        case 0xD5E2:    // DS1305 RTC
            CDS1305_WriteState(rtc, (byte & 1) != 0, !(byte & 2), (byte & 4) != 0);
            break;

        case 0xD5E4:    // top cartridge bank switching
            if (Top_Bank_Register != byte) {
                Top_Bank_Register = byte;
                Set_Top_Bank((byte & 0x3f) ^ 0x20, (byte & 0x80) == 0, ((byte & 0x40) != 0));
            }
            break;

        case 0xD5F0:
        case 0xD5F1:
        case 0xD5F2:
        case 0xD5F3:
        case 0xD5F4:
        case 0xD5F5:
        case 0xD5F6:
        case 0xD5F7:
            IDE_PutByte(addr, byte);
            break;

        case 0xD5F8:    // F8-FB: D0 = /reset
        case 0xD5F9:
        case 0xD5FA:
        case 0xD5FB:
            if (addr == 0xD5F9) {
                // Strobe to clear CARD_REMOVED. This can't be done if there isn't actually a
                // card.
                if (Block_Device)
                    IDE_Removed = FALSE;
            }

            IDE_Enabled = !(byte & 0x80);
            break;
        default:
            break;
        }
}

void SIDE2_ColdStart(void)
{
    // Reset RTC Chip
    CDS1305_ColdReset(rtc);

    Reset_Cart_Bank();

    IDE_Reset = TRUE;
    IDE_Enabled = TRUE;

    // If the CF card is absent, the removed flag is always set and can't be
    // cleared. If it's present, the removed flag is cleared on powerup.
    IDE_Removed = !Block_Device;

    Update_IDE_Reset();

}

static void LoadNVRAM()
{
    UBYTE buf[0x72];
    FILE *f;
    int len;
    
    f = fopen(side2_nvram_filename, "rb");
    if (f == NULL) {
        memset(buf, 0, sizeof(buf));
    } else {
        len = fread(buf, 1, 0x72, f);
        fclose(f);
        if (len != 0x72) {
            memset(buf, 0, sizeof(buf));
        }
    }
    CDS1305_Load(rtc, buf);
}

static void SaveNVRAM()
{
    UBYTE buf[0x72];
    FILE *f;

    CDS1305_Save(rtc, buf);
    f = fopen(side2_nvram_filename, "wb");
    if (f != NULL) {
        fwrite(buf, 1, 0x72, f);
        fclose(f);
    }
}

static void Set_SDX_Bank(int bank, int topEnable)
{
    if (SDX_Bank_Register == bank && Top_Enable == topEnable)
        return;

    SDX_Bank_Register = bank;
    Top_Enable = topEnable;

    //UpdateMemoryLayersCart();
    //mpCartridgePort->OnLeftWindowChanged(mCartId, IsLeftCartActive());
}

static void Set_Top_Bank(int bank, int topLeftEnable, int topRightEnable)
{
    // If the top cartridge is enabled in 16K mode, the LSB bank bit is ignored.
    // We force the LSB on in that case so the right cart window is in the right
    // place and the left cart window is 8K below that (mask LSB back off).
    if (topRightEnable)
        bank |= 0x01;

    if (Top_Bank_Register == bank && Top_Right_Enable == topRightEnable && Top_Left_Enable == topLeftEnable)
        return;

    Top_Bank_Register = bank;
    Top_Left_Enable = topLeftEnable;
    Top_Right_Enable = topRightEnable;

    //UpdateMemoryLayersCart();
    //mpCartridgePort->OnLeftWindowChanged(mCartId, IsLeftCartActive());
}

static void Reset_Cart_Bank(void)
{
    SDX_Bank_Register = 0x00;
    Set_SDX_Bank(0, TRUE);

    Top_Bank_Register = 0x00;
    Set_Top_Bank(0x20, TRUE, FALSE);
}

static void Update_IDE_Reset(void)
{
    //mIDE.SetReset(IDE_Reset || !Block_Device);
}

