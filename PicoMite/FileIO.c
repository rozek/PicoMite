/***********************************************************************************************************************
PicoMite MMBasic

FileIO.c

<COPYRIGHT HOLDERS>  Geoff Graham, Peter Mather
Copyright (c) 2021, <COPYRIGHT HOLDERS> All rights reserved.
Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
1.	Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2.	Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the distribution.
3.	The name MMBasic be used when referring to the interpreter in any documentation and promotional material and the original copyright message be displayed
    on the console at startup (additional copyright messages may be added).
4.	All advertising materials mentioning features or use of this software must display the following acknowledgement: This product includes software developed
    by the <copyright holder>.
5.	Neither the name of the <copyright holder> nor the names of its contributors may be used to endorse or promote products derived from this software
    without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDERS> AS IS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDERS> BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

************************************************************************************************************************/
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "ff.h"
#include "diskio.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "pico/binary_info.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/clocks.h"
#include "sys/stat.h"
#include "picojpeg.h"
extern const uint8_t *flash_target_contents;
extern const uint8_t *flash_option_contents;
extern const uint8_t *SavedVarsFlash;
extern const uint8_t *flash_progmemory;
extern void routinechecks(void);
struct option_s Option;
int dirflags;
int GPSfnbr = 0;
int lfs_FileFnbr=0;
int FatFSFileSystem=0; //Assume we are using flash file system

// 8*8*4 bytes * 3 = 768
int16_t *gCoeffBuf;

// 8*8*4 bytes * 3 = 768
uint8_t *gMCUBufR;
uint8_t *gMCUBufG;
uint8_t *gMCUBufB;

// 256 bytes
int16_t *gQuant0;
int16_t *gQuant1;
uint8_t *gHuffVal2;
uint8_t *gHuffVal3;
uint8_t *gInBuf;
#define BLOCK_SIZE 4096
char FlashReadBuffer[256];
char FlashProgBuffer[256];
char FlashLookBuffer[256];
int fs_flash_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);
int fs_flash_prog(const struct lfs_config *cfg, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size);
int fs_flash_erase(const struct lfs_config *cfg, lfs_block_t block);
int fs_flash_sync(const struct lfs_config *c);
struct lfs_config pico_lfs_cfg = {
    // block device operations
    .read  = fs_flash_read,
    .prog  = fs_flash_prog,
    .erase = fs_flash_erase,
    .sync  = fs_flash_sync,
   // block device configuration
    .read_size = 1,
    .prog_size = 256,
    .block_size = BLOCK_SIZE,
    .block_count = 0,
    .block_cycles=500,
    .cache_size=256,
    .lookahead_size = 256,
	.read_buffer = (void *)FlashReadBuffer,
	.prog_buffer = (void *)FlashProgBuffer,
	.lookahead_buffer = (void *)FlashLookBuffer,
};

volatile union u_flash
{
    uint64_t i64[32];
    uint8_t i8[256];
    uint32_t i32[64];
} MemWord;
volatile int mi8p = 0;
volatile uint32_t realflashpointer;
int FlashLoad = 0;
unsigned char *CFunctionFlash = NULL;
unsigned char *CFunctionLibrary = NULL;
#define SDbufferSize 512
static char *SDbuffer[MAXOPENFILES + 1] = {NULL};
int buffpointer[MAXOPENFILES + 1] = {0};
static uint32_t lastfptr[MAXOPENFILES + 1] = {[0 ... MAXOPENFILES] = -1};
uint8_t fmode[MAXOPENFILES + 1] = {0};
static unsigned int bw[MAXOPENFILES + 1] = {[0 ... MAXOPENFILES] = -1};
unsigned char filesource[MAXOPENFILES + 1] = {0};
char filepath[2][FF_MAX_LFN]={
    "A:/",
    "B:/"
};
char fullpathname[2][FF_MAX_LFN];
char fullfilepathname[FF_MAX_LFN];
extern BYTE BMP_bDecode(int x, int y, int fnbr);
#define RoundUp(a) (((a) + (sizeof(int) - 1)) & (~(sizeof(int) - 1))) // round up to the nearest integer size      [position 27:9]
int resolve_path(char *path, char *result, char *pos);
void getfullpath(char *p, char *q);
void getfullfilepath(char *p, char *q);
void fullpath(char *q);
int FatFSFileSystemSave=0;
/******************************************************************************************
Text for the file related error messages reported by MMBasic
******************************************************************************************/
const char *FErrorMsg[] = {"",
                           "A hard error occurred in the low level disk I/O layer",
                           "Assertion failed",
                           "SD Card not found",
                           "Could not find the file",
                           "Could not find the path",
                           "The path name format is invalid",
                           "FAccess denied due to prohibited access or directory full",
                           "Access denied due to prohibited access",
                           "The file/directory object is invalid",
                           "The physical drive is write protected",
                           "The logical drive number is invalid",
                           "The volume has no work area",
                           "There is no valid FAT volume",
                           "The f_mkfs() aborted due to any problem",
                           "Could not get a grant to access the volume within defined period",
                           "The operation is rejected according to the file sharing policy",
                           "LFN working buffer could not be allocated",
                           "Number of open files > FF_FS_LOCK",
                           "Given parameter is invalid",
                           "SD card not present"};
const char *LFSErrorMsg[]={
                            "",
                            "",
                            "Could not find the file",
                            "Could not find the path",
                            "",
                            "Error during device operation",
                            "",
                            "",
                            "",
                            "Bad file number",
                            "",
                            "",
                            "No more memory available",
                            "",
                            "",
                            "",
                            "",
                            "Entry already exists",
                            "",
                            "",
                            "Entry is not a dir",
                            "Entry is a dir",
                            "Invalid parameter",
                            "",
                            "",
                            "",
                            "",
                            "File too large",
                            "No space left on device",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "File name too long",
                            "",
                            "",
                            "Dir is not empty",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "No data/attr available",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "",
                            "Corrupted"
    };
extern BYTE MDD_SDSPI_CardDetectState(void);
extern void InitReservedIO(void);
int ForceFileClose(int fnbr);
int FSerror;
FATFS FatFs;
union uFileTable FileTable[MAXOPENFILES + 1];
volatile BYTE SDCardStat = STA_NOINIT | STA_NODISK;
int OptionFileErrorAbort = true;
bool irqs[32];
void disable_interrupts(void)
{
    int i;
    for (i = 0; i < 31; i++)
    {
        irqs[i] = irq_is_enabled(i);
        if (irqs[i])
        {
            irq_set_enabled(i, false);
        }
    }
}
void enable_interrupts(void)
{
    int i;
    for (i = 0; i < 31; i++)
    {
        if (irqs[i])
            irq_set_enabled(i, true);
    }
}
void ErrorThrow(int e, int type)
{
    FatFSFileSystem = FatFSFileSystemSave;
    MMerrno = e;
    FSerror = e;
    if(type==FATFSFILE)strcpy(MMErrMsg, (char *)FErrorMsg[e]);
    if(type==FLASHFILE)strcpy(MMErrMsg, (char *)LFSErrorMsg[-e]);
    if (e == 1)
    {
        BYTE s;
        s = SDCardStat;
        s |= (STA_NODISK | STA_NOINIT);
        SDCardStat = s;
        memset(&FatFs, 0, sizeof(FatFs));
    }
    if (e && OptionFileErrorAbort)
        error(MMErrMsg);
    return;
}
void ResetFlashStorage(int umount){
    int boot_count=0;
    if(umount)lfs_unmount(&lfs); 
    FSerror=lfs_format(&lfs, &pico_lfs_cfg);ErrorCheck(0);
    FSerror=lfs_mount(&lfs, &pico_lfs_cfg);	ErrorCheck(0);
    int fnbr = FindFreeFileNbr();
    BasicFileOpen("bootcount",fnbr,FA_WRITE | FA_OPEN_APPEND | FA_READ);
    FSerror=lfs_file_read(&lfs, FileTable[fnbr].lfsptr, &boot_count, sizeof(boot_count));
    if(FSerror>0)FSerror=0;
    ErrorCheck(fnbr);
    boot_count+=1;
    FSerror=lfs_file_rewind(&lfs, FileTable[fnbr].lfsptr);
    ErrorCheck(fnbr);
    FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, &boot_count, sizeof(boot_count));
    if(FSerror>0)FSerror=0;
    ErrorCheck(fnbr);
    FileClose(fnbr);
 }

int __not_in_flash_func(fs_flash_read)(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size)
{
    assert(off  % cfg->read_size == 0);
    assert(size % cfg->read_size == 0);
    assert(block < cfg->block_count);
    uint32_t addr = XIP_BASE + RoundUpK4(TOP_OF_SYSTEM_FLASH) + block*4096 + off;
    memcpy(buffer,(char *)addr,size);
    return 0;
}
int __not_in_flash_func(fs_flash_prog)(const struct lfs_config *cfg, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size)
{
    assert(off  % cfg->prog_size == 0);
    assert(size % cfg->prog_size == 0);
    assert(block < cfg->block_count);

    uint32_t addr = RoundUpK4(TOP_OF_SYSTEM_FLASH) + block*4096 + off;
    disable_interrupts();
    flash_range_program(addr, buffer, size);
    enable_interrupts();
    return 0;
}
int __not_in_flash_func(fs_flash_erase)(const struct lfs_config *cfg, lfs_block_t block){
    assert(block < cfg->block_count);

    uint32_t block_addr = RoundUpK4(TOP_OF_SYSTEM_FLASH) + block*4096;
        disable_interrupts();
        flash_range_erase(block_addr, BLOCK_SIZE);
        enable_interrupts();
        return 0;
}
int __not_in_flash_func(fs_flash_sync)(const struct lfs_config *c)
{
    return 0;
}
void cmd_disk(void){
    char *p=getCstring(cmdline);
    char *b=GetTempMemory(STRINGSIZE);
    for(int i=0;i<strlen(p);i++)b[i]=toupper(p[i]);
    if(strcmp(b, "A:/FORMAT")==0)  { 
        FatFSFileSystem = FatFSFileSystemSave = 0;
        ResetFlashStorage(1);
        return; 
    }
    if(strcmp(b, "A:")==0)  { FatFSFileSystem = FatFSFileSystemSave = 0;  return; }
    if(strcmp(b, "B:")==0)    {
        if(!Option.SD_CS)error("B: drive not enabled");
        FatFSFileSystem = FatFSFileSystemSave = 1;
        return; 
    }
    error((char *)"Syntax");
}
void cmd_flash(void)
{
    unsigned char *p;
    if ((p = checkstring(cmdline, "ERASE ALL")))
    {
        if (CurrentLinePtr)
            error("Invalid in program");
        uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE;
        uSec(250000);
        disable_interrupts();
        for (int i = 0; i < MAXFLASHSLOTS; i++)
        {
            uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (i * MAX_PROG_SIZE);
            flash_range_erase(j, MAX_PROG_SIZE);
        }
        enable_interrupts();
    }
    else if ((p = checkstring(cmdline, "ERASE")))
    {
        if (CurrentLinePtr)
            error("Invalid in program");
        int i = getint(p, 1, MAXFLASHSLOTS);
        uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i - 1) * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
    }
    else if ((p = checkstring(cmdline, "OVERWRITE")))
    {
        if (CurrentLinePtr)
            error("Invalid in program");
        int i = getint(p, 1, MAXFLASHSLOTS);
        uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i - 1) * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
        j = (Option.PROG_FLASH_SIZE >> 2);
        uSec(250000);
        int *pp = (int *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        while (j--)
            if (*pp++ != 0xFFFFFFFF)
            {
                error("Erase error");
            }
        disable_interrupts();
        uint8_t *q = ProgMemory;
        uint8_t *writebuff = GetTempMemory(4096);
        for (int k = 0; k < MAX_PROG_SIZE; k += 4096)
        {
            for (int j = 0; j < 4096; j++)
                writebuff[j] = *q++;
            flash_range_program(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i - 1) * MAX_PROG_SIZE + k), writebuff, 4096);
        }
        enable_interrupts();
    }
    else if ((p = checkstring(cmdline, "LIST")))
    {
        int j, i, k;
        int *pp;
        getargs(&p, 3, ",");
        if (argc)
        {
            int i = getint(argv[0], 1, MAXFLASHSLOTS);
            ProgMemory = (char *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
            if (argc == 1)
                ListProgram((char *)ProgMemory, false);
            else if (argc == 3 && checkstring(argv[2], "ALL"))
            {
                ListProgram((char *)ProgMemory, true);
            }
            else
                error("Syntax");
            ProgMemory = (unsigned char *)flash_progmemory;
        }
        else
        {
            for (i = 1; i <= MAXFLASHSLOTS; i++)
            {
                k = 0;
                j = Option.PROG_FLASH_SIZE >> 2;
                pp = (int *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
                while (j--)
                    if (*pp++ != 0xFFFFFFFF)
                    {
                        char buff[STRINGSIZE] = {0};
                        MMPrintString(" Slot ");
                        PInt(i);
                        MMPrintString(" in use");
                        pp--;
                        if ((unsigned char)*pp == T_NEWLINE)
                        {
                            MMPrintString(": \"");
                            llist(buff, (unsigned char *)pp);
                            MMPrintString(buff);
                            MMPrintString("\"\r\n");
                        }
                        else
                            MMPrintString("\r\n");
                        k = 1;
                        break;
                    }
                if (k == 0)
                {
                    MMPrintString(" Slot ");
                    PInt(i);
                    MMPrintString(" available\r\n");
                }
            }
        }
    }
    else if ((p = checkstring(cmdline, "SAVE")))
    {
        if (CurrentLinePtr)
            error("Invalid in program");
        int i = getint(p, 1, MAXFLASHSLOTS);
        uint32_t *c = (uint32_t *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        if (*c != 0xFFFFFFFF)
            error("Already programmed");
        ;
        uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i - 1) * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
        j = (Option.PROG_FLASH_SIZE >> 2);
        uSec(250000);
        int *pp = (int *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        while (j--)
            if (*pp++ != 0xFFFFFFFF)
            {
                error("Erase error");
            }
        disable_interrupts();
        uint8_t save = 0, *q = (uint8_t *)ProgMemory;
        uint8_t *writebuff = (uint8_t *)GetTempMemory(4096);
        for (int k = 0; k < MAX_PROG_SIZE; k += 4096)
        {
            for (int j = 0; j < 4096; j++)
                writebuff[j] = *q++;
            flash_range_program(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i - 1) * MAX_PROG_SIZE + k), (uint8_t *)writebuff, 4096);
        }
        enable_interrupts();
    }
    else if ((p = checkstring(cmdline, "LOAD")))
    {
        if (CurrentLinePtr)
            error("Invalid in program");
        int j = (Option.PROG_FLASH_SIZE >> 2), i = getint(p, 1, MAXFLASHSLOTS);
        disable_interrupts();
        flash_range_erase(PROGSTART, MAX_PROG_SIZE);
        enable_interrupts();
        j = (Option.PROG_FLASH_SIZE >> 2);
        uSec(250000);
        int *pp = (int *)flash_progmemory;
        while (j--)
            if (*pp++ != 0xFFFFFFFF)
            {
                error("Erase error");
            }
        disable_interrupts();
        uint8_t *q = (uint8_t *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        uint8_t *writebuff = GetTempMemory(4096);
        if (*q == 0xFF)
        {
            enable_interrupts();
            FlashWriteInit(PROGRAM_FLASH);
            flash_range_erase(realflashpointer, MAX_PROG_SIZE);
            FlashWriteByte(0);
            FlashWriteByte(0);
            FlashWriteByte(0); // terminate the program in flash
            FlashWriteClose();
            error("Flash slot empty");
        }
        for (int k = 0; k < MAX_PROG_SIZE; k += 4096)
        {
            for (int j = 0; j < 4096; j++)
                writebuff[j] = *q++;
            flash_range_program((PROGSTART + k), writebuff, 4096);
        }
        enable_interrupts();
        FlashLoad = 0;
    }
    else if ((p = checkstring(cmdline, "CHAIN")))
    {
        if (!CurrentLinePtr)
            error("Invalid at command prompt");
        int i = getint(p, 0, MAXFLASHSLOTS);
        if(i) ProgMemory = (char *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        else ProgMemory = (char *)(flash_target_contents + MAXFLASHSLOTS * MAX_PROG_SIZE);
        FlashLoad = i;
        PrepareProgram(true);
        nextstmt = (unsigned char *)ProgMemory;
    }
    else if ((p = checkstring(cmdline, "RUN")))
    {
        int i = getint(p, 0, MAXFLASHSLOTS);
        if(i) ProgMemory = (char *)(flash_target_contents + (i - 1) * MAX_PROG_SIZE);
        else ProgMemory = (char *)(flash_target_contents + MAXFLASHSLOTS * MAX_PROG_SIZE);
        ClearRuntime();
        PrepareProgram(true);
        nextstmt = (unsigned char *)ProgMemory;
    }
    else
        error("Syntax");
}

void ErrorCheck(int fnbr)
{ // checks for an error, if fnbr is specified frees up the filehandle before sending error
    int e, type;
    e = (int)FSerror;
    if (fnbr != 0 && e != 0)
        type=ForceFileClose(fnbr);
    if (e >= 1 && e <= 19)
        ErrorThrow(e, FATFSFILE);
    if (e<0 && e>=-84)
        ErrorThrow(e, FLASHFILE);
    return;
}
char *GetCWD(void)
{
    char *b;
    b = GetTempMemory(STRINGSIZE);
    if(FatFSFileSystem){
        if (!InitSDCard())
            return b;
        FSerror = f_getcwd(b, STRINGSIZE);
        ErrorCheck(0);
        return &b[1];
    }   else {
        fullpath("");
        strcpy(b,"A:");
        strcat(b,fullpathname[FatFSFileSystem]);
        return b;
    }
}
void LoadImage(unsigned char *p)
{
    int fnbr;
    int xOrigin, yOrigin;

    // get the command line arguments
    getargs(&p, 5, ","); // this MUST be the first executable line in the function
    if (argc == 0)
        error("Argument count");
    if (!InitSDCard())
        return;

    p = getCstring(argv[0]); // get the file name

    xOrigin = yOrigin = 0;
    if (argc >= 3)
        xOrigin = getinteger(argv[2]); // get the x origin (optional) argument
    if (argc == 5)
        yOrigin = getinteger(argv[4]); // get the y origin (optional) argument

    // open the file
    if (strchr(p, '.') == NULL)
        strcat(p, ".bmp");
    fnbr = FindFreeFileNbr();
    if (!BasicFileOpen(p, fnbr, FA_READ))
        return;
    BMP_bDecode(xOrigin, yOrigin, fnbr);
    FileClose(fnbr);
    if (Option.Refresh)
        Display_Refresh();
}
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

static uint g_nInFileSize;
static uint g_nInFileOfs;
static int jpgfnbr;
unsigned char pjpeg_need_bytes_callback(unsigned char *pBuf, unsigned char buf_size, unsigned char *pBytes_actually_read, void *pCallback_data)
{
    uint n, n_read;
    pCallback_data;

    n = min(g_nInFileSize - g_nInFileOfs, buf_size);
    if(FatFSFileSystem)  f_read(FileTable[jpgfnbr].fptr, pBuf, n, &n_read);
    else n_read=lfs_file_read(&lfs, FileTable[jpgfnbr].lfsptr, pBuf, n);
    if (n != n_read)
        return PJPG_STREAM_READ_ERROR;
    *pBytes_actually_read = (unsigned char)(n);
    g_nInFileOfs += n;
    return 0;
}

void LoadJPGImage(unsigned char *p)
{
    pjpeg_image_info_t image_info;
    int x, mcu_x = 0;
    int y, mcu_y = 0;
    uint row_pitch;
    uint8_t *pImage;
    uint8_t status;
    gCoeffBuf = (int16_t *)GetTempMemory(8 * 8 * sizeof(int16_t));
    gMCUBufR = (uint8_t *)GetTempMemory(256);
    gMCUBufG = (uint8_t *)GetTempMemory(256);
    gMCUBufB = (uint8_t *)GetTempMemory(256);
    gQuant0 = (int16_t *)GetTempMemory(8 * 8 * sizeof(int16_t));
    gQuant1 = (int16_t *)GetTempMemory(8 * 8 * sizeof(int16_t));
    gHuffVal2 = (uint8_t *)GetTempMemory(256);
    gHuffVal3 = (uint8_t *)GetTempMemory(256);
    gInBuf = (uint8_t *)GetTempMemory(PJPG_MAX_IN_BUF_SIZE);
    g_nInFileSize = g_nInFileOfs = 0;

    uint decoded_width, decoded_height;
    uint row_blocks_per_mcu, col_blocks_per_mcu;
    int xOrigin, yOrigin;

    // get the command line arguments
    getargs(&p, 5, ","); // this MUST be the first executable line in the function
    if (argc == 0)
        error("Argument count");
    if (!InitSDCard())
        return;

    p = getCstring(argv[0]); // get the file name

    xOrigin = yOrigin = 0;
    if (argc >= 3)
        xOrigin = getint(argv[2], 0, HRes - 1); // get the x origin (optional) argument
    if (argc == 5)
        yOrigin = getint(argv[4], 0, VRes - 1); // get the y origin (optional) argument

    // open the file
    if (strchr(p, '.') == NULL)
        strcat(p, ".jpg");
    jpgfnbr = FindFreeFileNbr();
    if (!BasicFileOpen(p, jpgfnbr, FA_READ))
        return;

    if(FatFSFileSystem)  g_nInFileSize = f_size(FileTable[jpgfnbr].fptr);
    else g_nInFileSize = lfs_file_size(&lfs,FileTable[jpgfnbr].lfsptr);
    status = pjpeg_decode_init(&image_info, pjpeg_need_bytes_callback, NULL, 0);

    if (status)
    {
        if (status == PJPG_UNSUPPORTED_MODE)
        {
            FileClose(jpgfnbr);
            error("Progressive JPEG files are not supported");
        }
        FileClose(jpgfnbr);
        error("pjpeg_decode_init() failed with status %", status);
    }
    decoded_width = image_info.m_width;
    decoded_height = image_info.m_height;

    row_pitch = image_info.m_MCUWidth * image_info.m_comps;

    unsigned char *imageblock = GetTempMemory(image_info.m_MCUHeight * image_info.m_MCUWidth * image_info.m_comps);
    for (;;)
    {
        uint8_t *pDst_row = imageblock;
        int y, x;

        status = pjpeg_decode_mcu();

        if (status)
        {
            if (status != PJPG_NO_MORE_BLOCKS)
            {
                FileClose(jpgfnbr);
                error("pjpeg_decode_mcu() failed with status %", status);
            }
            break;
        }

        if (mcu_y >= image_info.m_MCUSPerCol)
        {
            FileClose(jpgfnbr);
            return;
        }
        /*    for(int i=0;i<image_info.m_MCUHeight*image_info.m_MCUWidth ;i++){
                  imageblock[i*3+2]=image_info.m_pMCUBufR[i];
                  imageblock[i*3+1]=image_info.m_pMCUBufG[i];
                  imageblock[i*3]=image_info.m_pMCUBufB[i];
              }*/
        //         pDst_row = pImage + (mcu_y * image_info.m_MCUHeight) * row_pitch + (mcu_x * image_info.m_MCUWidth * image_info.m_comps);

        for (y = 0; y < image_info.m_MCUHeight; y += 8)
        {
            const int by_limit = min(8, image_info.m_height - (mcu_y * image_info.m_MCUHeight + y));
            for (x = 0; x < image_info.m_MCUWidth; x += 8)
            {
                uint8_t *pDst_block = pDst_row + x * image_info.m_comps;
                // Compute source byte offset of the block in the decoder's MCU buffer.
                uint src_ofs = (x * 8U) + (y * 16U);
                const uint8_t *pSrcR = image_info.m_pMCUBufR + src_ofs;
                const uint8_t *pSrcG = image_info.m_pMCUBufG + src_ofs;
                const uint8_t *pSrcB = image_info.m_pMCUBufB + src_ofs;

                const int bx_limit = min(8, image_info.m_width - (mcu_x * image_info.m_MCUWidth + x));

                {
                    int bx, by;
                    for (by = 0; by < by_limit; by++)
                    {
                        uint8_t *pDst = pDst_block;

                        for (bx = 0; bx < bx_limit; bx++)
                        {
                            pDst[2] = *pSrcR++;
                            pDst[1] = *pSrcG++;
                            pDst[0] = *pSrcB++;
                            pDst += 3;
                        }

                        pSrcR += (8 - bx_limit);
                        pSrcG += (8 - bx_limit);
                        pSrcB += (8 - bx_limit);

                        pDst_block += row_pitch;
                    }
                }
            }
            pDst_row += (row_pitch * 8);
        }

        x = mcu_x * image_info.m_MCUWidth + xOrigin;
        y = mcu_y * image_info.m_MCUHeight + yOrigin;
        if (y < VRes && x < HRes)
        {
            int yend = min(VRes - 1, y + image_info.m_MCUHeight - 1);
            int xend = min(HRes - 1, x + image_info.m_MCUWidth - 1);
            if (xend < x + image_info.m_MCUWidth - 1)
            {
                // need to get rid of some pixels to remove artifacts
                xend = HRes - 1;
                unsigned char *s = imageblock;
                unsigned char *d = imageblock;
                for (int yp = 0; yp < image_info.m_MCUHeight; yp++)
                {
                    for (int xp = 0; xp < image_info.m_MCUWidth; xp++)
                    {
                        if (xp < xend - x + 1)
                        {
                            *d++ = *s++;
                            *d++ = *s++;
                            *d++ = *s++;
                        }
                        else
                        {
                            s += 3;
                        }
                    }
                }
            }
            DrawBuffer(x, y, xend, yend, imageblock);
        }

        if (y >= VRes)
        { // nothing useful left to process
            FileClose(jpgfnbr);
            if (Option.Refresh)
                Display_Refresh();
            return;
        }
        mcu_x++;
        if (mcu_x == image_info.m_MCUSPerRow)
        {
            mcu_x = 0;
            mcu_y++;
        }
    }
    FileClose(jpgfnbr);
    if (Option.Refresh)
        Display_Refresh();
}

// search for a volume label, directory or file
// s$ = DIR$(fspec, DIR|FILE|ALL)       will return the first entry
// s$ = DIR$()                          will return the next
// If s$ is empty then no (more) files found
void fun_dir(void)
{
    static DIR djd;
    static FILINFO fnod;
    static char pp[FF_MAX_LFN];
    static char path[FF_MAX_LFN];
    static int FSsave;
    static lfs_dir_t lfs_dir_dir;
    struct lfs_info lfs_info_dir;
    unsigned char *p;
    getargs(&ep, 3, ",");
    if (argc != 0)
        dirflags = -1;
    if (!(argc <= 3))
        error("Syntax");

    if (argc == 3)
    {
        if (checkstring(argv[2], "DIR"))
            dirflags = AM_DIR;
        else if (checkstring(argv[2], "FILE"))
            dirflags = -1;
        else if (checkstring(argv[2], "ALL"))
            dirflags = 0;
        else
            error("Invalid flag specification");
    }

    if (argc != 0)
    {
        memset(pp,0,FF_MAX_LFN);
        memset(path,0,FF_MAX_LFN);
        memset(&djd, 0, sizeof(DIR));
        memset(&fnod, 0, sizeof(FILINFO));
        // this must be the first call eg:  DIR$("*.*", FILE)
        p = getCstring(argv[0]);
        char fullfilename[FF_MAX_LFN]={0};
        getfullfilename(p,fullfilename);
        FSsave=FatFSFileSystem;
        for (int j = 0; j < strlen(fullfilename); j++)
            if (fullfilename[j] == '\\')
                fullfilename[j] = '/'; // allow backslash for the DOS oldies
        int i = strlen(fullfilename) - 1;
        while (i > 1 && !(fullfilename[i] == '/')) i--;
        if(i>1){
            memcpy(path,fullfilename,i);
            strcpy(pp,&fullfilename[i+1]);
        } else {
            strcpy(pp,&fullfilename[1]);
        }
        if(!(*pp))*pp='*';
        if(!(*path))*path='/';
        djd.pat = pp;
        if (!InitSDCard())
            return; // setup the SD card
        if(FSsave==1)FSerror = f_opendir(&djd, path);
        else FSerror=lfs_dir_open(&lfs, &lfs_dir_dir, path);
        ErrorCheck(0);
    }
    if (SDCardStat & STA_NOINIT && FSsave==1)
    {
        f_closedir(&djd);
        error("SD card not found");
    }
    if (dirflags == AM_DIR)
    {
        for (;;)
        {
            if(FSsave==1){
                    FSerror = f_readdir(&djd, &fnod); // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0])
                    break; // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && (fnod.fattrib & AM_DIR) && !(fnod.fattrib & AM_SYS))
                    break; // Test for the file name
            } else {
                FSerror=lfs_dir_read(&lfs, &lfs_dir_dir, &lfs_info_dir);
                strcpy(fnod.fname,lfs_info_dir.name);
                if(FSerror==0)                    
                    break;
                if (lfs_info_dir.type==LFS_TYPE_DIR && pattern_matching(pp, lfs_info_dir.name, 0, 0)){
                    break;
                }
            }
        }
    }
    else if (dirflags == -1)
    {
        for (;;)
        {
            if(FSsave==1){
                FSerror = f_readdir(&djd, &fnod); // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0])
                    break; // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && !(fnod.fattrib & AM_DIR) && !(fnod.fattrib & AM_SYS))
                    break; // Test for the file name
            } else {
                FSerror=lfs_dir_read(&lfs, &lfs_dir_dir, &lfs_info_dir);
                strcpy(fnod.fname,lfs_info_dir.name);
                if(FSerror==0)                    
                    break;
                if (lfs_info_dir.type==LFS_TYPE_REG && pattern_matching(pp, lfs_info_dir.name, 0, 0)){
                    break;
                }
            }
        }
    }
    else
    {
        for (;;)
        {
            if(FSsave==1){
                FSerror = f_readdir(&djd, &fnod); // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0])
                    break; // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && !(fnod.fattrib & AM_SYS))
                    break; // Test for the file name
            } else {
                FSerror=lfs_dir_read(&lfs, &lfs_dir_dir, &lfs_info_dir);
                strcpy(fnod.fname,lfs_info_dir.name);
                if(FSerror==0)                    
                    break;
                if (lfs_info_dir.type & (LFS_TYPE_REG |  LFS_TYPE_DIR) && pattern_matching(pp, lfs_info_dir.name, 0, 0)){
                    break;
                }
            }
        }
    }

    if (FSerror != FR_OK || !fnod.fname[0])
        if(FSsave==1)f_closedir(&djd);
        else lfs_dir_close(&lfs, &lfs_dir_dir);

    sret = GetTempMemory(STRINGSIZE); // this will last for the life of the command
    strcpy(sret, fnod.fname);
    CtoM(sret); // convert to a MMBasic style string
    FatFSFileSystem=FatFSFileSystemSave;
    targ = T_STR;
}

void cmd_mkdir(void)
{
    char *p;
    int i;
    char q[FF_MAX_LFN]={0};
    p = getCstring(cmdline);                                        // get the directory name and convert to a standard C string
    if(drivecheck(p,&i)!=FatFSFileSystem+1) error("Only valid on current drive");
    getfullpath(p,q);
    if(FatFSFileSystem){
        if (!InitSDCard())
        return;
        FSerror = f_mkdir(q);
        ErrorCheck(0);
    } else {
        FSerror=lfs_mkdir(&lfs, q);
        ErrorCheck(0);
    }
}

void cmd_rmdir(void)
{
    char *p;
    int i;
    char q[FF_MAX_LFN]={0};
    p = getCstring(cmdline);                                        // get the directory name and convert to a standard C string
    if(drivecheck(p,&i)!=FatFSFileSystem+1) error("Only valid on current drive");
    getfullpath(p,q);
    if(FatFSFileSystem){
        if (!InitSDCard())
            return;
        FSerror = f_unlink(q);
        ErrorCheck(0);
    } else {
        FSerror=lfs_remove(&lfs, q);
        ErrorCheck(0);
    }
}

void cmd_chdir(void){
	int i;
    char *p;
    char rp[STRINGSIZE],oldfilepath[STRINGSIZE];
    p = getCstring(cmdline);  // get the directory name and convert to a standard C string
    if(drivecheck(p,&i)!=FatFSFileSystem+1) error("Only valid on current drive");
    for(i=0;i<strlen(p);i++)if(p[i]=='\\')p[i]='/';  //allow backslash for the DOS oldies
    if(strcmp(p,".")==0)return; //nothing to do
    if(strlen(p)==0)return;//nothing to do
    strcpy(oldfilepath,filepath[FatFSFileSystem]); //save the path in case the change of directory fails
    if(p[1]==':'){ //modify the requested path so that if the disk is specified the pathname is absolute and starts with /
    	if(p[2]=='/')p+=2;
    	else {
    		p[1]='/';
    		p++;
    	}
    }
    if (*p=='/'){ //absolute path specified
    	strcpy(rp,FatFSFileSystem==0? "A:":"B:");
    	strcat(rp,p);
    } else { // relative path specified
    	strcpy(rp,filepath[FatFSFileSystem]); //copy the current pathname
        if(rp[strlen(rp)-1]!='/')  strcat(rp,"/"); //make sure the previous pathname ends in slash, will only be the case at root
    	strcat(rp,p); //append the new pathname
    }
	strcpy(filepath[FatFSFileSystem],rp); //set the new pathname
	resolve_path(filepath[FatFSFileSystem],rp,rp); //resolve to single absolute path
	if(strcmp(rp,"A:")==0 || strcmp(rp,"B:")==0 )strcat(rp,"/"); //if root append the slash
	strcpy(filepath[FatFSFileSystem],rp); //store this back to the filepath variable
    if(!InitSDCard()) { //If no disk restore the old path and return
    	strcpy(filepath[FatFSFileSystem],oldfilepath);
    	return;
    }
//    MMPrintString(filepath[FatFSFileSystem]);
	if(FatFSFileSystem)FSerror = f_chdir(&filepath[FatFSFileSystem][2]); //finally change directory always using an absolute pathname
    else {
//        MMPrintString(&filepath[FatFSFileSystem][3]);PRet();
        FSerror=lfs_dir_open(&lfs, &lfs_dir, &filepath[FatFSFileSystem][3]);
        if(!FSerror)lfs_dir_close(&lfs, &lfs_dir);
    }
    if(FSerror==-2)FSerror=-3;
	if(FSerror)strcpy(filepath[FatFSFileSystem],oldfilepath); //if it didn't work restore the original path
	ErrorCheck(0); // error if the pathname was invalid

}

void fun_cwd(void)
{
    sret = CtoM(GetCWD());
    targ = T_STR;
}

void cmd_kill(void)
{
    char q[FF_MAX_LFN]={0};
    char *p = getCstring(cmdline);
    getfullfilepath(p,q);
    if(!FatFSFileSystem){
        FSerror=lfs_remove(&lfs, q);	ErrorCheck(0);
    } else {
        if (!InitSDCard()) return;
        FSerror = f_unlink(q);
        ErrorCheck(0);
    }
}

void cmd_seek(void)
{
    int fnbr, idx;
    char *buff;
    getargs(&cmdline, 5, ",");
    if (argc != 3)
        error("Syntax");
    if (*argv[0] == '#')
        argv[0]++;
    fnbr = getinteger(argv[0]);
    if (fnbr < 1 || fnbr > MAXOPENFILES || FileTable[fnbr].com <= MAXCOMPORTS)
        error("File number");
    if (FileTable[fnbr].com == 0)
        error("File number #% is not open", fnbr);
    if (!InitSDCard())
        return;
    idx = getint(argv[2], 1, 0x7FFFFFFF) - 1;
    if (idx < 0)
        idx = 0;
    if(filesource[fnbr]==FLASHFILE){
        if(idx>FileTable[fnbr].lfsptr->ctz.size)idx=FileTable[fnbr].lfsptr->ctz.size;
        FSerror = lfs_file_seek(&lfs, FileTable[fnbr].lfsptr, idx, LFS_SEEK_SET);
        if(FSerror<0)ErrorCheck(fnbr);
    } else {
        if (fmode[fnbr] & FA_WRITE)
        {
            FSerror = f_lseek(FileTable[fnbr].fptr, idx);
            ErrorCheck(fnbr);
        }
        else
        {
            buff = SDbuffer[fnbr];
            FSerror = f_lseek(FileTable[fnbr].fptr, idx - (idx % 512));
            ErrorCheck(fnbr);
            FSerror = f_read(FileTable[fnbr].fptr, buff, SDbufferSize, &bw[fnbr]);
            ErrorCheck(fnbr);
            buffpointer[fnbr] = idx % 512;
            lastfptr[fnbr] = (uint32_t)FileTable[fnbr].fptr;
        }
    }
}

void cmd_name(void)
{
    char *old, *new, ss[2];
    int i;
    ss[0] = tokenAS; // this will be used to split up the argument line
    ss[1] = 0;
    char qold[FF_MAX_LFN]={0};
    char qnew[FF_MAX_LFN]={0};
    getargs(&cmdline, 3, ss);                                   // getargs macro must be the first executable stmt in a block
    if(argc != 3) error("Syntax");
    old = getCstring(argv[0]);                                  // get the old name
    if(drivecheck(old,&i)!=FatFSFileSystem+1) error("Only valid on current drive");
    getfullfilepath(old,qold);
    new = getCstring(argv[2]);                                  // get the new name
    if(drivecheck(new,&i)!=FatFSFileSystem+1) error("Only valid on current drive");
    getfullfilepath(new,qnew);
    if(!FatFSFileSystem){
    	// start a new block
        FSerror = lfs_rename(&lfs, qold, qnew);
        ErrorCheck(0);

    } else {                             // start a new block
       if (!InitSDCard()) return;
        FSerror = f_rename(qold, qnew);
        ErrorCheck(0);
    }
}

void cmd_save(void)
{
    int fnbr;
    unsigned char *pp, *flinebuf, *p; // get the file name and change to the directory
    int maxH = VRes;
    int maxW = HRes;
    if (!InitSDCard()) return;
    fnbr = FindFreeFileNbr();
    if ((p = checkstring(cmdline, "IMAGE")) != NULL)
    {
        unsigned int nbr;
        int i, x, y, w, h, filesize;
        unsigned char bmpfileheader[14] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0};
        unsigned char bmpinfoheader[40] = {40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 24, 0};
        unsigned char bmppad[3] = {0, 0, 0};
        getargs(&p, 9, ",");
        if (!InitSDCard())
            return;
        if ((void *)ReadBuffer == (void *)DisplayNotSet)
            error("SAVE IMAGE not available on this display");
        pp = getCstring(argv[0]);
        if (argc != 1 && argc != 9)
            error("Syntax");
        if (strchr(pp, '.') == NULL)
            strcat(pp, ".bmp");
        if (!BasicFileOpen(pp, fnbr, FA_WRITE | FA_CREATE_ALWAYS))
            return;
        if (argc == 1)
        {
            x = 0;
            y = 0;
            h = maxH;
            w = maxW;
        }
        else
        {
            x = getint(argv[2], 0, maxW - 1);
            y = getint(argv[4], 0, maxH - 1);
            w = getint(argv[6], 1, maxW - x);
            h = getint(argv[8], 1, maxH - y);
        }
        filesize = 54 + 3 * w * h;
        bmpfileheader[2] = (unsigned char)(filesize);
        bmpfileheader[3] = (unsigned char)(filesize >> 8);
        bmpfileheader[4] = (unsigned char)(filesize >> 16);
        bmpfileheader[5] = (unsigned char)(filesize >> 24);

        bmpinfoheader[4] = (unsigned char)(w);
        bmpinfoheader[5] = (unsigned char)(w >> 8);
        bmpinfoheader[6] = (unsigned char)(w >> 16);
        bmpinfoheader[7] = (unsigned char)(w >> 24);
        bmpinfoheader[8] = (unsigned char)(h);
        bmpinfoheader[9] = (unsigned char)(h >> 8);
        bmpinfoheader[10] = (unsigned char)(h >> 16);
        bmpinfoheader[11] = (unsigned char)(h >> 24);
        if(FatFSFileSystem) {
            f_write(FileTable[fnbr].fptr, bmpfileheader, 14, &nbr);
            f_write(FileTable[fnbr].fptr, bmpinfoheader, 40, &nbr);
        } else {
            FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, bmpfileheader, 14); 
            if(FSerror>0)FSerror=0;
            ErrorCheck(fnbr);
            FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, bmpinfoheader, 40); 
            if(FSerror>0)FSerror=0;
            ErrorCheck(fnbr);
        }
        flinebuf = GetTempMemory(maxW * 4);
        for (i = y + h - 1; i >= y; i--)
        {
            ReadBuffer(x, i, x + w - 1, i, flinebuf);
            if(FatFSFileSystem) f_write(FileTable[fnbr].fptr, flinebuf, w * 3, &nbr);
            else {
                    FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, flinebuf, w * 3); 
                    if(FSerror>0)FSerror=0;
                    ErrorCheck(fnbr);
            }
            if ((w * 3) % 4 != 0)
                if(FatFSFileSystem) f_write(FileTable[fnbr].fptr, bmppad, 4 - ((w * 3) % 4), &nbr);
                else {
                    FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, bmppad, 4 - ((w * 3) % 4)); 
                    if(FSerror>0)FSerror=0;
                    ErrorCheck(fnbr);
                }
        }
        FileClose(fnbr);
        return;
    }
    else
    {
        unsigned char b[STRINGSIZE];
        p = getCstring(cmdline); // get the file name and change to the directory
        if (strchr(p, '.') == NULL)
            strcat(p, ".bas");
        if (!BasicFileOpen(p, fnbr, FA_WRITE | FA_CREATE_ALWAYS))
            return;
        p = ProgMemory;
        while (!(*p == 0 || *p == 0xff))
        {                    // this is a safety precaution
            p = llist(b, p); // expand the line
            pp = b;
            while (*pp) FilePutChar(*pp++, fnbr); // write the line to the SD card
            FilePutChar('\r', fnbr);
            FilePutChar('\n', fnbr); // terminate the line
            if (p[0] == 0 && p[1] == 0) break; // end of the listing ?
        }
        FileClose(fnbr);
    }
}
// load a file into program memory
int FileLoadProgram(unsigned char *fname)
{
    int fnbr;
    char *p, *buf;
    int c;
    if (!InitSDCard()) return false;
    ClearProgram(); // clear any leftovers from the previous program
    fnbr = FindFreeFileNbr();
    p = getCstring(fname);
    if (strchr(p, '.') == NULL) strcat(p, ".bas");
    if (!BasicFileOpen(p, fnbr, FA_READ)) return false;
    p = buf = GetTempMemory(EDIT_BUFFER_SIZE-2048); // get all the memory while leaving space for the couple of buffers defined and the file handle
    while (!FileEOF(fnbr))
    { // while waiting for the end of file
        if ((p - buf) >= EDIT_BUFFER_SIZE - 512)
            error("Not enough memory");
        c = FileGetChar(fnbr) & 0x7f;
        if (isprint(c) || c == '\r' || c == '\n' || c == TAB)
        {
            if (c == TAB)
                c = ' ';
            *p++ = c; // get the input into RAM
        }
    }
    *p = 0; // terminate the string in RAM
    FileClose(fnbr);
    ClearSavedVars(); // clear any saved variables
    SaveProgramToFlash(buf, false);
    return true;
}

void cmd_load(void)
{
    int autorun = false;
    unsigned char *p;

    p = checkstring(cmdline, "IMAGE");
    if (p)
    {
        LoadImage(p);
        return;
    }
    p = checkstring(cmdline, "JPG");
    if (p)
    {
        LoadJPGImage(p);
        return;
    }

    getargs(&cmdline, 3, ",");
    if (!(argc & 1) || argc == 0)
        error("Syntax");
    if (argc == 3)
    {
        if (toupper(*argv[2]) == 'R')
            autorun = true;
        else
            error("Syntax");
    }
    else if (CurrentLinePtr != NULL)
        error("Invalid in a program");

    if (!FileLoadProgram(argv[0]))
        return;
    FlashLoad = 0;
    if (autorun)
    {
        if (*ProgMemory != 0x01)
            return; // no program to run
        ClearRuntime();
        WatchdogSet = false;
        PrepareProgram(true);
        IgnorePIN = false;
        nextstmt = ProgMemory;
    }
}

char __not_in_flash_func(FileGetChar)(int fnbr)
{
    char ch;
    if(filesource[fnbr]==FLASHFILE){
        FSerror=lfs_file_read(&lfs, FileTable[fnbr].lfsptr, &ch, 1);
        if(FSerror>0)FSerror=0;
        ErrorCheck(fnbr);
        return ch;
    } else {
        char *buff = SDbuffer[fnbr];
        if (!InitSDCard())
            return 0;
        if (fmode[fnbr] & FA_WRITE)
        {
            FSerror = f_read(FileTable[fnbr].fptr, &ch, 1, &bw[fnbr]);
            ErrorCheck(fnbr);
        }
        else
        {
            if (!(lastfptr[fnbr] == (uint32_t)FileTable[fnbr].fptr && buffpointer[fnbr] < SDbufferSize))
            {
                FSerror = f_read(FileTable[fnbr].fptr, buff, SDbufferSize, &bw[fnbr]);
                ErrorCheck(fnbr);
                buffpointer[fnbr] = 0;
                lastfptr[fnbr] = (uint32_t)FileTable[fnbr].fptr;
            }
            ch = buff[buffpointer[fnbr]];
            buffpointer[fnbr]++;
        }
        diskchecktimer = DISKCHECKRATE;
    return ch;
    }
}

char __not_in_flash_func(FilePutChar)(char c, int fnbr)
{
    if(filesource[fnbr]==FLASHFILE){
        FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, &c, 1);
        if(FSerror!=1)FSerror=-5;
        if(FSerror>0)FSerror=0;
        ErrorCheck(fnbr);
        return c;
    } else {
        static char t;
        unsigned int bw;
        t = c;
        if (!InitSDCard())
            return 0;
        FSerror = f_write(FileTable[fnbr].fptr, &t, 1, &bw);
        lastfptr[fnbr] = -1; // invalidate the read file buffer
        ErrorCheck(fnbr);
        diskchecktimer = DISKCHECKRATE;
        return t;
    }
}
int FileEOF(int fnbr)
{
    int i;
    if(filesource[fnbr]==FATFSFILE){
        if (!InitSDCard())
            return 0;
        if (buffpointer[fnbr] <= bw[fnbr] - 1)
            i = 0;
        else
        {
            i = f_eof(FileTable[fnbr].fptr);
        }
    } else {
        i = (lfs_file_tell(&lfs,FileTable[fnbr].lfsptr)==lfs_file_size(&lfs,FileTable[fnbr].lfsptr));
    }
    return i;
}
// send a character to a file or the console
// if fnbr == 0 then send the char to the console
// otherwise the COM port or file opened as #fnbr
unsigned char MMfputc(unsigned char c, int fnbr)
{
    if (fnbr == 0)
        return MMputchar(c, 1); // accessing the console
    if (fnbr < 1 || fnbr > MAXOPENFILES)
        error("File number");
    if (FileTable[fnbr].com == 0)
        error("File number is not open");
    if (FileTable[fnbr].com > MAXCOMPORTS)
        return FilePutChar(c, fnbr);
    else
        return SerialPutchar(FileTable[fnbr].com, c); // send the char to the serial port
}
int MMfgetc(int fnbr)
{
    int ch;
    if (fnbr == 0)
        return MMgetchar(); // accessing the console
    if (fnbr < 1 || fnbr > MAXOPENFILES)
        error("File number");
    if (FileTable[fnbr].com == 0)
        error("File number is not open");
    if (FileTable[fnbr].com > MAXCOMPORTS)
        ch = FileGetChar(fnbr);
    else
        ch = SerialGetchar(FileTable[fnbr].com); // get the char from the serial port
    return ch;
}

int MMfeof(int fnbr)
{
    if (fnbr == 0)
        return (kbhitConsole() == 0); // accessing the console
    if (fnbr < 1 || fnbr > MAXOPENFILES)
        error("File number");
    if (FileTable[fnbr].com == 0)
        error("File number is not open");
    if (FileTable[fnbr].com > MAXCOMPORTS)
        return FileEOF(fnbr);
    else
        return SerialRxStatus(FileTable[fnbr].com) == 0;
}
// close the file and free up the file handle
//  it will generate an error if needed
void FileClose(int fnbr)
{
    int type=ForceFileClose(fnbr);
    ErrorThrow(FSerror, type);
}

// close the file and free up the file handle
//  it will NOT generate an error
int ForceFileClose(int fnbr)
{
    FatFSFileSystem = FatFSFileSystemSave;
    int type=NONEFILE;
    if (fnbr && FileTable[fnbr].fptr != NULL && filesource[fnbr]==FATFSFILE)
    {
        
        FSerror = f_close(FileTable[fnbr].fptr);
        FreeMemory((void *)FileTable[fnbr].fptr);
        FreeMemory((void *)SDbuffer[fnbr]);
        FileTable[fnbr].fptr = NULL;
        type=FATFSFILE;
    } else {
        if(FileTable[fnbr].lfsptr != NULL){
        FSerror = lfs_file_close(&lfs, FileTable[fnbr].lfsptr);
        FreeMemory((void *)FileTable[fnbr].lfsptr);
        FileTable[fnbr].lfsptr = NULL;
        }
        type=FLASHFILE;
    }
    buffpointer[fnbr] = 0;
    lastfptr[fnbr] = -1;
    bw[fnbr] = -1;
    fmode[fnbr] = 0;
    filesource[fnbr] = NONEFILE;
    return type;
}
// finds the first available free file number.  Throws an error if no free file numbers
int FindFreeFileNbr(void)
{
    int i;
    for (i = MAXOPENFILES; i >= 1; i--)
        if (FileTable[i].com == 0)
            return i;
    error("Too many files open");
    return 0;
}

void CloseAllFiles(void)
{
    int i;
#ifdef PICOMITEVGA
    closeall3d();
    closeallsprites();
    closeframebuffer();
#endif
    for (i = 1; i <= MAXOPENFILES; i++)
    {
        if (FileTable[i].com != 0)
        {
            if (FileTable[i].com > MAXCOMPORTS)
            {
                ForceFileClose(i);
            }
            else
                SerialClose(FileTable[i].com);
            FileTable[i].com = 0;
        }
    }
}

void FilePutStr(int count, char *c, int fnbr)
{
   if(filesource[fnbr]==FLASHFILE){
        int err;
        FSerror=lfs_file_write(&lfs, FileTable[fnbr].lfsptr, c, count);
        if(FSerror!=count)FSerror=-5;
        if(FSerror>0)FSerror=0;
        ErrorCheck(fnbr);
    } else {
        unsigned int bw;
        InitSDCard();
        FSerror = f_write(FileTable[fnbr].fptr, c, count, &bw);
        ErrorCheck(fnbr);
        diskchecktimer = DISKCHECKRATE;
    }
}

// output a string to a file
// the string must be a MMBasic string
void MMfputs(unsigned char *p, int filenbr)
{
    int i;
    i = *p++;
    if (FileTable[filenbr].com > MAXCOMPORTS)
    {
        FilePutStr(i, p, filenbr);
    }
    else
    {
        while (i--)
            MMfputc(*p++, filenbr);
    }
}

// this is invoked as a command (ie, date$ = "6/7/2010")
// search through the line looking for the equals sign and step over it,
// evaluate the rest of the command, split it up and save in the system counters
int InitSDCard(void)
{
    if(!FatFSFileSystem) return 1;
    int i;
    ErrorThrow(0,NONEFILE); // reset mm.errno to zero
    if ((IsInvalidPin(Option.SD_CS) || (IsInvalidPin(Option.SYSTEM_MOSI) && IsInvalidPin(Option.SD_MOSI_PIN)) || (IsInvalidPin(Option.SYSTEM_MISO) && IsInvalidPin(Option.SD_MISO_PIN)) || (IsInvalidPin(Option.SYSTEM_CLK) && IsInvalidPin(Option.SD_CLK_PIN))))
        error("SDcard not configured");
    if (!(SDCardStat & STA_NOINIT))
        return 1; // if the card is present and has been initialised we have nothing to do
    for (i = 0; i < MAXOPENFILES; i++)
        if (FileTable[i].com > MAXCOMPORTS)
            if (FileTable[i].fptr != NULL)
                ForceFileClose(i);
    i = f_mount(&FatFs, "", 1);
    if (i)
    {
        FatFSFileSystem=0;
        ErrorThrow(i,FATFSFILE);
        return false;
    }
    return 2;
}
void getfullfilename(char *fname, char *q){
    int waste=0, t=FatFSFileSystem+1;
    if(*cmdline){
        t = drivecheck(fname,&waste);
        fname+=waste;
    }
    FatFSFileSystem=t-1;
    char pp[FF_MAX_LFN] = {0};
    char *p=fname;
    int i,j;
    i=strlen(p)-1;
    while(i>0 && !(p[i] == 92 || p[i]==47))i--;
    if(i>0){
        memcpy(q,p,i);
            for(j=0;j<strlen(q);j++)if(q[j]=='\\')q[j]='/';  //allow backslash for the DOS oldies
        if(q[1]==':')q[0]='0';
        i++;
    }
    strcpy(pp,&p[i]);
    if((pp[0]==47 || pp[0]==92) && i==0){
        strcpy(q,&pp[1]);
        strcpy(pp,q);
        strcpy(q,"0:/");
    }
    fullpath(q);
//       	MMPrintString("Was: ");MMPrintString(fname);PRet();
//      	MMPrintString("Path: ");MMPrintString(filepath[FatFSFileSystem]);PRet();
//       	MMPrintString("File: ");MMPrintString(pp);PRet();
    strcpy(q,fullpathname[FatFSFileSystem]);
    if(fullpathname[FatFSFileSystem][strlen(fullpathname[FatFSFileSystem])-1]!='/')strcat(q,"/");
    strcat(q,pp);
//       	MMPrintString("Full: ");MMPrintString(q);PRet();

}
// this performs the basic duties of opening a file, all file opens in MMBasic should use this
// it will open the file, set the FileTable[] entry and populate the file descriptor
// it returns with true if successful or false if an error
int BasicFileOpen(char *fname, int fnbr, int mode)
{
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com != 0) error("File number already open");
    char q[FF_MAX_LFN]={0};
    getfullfilename(fname,q);
    if(FatFSFileSystem){
        if (!InitSDCard())
            return false;
        // if we are writing check the write protect pin (negative pin number means that low = write protect)
        FileTable[fnbr].fptr = GetMemory(sizeof(FIL)); // allocate the file descriptor
        SDbuffer[fnbr] = GetMemory(SDbufferSize);
        FSerror = f_open(FileTable[fnbr].fptr, q, mode); // open it
        ErrorCheck(fnbr);
        filesource[fnbr] = FATFSFILE;
        buffpointer[fnbr] = 0;
        lastfptr[fnbr] = -1;
        bw[fnbr] = -1;
        fmode[fnbr] = mode;

    } else {
        int lfsmode=0;
        if(mode == FA_READ)lfsmode=LFS_O_RDONLY;
        else if(mode==(FA_WRITE | FA_CREATE_ALWAYS))lfsmode=LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
        else if(mode==(FA_WRITE | FA_OPEN_APPEND)) lfsmode = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
        else if(mode==(FA_WRITE | FA_OPEN_APPEND | FA_READ))lfsmode =LFS_O_RDWR | LFS_O_CREAT | LFS_O_APPEND;
        else error("Internal error");
        FileTable[fnbr].lfsptr = GetMemory(sizeof(lfs_file_t)); // allocate the file descriptor
        FSerror=lfs_file_open(&lfs, FileTable[fnbr].lfsptr, q, lfsmode);
        if(mode!=LFS_O_RDONLY){
            int dt=get_fattime();
            FSerror=lfs_setattr(&lfs, q, 'A', &dt,   4);
            ErrorCheck(0);
        }
	    ErrorCheck(fnbr);
        filesource[fnbr] = FLASHFILE;
    }

    if (FSerror)
    {
        ForceFileClose(fnbr);
        return false;
    }
    else {
        FatFSFileSystem=FatFSFileSystemSave;
        return true;
    }
}

#define MAXFILES 1000
typedef struct ss_flist
{
    char fn[FF_MAX_LFN];
    int fs; // file size
    int fd; // file date
    int ft; // file time
} s_flist;

int strcicmp(char const *a, char const *b)
{
    for (;; a++, b++)
    {
        int d = tolower(*a) - tolower(*b);
        if (d != 0 || !*a)
            return d;
    }
}
void B2A(unsigned char *tp){
    char ss[2]; // this will be used to split up the argument line
    char *fromfile, *tofile;
    ss[0] = tokenTO;
    ss[1] = 0;
    char buff[512];
    unsigned int nbr = 0, bw;
    int fnbr1, fnbr2, err;
    getargs(&tp, 3, ss);
    if (argc != 3)
        error("Syntax");
    fnbr1 = FindFreeFileNbr();
    fromfile = getCstring(argv[0]);
    tofile = getCstring(argv[2]);
    FatFSFileSystem=1; //set to SD
    BasicFileOpen(fromfile, fnbr1, FA_READ);
    fnbr2 = FindFreeFileNbr();
    FatFSFileSystem=0; //set to flash
    if (!BasicFileOpen(tofile, fnbr2, FA_WRITE | FA_CREATE_ALWAYS))
    {
        FileClose(fnbr1);
    }
    while (!f_eof(FileTable[fnbr1].fptr))
    {
        FSerror = f_read(FileTable[fnbr1].fptr, buff, 512, &nbr);
        ErrorCheck(fnbr1);
        FSerror=lfs_file_write(&lfs, FileTable[fnbr2].lfsptr, buff, nbr); 
        if(FSerror>0)FSerror=0;
        ErrorCheck(fnbr2);

    }
    FileClose(fnbr1);
    FileClose(fnbr2);
    FatFSFileSystem=FatFSFileSystemSave;
}
void A2B(unsigned char *tp){
    char ss[2]; // this will be used to split up the argument line
    char *fromfile, *tofile;
    ss[0] = tokenTO;
    ss[1] = 0;
    char buff[512];
    unsigned int nbr = 0, bw;
    int fnbr1, fnbr2, err;
    getargs(&tp, 3, ss);
    if (argc != 3) error("Syntax");
    FatFSFileSystem=0; //set to flash
    fnbr1 = FindFreeFileNbr();
    fromfile = getCstring(argv[0]);
    tofile = getCstring(argv[2]);
    BasicFileOpen(fromfile, fnbr1, FA_READ);
    fnbr2 = FindFreeFileNbr();
    FatFSFileSystem=1; //set to SD
    if (!BasicFileOpen(tofile, fnbr2, FA_WRITE | FA_CREATE_ALWAYS))
    {
        FileClose(fnbr1);
    }
    while (!(lfs_file_tell(&lfs,FileTable[fnbr1].lfsptr)==lfs_file_size(&lfs,FileTable[fnbr1].lfsptr)))
    {
        nbr=lfs_file_read(&lfs, FileTable[fnbr1].lfsptr, buff, 512);
        if(nbr<0)FSerror=nbr;	
        ErrorCheck(fnbr1);
        FSerror = f_write(FileTable[fnbr2].fptr, buff, nbr, &bw);
        ErrorCheck(fnbr2);
    }
    FileClose(fnbr1);
    FileClose(fnbr2);
    FatFSFileSystem=FatFSFileSystemSave;
}
void B2B(unsigned char *tp){
    char ss[2]; // this will be used to split up the argument line
    char *fromfile, *tofile;
    ss[0] = tokenTO;
    ss[1] = 0;
    char buff[512];
    unsigned int nbr = 0, bw;
    int fnbr1, fnbr2, err;
    getargs(&tp, 3, ss);
    if (argc != 3)
        error("Syntax");
    fnbr1 = FindFreeFileNbr();
    fromfile = getCstring(argv[0]);
    tofile = getCstring(argv[2]);
    FatFSFileSystem=1; //set to SD
    BasicFileOpen(fromfile, fnbr1, FA_READ);
    fnbr2 = FindFreeFileNbr();
    if (!BasicFileOpen(tofile, fnbr2, FA_WRITE | FA_CREATE_ALWAYS))
    {
        FileClose(fnbr1);
    }
    while (!f_eof(FileTable[fnbr1].fptr))
    {
        FSerror = f_read(FileTable[fnbr1].fptr, buff, 512, &nbr);
        ErrorCheck(fnbr1);
        FSerror = f_write(FileTable[fnbr2].fptr, buff, nbr, &bw);
        ErrorCheck(fnbr2);
    }
    FileClose(fnbr1);
    FileClose(fnbr2);
    FatFSFileSystem=FatFSFileSystemSave;
}
void A2A(unsigned char *tp){
    char ss[2]; // this will be used to split up the argument line
    char *fromfile, *tofile;
    ss[0] = tokenTO;
    ss[1] = 0;
    char buff[512];
    unsigned int nbr = 0, bw;
    int fnbr1, fnbr2, err;
    getargs(&tp, 3, ss);
    if (argc != 3)
        error("Syntax");
    fnbr1 = FindFreeFileNbr();
    fromfile = getCstring(argv[0]);
    tofile = getCstring(argv[2]);
    FatFSFileSystem=0; //set to FLASH
    BasicFileOpen(fromfile, fnbr1, FA_READ);
    fnbr2 = FindFreeFileNbr();
    if (!BasicFileOpen(tofile, fnbr2, FA_WRITE | FA_CREATE_ALWAYS))
    {
        FileClose(fnbr1);
    }
    while (!(lfs_file_tell(&lfs,FileTable[fnbr1].lfsptr)==lfs_file_size(&lfs,FileTable[fnbr1].lfsptr)))
    {
        nbr=lfs_file_read(&lfs, FileTable[fnbr1].lfsptr, buff, 512);
        if(nbr<0)FSerror=nbr;	
        ErrorCheck(fnbr1);
        FSerror=lfs_file_write(&lfs, FileTable[fnbr2].lfsptr, buff, nbr); 
        if(FSerror>0)FSerror=0;
        ErrorCheck(fnbr2);
    }
    FileClose(fnbr1);
    FileClose(fnbr2);
    FatFSFileSystem=FatFSFileSystemSave;
}
int drivecheck(char *p, int *waste){
    *waste=0;
    if(strlen(p)==2){
        if(!(p[1]==':')) return FatFSFileSystem+1;
        if(*p=='a' || *p=='A') {
            *waste=2;
            return FLASHFILE;
        }
        if(*p=='b' || *p=='B') {
            *waste=2;
             return FATFSFILE;
        }
        return FatFSFileSystem+1;
    } else  {
        if(!(p[1]==':' && (p[2]==47 || p[2]==92))) return FatFSFileSystem+1;
        if(*p=='a' || *p=='A') {
            *waste=2;
            return FLASHFILE;
        }
        if(*p=='b' || *p=='B') {
            *waste=2;
             return FATFSFILE;
        }
        return FatFSFileSystem+1;
    }
}
void cmd_copy(void)
{
    unsigned char *p=GetTempMemory(STRINGSIZE);
    memcpy(p,cmdline,STRINGSIZE);
    char ss[2]; // this will be used to split up the argument line
    char *fromfile, *tofile;
    ss[0] = tokenTO;
    ss[1] = 0;
    int waste;
    unsigned char *tp = checkstring(cmdline, "B2A");
    if(tp){
        B2A(tp);
        return;
    }        
    tp = checkstring(cmdline, "A2B");
    if(tp){
        A2B(tp);
        return;
    }
    tp = checkstring(cmdline, "A2A");
    if(tp){
        A2A(tp);
        return;
    }
    tp = checkstring(cmdline, "B2B");
    if(tp){
        B2B(tp);
        return;
    }
    getargs(&p, 3, ss);
    if (argc != 3) error("Syntax");
    fromfile = getCstring(argv[0]);
    tofile = getCstring(argv[2]);

    if(drivecheck(fromfile, &waste)==FLASHFILE && drivecheck(tofile, &waste)==FLASHFILE){
        A2A(cmdline);
        return;
    }
    if(drivecheck(fromfile, &waste)==FATFSFILE && drivecheck(tofile, &waste)==FATFSFILE){
        B2B(cmdline);
        return;
    }
    if(drivecheck(fromfile, &waste)==FLASHFILE && drivecheck(tofile, &waste)==FATFSFILE){
        A2B(cmdline);
        return;
    }
    if(drivecheck(fromfile, &waste)==FATFSFILE && drivecheck(tofile, &waste)==FLASHFILE){
        B2A(cmdline);
        return;
    }
}

int resolve_path(char *path, char *result, char *pos)
{
    if (*path == '/')
    {
        *result = '/';
        pos = result + 1;
        path++;
    }
    *pos = 0;
    if (!*path)
        return 0;
    while (1)
    {
        char *slash;
        struct stat st;
        st.st_mode = 0;
        slash = *path ? strchr(path, '/') : NULL;
        if (slash)
            *slash = 0;
        if (!path[0] || (path[0] == '.' &&
                         (!path[1] || (path[1] == '.' && !path[2]))))
        {
            pos--;
            if (pos != result && path[0] && path[1])
                while (*--pos != '/')
                    ;
        }
        else
        {
            strcpy(pos, path);
            //	    if (lstat(result,&st) < 0) return -1;
            if (S_ISLNK(st.st_mode))
            {
                char buf[PATH_MAX];
                //		if (readlink(result,buf,sizeof(buf)) < 0) return -1;
                *pos = 0;
                if (slash)
                {
                    *slash = '/';
                    strcat(buf, slash);
                }
                strcpy(path, buf);
                if (*path == '/')
                    result[1] = 0;
                pos = strchr(result, 0);
                continue;
            }
            pos = strchr(result, 0);
        }
        if (slash)
        {
            *pos++ = '/';
            path = slash + 1;
        }
        *pos = 0;
        if (!slash)
            break;
    }
    return 0;
}

void fullpath(char *q)
{
    char *p = GetTempMemory(STRINGSIZE);
    char *rp = GetTempMemory(STRINGSIZE);
    int i;
    strcpy(p, q);
    memset(fullpathname[FatFSFileSystem], 0, sizeof(fullpathname[FatFSFileSystem]));
    strcpy(fullpathname[FatFSFileSystem], filepath[FatFSFileSystem]);
    for (i = 0; i < strlen(p); i++)
        if (p[i] == '\\')
            p[i] = '/'; // allow backslash for the DOS oldies
    if (strcmp(p, ".") == 0 || strlen(p) == 0)
    {
        memmove(fullpathname[FatFSFileSystem], &fullpathname[FatFSFileSystem][2], strlen(fullpathname[FatFSFileSystem]));
        //    	MMPrintString("Now: ");MMPrintString(fullpathname);PRet();
        return; // nothing to do
    }
    if (p[1] == ':')
    { // modify the requested path so that if the disk is specified the pathname is absolute and starts with /
        if (p[2] == '/')
            p += 2;
        else
        {
            p[1] = '/';
            p++;
        }
    }
    if (*p == '/')
    { // absolute path specified
        strcpy(rp, FatFSFileSystem==0?"A:":"B:");
        strcat(rp, p);
    }
    else
    {                             // relative path specified
        strcpy(rp, fullpathname[FatFSFileSystem]); // copy the current pathname
        if (rp[strlen(rp) - 1] != '/')
            strcat(rp, "/"); // make sure the previous pathname ends in slash, will only be the case at root
        strcat(rp, p);       // append the new pathname
    }
    strcpy(fullpathname[FatFSFileSystem], rp);           // set the new pathname
    resolve_path(fullpathname[FatFSFileSystem], rp, rp); // resolve to single absolute path
    if (strcmp(rp, "A:") == 0 || strcmp(rp, "0:") == 0 || strcmp(rp, "B:") == 0)
        strcat(rp, "/");      // if root append the slash
    strcpy(fullpathname[FatFSFileSystem], rp); // store this back to the filepath variable
    memmove(fullpathname[FatFSFileSystem], &fullpathname[FatFSFileSystem][2], strlen(fullpathname[FatFSFileSystem]));
//    MMPrintString("Now: ");MMPrintString(fullpathname[FatFSFileSystem]);PRet();
}
void getfullpath(char *p, char *q){
	int j;
    strcpy(q,p);
    for(j=0;j<strlen(q);j++)if(q[j]=='\\')q[j]='/';  //allow backslash for the DOS oldies
    if(q[1]==':')q[0]='0';
    fullpath(q);
    strcpy(q,fullpathname[FatFSFileSystem]);
}
void getfullfilepath(char *p, char *q){
	int i,j;
    char pp[FF_MAX_LFN] = {0};
    i=strlen(p)-1;
    while(i>0 && !(p[i] == 92 || p[i]==47))i--;
    if(i>0){
    	memcpy(q,p,i);
    	 for(j=0;j<strlen(q);j++)if(q[j]=='\\')q[j]='/';  //allow backslash for the DOS oldies
    	if(q[1]==':')q[0]='0';
    	i++;
    }
    strcpy(pp,&p[i]);
    if((pp[0]==47 || pp[0]==92) && i==0){
    	strcpy(q,&pp[1]);
    	strcpy(pp,q);
    	strcpy(q,"0:/");
    }
    fullpath(q);
    strcpy(q,fullpathname[FatFSFileSystem]);
    if(q[strlen(q)-1]!=47)strcat(q,"/");
    strcat(q,pp);
}

void cmd_files(void)
{
    if(CurrentLinePtr) error("Invalid in a program");
    int waste=0, t=FatFSFileSystem+1;
    if(*cmdline){
        t = drivecheck(getCstring(cmdline),&waste);
        cmdline+=waste;
        *cmdline='"';
    }
    ClearVars(0);
    int i, j, c, dirs, ListCnt, currentsize;
    uint32_t currentdate;
    char *p, extension[8];
    int fcnt, sortorder = 0;
    char ts[FF_MAX_LFN] = {0};
    char pp[FF_MAX_LFN] = {0};
    char q[FF_MAX_LFN] = {0};
    static s_flist *flist = NULL;
    DIR djd;
    FILINFO fnod;
    memset(&djd, 0, sizeof(DIR));
    memset(&fnod, 0, sizeof(FILINFO));
    fcnt = 0;
    if (*cmdline)
    {
        getargs(&cmdline, 3, ",");
        if (!(argc == 1 || argc == 3))
            error("Syntax");
        p = getCstring(argv[0]);
        i = strlen(p) - 1;
        while (i > 0 && !(p[i] == 92 || p[i] == 47))
            i--;
        if (i > 0)
        {
            memcpy(q, p, i);
            for (j = 0; j < strlen(q); j++)
                if (q[j] == '\\')
                    q[j] = '/'; // allow backslash for the DOS oldies
            if (q[1] == ':')
                q[0] = '0';
            i++;
        }
        strcpy(pp, &p[i]);
        if ((pp[0] == 47 || pp[0] == 92) && i == 0)
        {
            strcpy(q, &pp[1]);
            strcpy(pp, q);
            strcpy(q, "0:/");
        }
        if (argc == 3)
        {
            if (checkstring(argv[2], "NAME"))
                sortorder = 0;
            else if (checkstring(argv[2], "TIME"))
                sortorder = 1;
            else if (checkstring(argv[2], "SIZE"))
                sortorder = 2;
            else if (checkstring(argv[2], "TYPE"))
                sortorder = 3;
            else
                error("Syntax");
        }
    }
    if (pp[0] == 0)
        strcpy(pp, "*");
    if (CurrentLinePtr)
        error("Invalid in a program");
    if (flist)
        FreeMemorySafe((void **)&flist);
#ifdef PICOMITEVGA
    closeallsprites();
    closeframebuffer();
    closeall3d();
#endif
    CloseAudio(1);
    flist = GetMemory(sizeof(s_flist) * MAXFILES);
    FatFSFileSystem=t-1;
    if (!InitSDCard())
        error((char *)FErrorMsg[20]); // setup the SD card
    FatFSFileSystem=t-1;
    fullpath(q);
    putConsole('A'+FatFSFileSystem,0);
    putConsole(':',1);
    MMPrintString(fullpathname[FatFSFileSystem]);PRet();
    if(FatFSFileSystem==0) FSerror=lfs_dir_open(&lfs, &lfs_dir, fullpathname[FatFSFileSystem]);
    else FSerror = f_findfirst(&djd, &fnod, fullpathname[FatFSFileSystem], pp);
    ErrorCheck(0);
        // add the file to the list, search for the next and keep looping until no more files
        if(FatFSFileSystem){
            while (FSerror == FR_OK && fnod.fname[0])
            {
                if (fcnt >= MAXFILES)
                {
                    FreeMemorySafe((void **)&flist);
                    f_closedir(&djd);
                    error("Too many files to list");
                }
                if (!(fnod.fattrib & (AM_SYS | AM_HID)))
                {
                    // add a prefix to each line so that directories will sort ahead of files
                    if (fnod.fattrib & AM_DIR)
                    {
                        ts[0] = 'D';
                        currentdate = 0xFFFFFFFF;
                        fnod.fdate = 0xFFFF;
                        fnod.ftime = 0xFFFF;
                        memset(extension, '+', sizeof(extension));
                        extension[sizeof(extension) - 1] = 0;
                    }
                    else
                    {
                        ts[0] = 'F';
                        currentdate = (fnod.fdate << 16) | fnod.ftime;
                        if (fnod.fname[strlen(fnod.fname) - 1] == '.')
                            strcpy(extension, &fnod.fname[strlen(fnod.fname) - 1]);
                        else if (fnod.fname[strlen(fnod.fname) - 2] == '.')
                            strcpy(extension, &fnod.fname[strlen(fnod.fname) - 2]);
                        else if (fnod.fname[strlen(fnod.fname) - 3] == '.')
                            strcpy(extension, &fnod.fname[strlen(fnod.fname) - 3]);
                        else if (fnod.fname[strlen(fnod.fname) - 4] == '.')
                            strcpy(extension, &fnod.fname[strlen(fnod.fname) - 4]);
                        else if (fnod.fname[strlen(fnod.fname) - 5] == '.')
                            strcpy(extension, &fnod.fname[strlen(fnod.fname) - 5]);
                        else
                        {
                            memset(extension, '.', sizeof(extension));
                            extension[sizeof(extension) - 1] = 0;
                        }
                    }
                    currentsize = fnod.fsize;
                    // and concatenate the filename found
                    strcpy(&ts[1], fnod.fname);
                    // sort the file name into place in the array
                    if (sortorder == 0)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if (strcicmp((flist[i - 1].fn), (ts)) > 0)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else if (sortorder == 2)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if ((flist[i - 1].fs) > currentsize)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else if (sortorder == 3)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            char e2[8];
                            if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 1] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 1]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 2] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 2]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 3] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 3]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 4] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 4]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 5] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 5]);
                            else
                            {
                                if (flist[i - 1].fn[0] == 'D')
                                {
                                    memset(e2, '+', sizeof(e2));
                                    e2[sizeof(e2) - 1] = 0;
                                }
                                else
                                {
                                    memset(e2, '.', sizeof(e2));
                                    e2[sizeof(e2) - 1] = 0;
                                }
                            }
                            if (strcicmp((e2), (extension)) > 0)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if (((flist[i - 1].fd << 16) | flist[i - 1].ft) < currentdate)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    strcpy(flist[i].fn, ts);
                    flist[i].fs = fnod.fsize;
                    flist[i].fd = fnod.fdate;
                    flist[i].ft = fnod.ftime;
                    fcnt++;
                }
            FSerror = f_findnext(&djd, &fnod);
            } 
        } else {
            while(1){
                int found=0;
                FSerror=lfs_dir_read(&lfs, &lfs_dir, &lfs_info);
                if(FSerror==0)break;
//                if(!lfs_info.type){
//                    FSerror=lfs_dir_close(&lfs, &lfs_dir);	ErrorCheck(0);
//                    break;
//                }
                if(FSerror<0)ErrorCheck(0);
                if (lfs_info.type==LFS_TYPE_DIR && pattern_matching(pp, lfs_info.name, 0, 0))
                {
                    ts[0] = 'D';
                    currentdate = 0xFFFFFFFF;
                    memset(extension, '+', sizeof(extension));
                    fnod.fdate = 0xFFFF;
                    fnod.ftime = 0xFFFF;
                    extension[sizeof(extension) - 1] = 0;
                    found=1;
                }
                else if (lfs_info.type==LFS_TYPE_REG && pattern_matching(pp, lfs_info.name, 0, 0))
                {
                    ts[0] = 'F';
                    if (lfs_info.name[strlen(lfs_info.name) - 1] == '.')
                        strcpy(extension, &lfs_info.name[strlen(lfs_info.name) - 1]);
                    else if (lfs_info.name[strlen(lfs_info.name) - 2] == '.')
                        strcpy(extension, &lfs_info.name[strlen(lfs_info.name) - 2]);
                    else if (lfs_info.name[strlen(lfs_info.name) - 3] == '.')
                        strcpy(extension, &lfs_info.name[strlen(lfs_info.name) - 3]);
                    else if (lfs_info.name[strlen(lfs_info.name) - 4] == '.')
                        strcpy(extension, &lfs_info.name[strlen(lfs_info.name) - 4]);
                    else if (lfs_info.name[strlen(lfs_info.name) - 5] == '.')
                        strcpy(extension, &lfs_info.name[strlen(lfs_info.name) - 5]);
                    else
                    {
                        memset(extension, '.', sizeof(extension));
                        extension[sizeof(extension) - 1] = 0;
                    }
                    found=1;
                }
                if(found){
                    currentsize = lfs_info.size;
                    // and concatenate the filename found
                    strcpy(&ts[1], lfs_info.name);
                    int dt;
                    FSerror=lfs_getattr(&lfs, lfs_info.name, 'A', &dt,    4);
                    if(FSerror!=4){
                        fnod.fdate=0;
                        fnod.ftime=0;
                    } else {
                        WORD *p=(WORD *)&dt;
                        fnod.fdate=(WORD)p[1];
                        fnod.ftime=(WORD)p[0];
                    }
                    currentdate = (fnod.fdate << 16) | fnod.ftime;
                     // sort the file name into place in the array
                    if (sortorder == 0)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if (strcicmp((flist[i - 1].fn), (ts)) > 0)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else if (sortorder == 2)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if ((flist[i - 1].fs) > currentsize)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else if (sortorder == 3)
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            char e2[8];
                            if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 1] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 1]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 2] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 2]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 3] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 3]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 4] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 4]);
                            else if (flist[i - 1].fn[strlen(flist[i - 1].fn) - 5] == '.')
                                strcpy(e2, &flist[i - 1].fn[strlen(flist[i - 1].fn) - 5]);
                            else
                            {
                                if (flist[i - 1].fn[0] == 'D')
                                {
                                    memset(e2, '+', sizeof(e2));
                                    e2[sizeof(e2) - 1] = 0;
                                }
                                else
                                {
                                    memset(e2, '.', sizeof(e2));
                                    e2[sizeof(e2) - 1] = 0;
                                }
                            }
                            if (strcicmp((e2), (extension)) > 0)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    else
                    {
                        for (i = fcnt; i > 0; i--)
                        {
                            if (((flist[i - 1].fd << 16) | flist[i - 1].ft) < currentdate)
                                flist[i] = flist[i - 1];
                            else
                                break;
                        }
                    }
                    strcpy(flist[i].fn, ts);
                    flist[i].fs = lfs_info.size;
                    flist[i].fd = fnod.fdate;
                    flist[i].ft = fnod.ftime;
                    fcnt++;
                }
            }
        }
       // list the files with a pause every screen full
        ListCnt = 2;
        for (i = dirs = 0; i < fcnt; i++)
        {
            routinechecks();
            if (MMAbort)
            {
                FreeMemorySafe((void **)&flist);
                if(FatFSFileSystem) f_closedir(&djd);
                else lfs_dir_close(&lfs, &lfs_dir);
                WDTimer = 0; // turn off the watchdog timer
                memset(inpbuf, 0, STRINGSIZE);
                FatFSFileSystem=FatFSFileSystemSave;
                longjmp(mark, 1);
            }
            if (flist[i].fn[0] == 'D')
            {
                dirs++;
                MMPrintString("   <DIR>  ");
            }
            else
            {
                IntToStrPad(ts, (flist[i].ft >> 11) & 0x1F, '0', 2, 10);
                ts[2] = ':';
                IntToStrPad(ts + 3, (flist[i].ft >> 5) & 0x3F, '0', 2, 10);
                ts[5] = ' ';
                IntToStrPad(ts + 6, flist[i].fd & 0x1F, '0', 2, 10);
                ts[8] = '-';
                IntToStrPad(ts + 9, (flist[i].fd >> 5) & 0xF, '0', 2, 10);
                ts[11] = '-';
                IntToStr(ts + 12, ((flist[i].fd >> 9) & 0x7F) + 1980, 10);
                ts[16] = ' ';
                IntToStrPad(ts + 17, flist[i].fs, ' ', 10, 10);
                MMPrintString(ts);
                MMPrintString("  ");
            }
            MMPrintString(flist[i].fn + 1);
            MMPrintString("\r\n");
            // check if it is more than a screen full
            if (++ListCnt >= Option.Height && i < fcnt)
            {
                MMPrintString("PRESS ANY KEY ...");
                do
                {
                    ShowCursor(1);
                    routinechecks();
                    if (MMAbort)
                    {
                        FreeMemorySafe((void **)&flist);
                        if(FatFSFileSystem) f_closedir(&djd);
                        else lfs_dir_close(&lfs, &lfs_dir);
                        WDTimer = 0; // turn off the watchdog timer
                        memset(inpbuf, 0, STRINGSIZE);
                        ShowCursor(false);
                        FatFSFileSystem=FatFSFileSystemSave;
                        longjmp(mark, 1);
                    }
                    c = -1;
                    if (ConsoleRxBufHead != ConsoleRxBufTail)
                    { // if the queue has something in it
                        c = ConsoleRxBuf[ConsoleRxBufTail];
                        ConsoleRxBufTail = (ConsoleRxBufTail + 1) % CONSOLE_RX_BUF_SIZE; // advance the head of the queue
                    }
                } while (c == -1);
                ShowCursor(0);
                MMPrintString("\r                 \r");
                ListCnt = 1;
            }
        }
        // display the summary
        IntToStr(ts, dirs, 10);
        MMPrintString(ts);
        MMPrintString(" director");
        MMPrintString(dirs == 1 ? "y, " : "ies, ");
        IntToStr(ts, fcnt - dirs, 10);
        MMPrintString(ts);
        MMPrintString(" file");
        MMPrintString((fcnt - dirs) == 1 ? "" : "s");
        MMPrintString("\r\n");
        FreeMemorySafe((void **)&flist);
        if(FatFSFileSystem) f_closedir(&djd);
        else lfs_dir_close(&lfs, &lfs_dir);
        memset(inpbuf, 0, STRINGSIZE);
        FatFSFileSystem=FatFSFileSystemSave;
        longjmp(mark, 1);

}
// remove unnecessary text
void CrunchData(unsigned char **p, int c)
{
    static unsigned char inquotes, lastch, incomment;

    if (c == '\n')
        c = '\r'; // CR is the end of line terminator
    if (c == 0 || c == '\r')
    {
        inquotes = false;
        incomment = false; // newline so reset our flags
        if (c)
        {
            if (lastch == '\r')
                return; // remove two newlines in a row (ie, empty lines)
            *((*p)++) = '\r';
        }
        lastch = '\r';
        return;
    }

    if (incomment)
        return; // discard comments
    if (c == ' ' && lastch == '\r')
        return; // trim all spaces at the start of the line
    if (c == '"')
        inquotes = !inquotes;
    if (inquotes)
    {
        *((*p)++) = c; // copy everything within quotes
        return;
    }
    if (c == '\'')
    { // skip everything following a comment
        incomment = true;
        return;
    }
    if (c == ' ' && (lastch == ' ' || lastch == ','))
    {
        lastch = ' ';
        return; // remove more than one space or a space after a comma
    }
    *((*p)++) = lastch = c;
}
void cmd_autosave(void)
{
    unsigned char *buf, *p;
    int c, prevc = 0, crunch = false;
    int count = 0;
    uint64_t timeout;
    if (CurrentLinePtr)
        error("Invalid in a program");
    if (*cmdline)
    {
        if (toupper(*cmdline) == 'C')
            crunch = true;
        else
            error("Syntax");
    }
    ClearProgram(); // clear any leftovers from the previous program
    p = buf = GetMemory(EDIT_BUFFER_SIZE);
    CrunchData(&p, 0); // initialise the crunch data subroutine
    while ((c = MMInkey()) != 0x1a && c != F1 && c != F2)
    { // while waiting for the end of text char
        if (c == -1 && count && time_us_64() - timeout > 100000)
        {
            fflush(stdout);
            count = 0;
        }
        if (p == buf && c == '\n')
            continue; // throw away an initial line feed which can follow the command
        if ((p - buf) >= EDIT_BUFFER_SIZE)
            error("Not enough memory");
        if (isprint(c) || c == '\r' || c == '\n' || c == TAB)
        {
            if (c == TAB)
                c = ' ';
            if (crunch)
                CrunchData(&p, c); // insert into RAM after throwing away comments. etc
            else
                *p++ = c; // insert the input into RAM
            {
                if (!(c == '\n' && prevc == '\r'))
                {
                    MMputchar(c, 0);
                    count++;
                    timeout = time_us_64();
                } // and echo it
                if (c == '\r')
                {
                    MMputchar('\n', 1);
                    count = 0;
                }
            }
            prevc = c;
        }
    }
    fflush(stdout);

    *p = 0; // terminate the string in RAM
    while (getConsole() != -1)
        ; // clear any rubbish in the input
          //    ClearSavedVars();                                               // clear any saved variables
    SaveProgramToFlash(buf, true);
    ClearSavedVars(); // clear any saved variables
    FreeMemory(buf);
    if (c == F2)
    {
        ClearVars(0);
        strcpy(inpbuf, "RUN\r\n");
        tokenise(true);         // turn into executable code
        ExecuteProgram(tknbuf); // execute the line straight away
    }
}

void FileOpen(char *fname, char *fmode, char *ffnbr)
{
    int fnbr;
    BYTE mode = 0;
    if (str_equal(fmode, "OUTPUT"))
        mode = FA_WRITE | FA_CREATE_ALWAYS;
    else if (str_equal(fmode, "APPEND"))
        mode = FA_WRITE | FA_OPEN_APPEND;
    else if (str_equal(fmode, "INPUT"))
        mode = FA_READ;
    else if (str_equal(fmode, "RANDOM"))
        mode = FA_WRITE | FA_OPEN_APPEND | FA_READ;
    else
        error("File access mode");

    if (*ffnbr == '#')
        ffnbr++;
    fnbr = getinteger(ffnbr);
    BasicFileOpen(fname, fnbr, mode);
}

void cmd_open(void)
{
    int fnbr;
    char *fname;
    char ss[4]; // this will be used to split up the argument line

    ss[0] = tokenAS;
    ss[1] = tokenFOR;
    ss[2] = ',';
    ss[3] = 0;
    {                             // start a new block
        getargs(&cmdline, 7, ss); // getargs macro must be the first executable stmt in a block
        if (!(argc == 3 || argc == 5 || argc == 7))
            error("Syntax");
        fname = getCstring(argv[0]);

        // check that it is a serial port that we are opening
        if (argc == 5 && !(mem_equal(fname, "COM1:", 5) || mem_equal(fname, "COM2:", 5)))
        {
            FileOpen(fname, argv[2], argv[4]);
            diskchecktimer = DISKCHECKRATE;
            return;
        }
        if (!(mem_equal(fname, "COM1:", 5) || mem_equal(fname, "COM2:", 5)))
            error("Invalid COM port");
        if ((*argv[2] == 'G') || (*argv[2] == 'g'))
        {
            MMFLOAT timeadjust = 0.0;
            argv[2]++;
            if (!((*argv[2] == 'P') || (*argv[2] == 'p')))
                error("Syntax");
            argv[2]++;
            if (!((*argv[2] == 'S') || (*argv[2] == 's')))
                error("Syntax");
            if (argc >= 5)
                timeadjust = getnumber(argv[4]);
            if (timeadjust < -12.0 || timeadjust > 14.0)
                error("Invalid Time Offset");
            gpsmonitor = 0;
            if (argc == 7)
                gpsmonitor = getint(argv[6], 0, 1);
            GPSadjust = (int)(timeadjust * 3600.0);
            // check that it is a serial port that we are opening
            SerialOpen(fname);
            fnbr = FindFreeFileNbr();
            GPSfnbr = fnbr;
            FileTable[fnbr].com = fname[3] - '0';
            if (mem_equal(fname, "COM1:", 5))
                GPSchannel = 1;
            if (mem_equal(fname, "COM2:", 5))
                GPSchannel = 2;
            gpsbuf = gpsbuf1;
            gpscurrent = 0;
            gpscount = 0;
        }
        else
        {
            if (*argv[2] == '#')
                argv[2]++;
            fnbr = getint(argv[2], 1, MAXOPENFILES);
            if (FileTable[fnbr].com != 0)
                error("Already open");
            SerialOpen(fname);
            FileTable[fnbr].com = fname[3] - '0';
        }
    }
}

void fun_inputstr(void)
{
    int i, nbr, fnbr;
    getargs(&ep, 3, ",");
    if (argc != 3)
        error("Syntax");
    sret = GetTempMemory(STRINGSIZE); // this will last for the life of the command
    nbr = getint(argv[0], 1, MAXSTRLEN);
    if (*argv[2] == '#')
        argv[2]++;
    fnbr = getinteger(argv[2]);
    if (fnbr == 0)
    { // accessing the console
        for (i = 1; i <= nbr && kbhitConsole(); i++)
            sret[i] = getConsole(); // get the char from the console input buffer and save in our returned string
    }
    else
    {
        if (fnbr < 1 || fnbr > MAXOPENFILES)
            error("File number");
        if (FileTable[fnbr].com == 0)
            error("File number is not open");
        targ = T_STR;
        if (FileTable[fnbr].com > MAXCOMPORTS)
        {
            for (i = 1; i <= nbr && !MMfeof(fnbr); i++)
                sret[i] = FileGetChar(fnbr); // get the char from the SD card and save in our returned string
            *sret = i - 1;                   // update the length of the string
            return;                          // all done so skip the rest
        }
        for (i = 1; i <= nbr && SerialRxStatus(FileTable[fnbr].com); i++)
            sret[i] = SerialGetchar(FileTable[fnbr].com); // get the char from the serial input buffer and save in our returned string
    }
    *sret = i - 1;
}

void fun_eof(void)
{
    int fnbr;
    getargs(&ep, 1, ",");
    if (argc == 0)
        error("Syntax");
    if (*argv[0] == '#')
        argv[0]++;
    fnbr = getinteger(argv[0]);
    iret = MMfeof(fnbr);
    targ = T_INT;
}

void cmd_flush(void)
{
    int fnbr;
    getargs(&cmdline, 1, ",");
    if (argc == 0)
        error("Syntax");
    if (*argv[0] == '#')
        argv[0]++;
    fnbr = getinteger(argv[0]);
    if (fnbr == 0) // accessing the console
        return;
    else
    {
        if (fnbr < 1 || fnbr > MAXOPENFILES)
            error("File number");
        if (FileTable[fnbr].com == 0)
            error("File number is not open");
        if (FileTable[fnbr].com > MAXCOMPORTS )
        {
            if(filesource[fnbr]==FATFSFILE)f_sync(FileTable[fnbr].fptr);
            else lfs_file_sync(&lfs, FileTable[fnbr].lfsptr);
        }
        else
        {
            while (SerialTxStatus(FileTable[fnbr].com))
            {
            }
        }
    }
}

void fun_loc(void)
{
    int fnbr;
    getargs(&ep, 1, ",");
    if (argc == 0)
        error("Syntax");
    if (*argv[0] == '#')
        argv[0]++;
    fnbr = getinteger(argv[0]);
    if (fnbr == 0) // accessing the console
        iret = kbhitConsole();
    else
    {
        if (fnbr < 1 || fnbr > MAXOPENFILES)
            error("File number");
        if (FileTable[fnbr].com == 0)
            error("File number is not open");
        if (FileTable[fnbr].com > MAXCOMPORTS)
        {
            if(filesource[fnbr]==FLASHFILE)iret = lfs_file_tell(&lfs,FileTable[fnbr].lfsptr) + 1;
            else iret = (*(FileTable[fnbr].fptr)).fptr + 1;
        }
        else
            iret = SerialRxStatus(FileTable[fnbr].com);
    }
    targ = T_INT;
}

void fun_lof(void)
{
    int fnbr;
    getargs(&ep, 1, ",");
    if (argc == 0)
        error("Syntax");
    if (*argv[0] == '#')
        argv[0]++;
    fnbr = getinteger(argv[0]);
    if (fnbr == 0) // accessing the console
        iret = 0;
    else
    {
        if (fnbr < 1 || fnbr > MAXOPENFILES)
            error("File number");
        if (FileTable[fnbr].com == 0)
            error("File number is not open");
        if (FileTable[fnbr].com > MAXCOMPORTS)
        {
            if(filesource[fnbr]==FLASHFILE) iret = FileTable[fnbr].lfsptr->ctz.size;
            else iret = f_size(FileTable[fnbr].fptr);
        }
        else
            iret = (TX_BUFFER_SIZE - SerialTxStatus(FileTable[fnbr].com));
    }
    targ = T_INT;
}

void cmd_close(void)
{
    int i, fnbr;
    getargs(&cmdline, (MAX_ARG_COUNT * 2) - 1, ","); // getargs macro must be the first executable stmt in a block
    if ((argc & 0x01) == 0)
        error("Syntax");
    for (i = 0; i < argc; i += 2)
    {
        if ((*argv[i] == 'G') || (*argv[i] == 'g'))
        {
            argv[i]++;
            if (!((*argv[i] == 'P') || (*argv[i] == 'p')))
                error("Syntax");
            argv[i]++;
            if (!((*argv[i] == 'S') || (*argv[i] == 's')))
                error("Syntax");
            if (!GPSfnbr)
                error("Not open");
            SerialClose(FileTable[GPSfnbr].com);
            FileTable[GPSfnbr].com = 0;
            GPSfnbr = 0;
            GPSchannel = 0;
            GPSlatitude = 0;
            GPSlongitude = 0;
            GPSspeed = 0;
            GPSvalid = 0;
            GPStime[1] = '0';
            GPStime[2] = '0';
            GPStime[4] = '0';
            GPStime[5] = '0';
            GPStime[7] = '0';
            GPStime[8] = '0';
            GPSdate[1] = '0';
            GPSdate[2] = '0';
            GPSdate[4] = '0';
            GPSdate[5] = '0';
            GPSdate[9] = '0';
            GPSdate[10] = '0';
            GPStrack = 0;
            GPSdop = 0;
            GPSsatellites = 0;
            GPSaltitude = 0;
            GPSfix = 0;
            GPSadjust = 0;
            gpsmonitor = 0;
        }
        else
        {
            if (*argv[i] == '#')
                argv[i]++;
            fnbr = getint(argv[i], 1, MAXOPENFILES);
            if (FileTable[fnbr].com == 0)
                error("File number is not open");
            while (SerialTxStatus(FileTable[fnbr].com) && !MMAbort)
                ; // wait for anything in the buffer to be transmitted
            if (FileTable[fnbr].com > MAXCOMPORTS)
            {
                FileClose(fnbr);
                diskchecktimer = DISKCHECKRATE;
            }
            else
                SerialClose(FileTable[fnbr].com);

            FileTable[fnbr].com = 0;
        }
    }
}
void __not_in_flash_func(CheckSDCard)(void)
{
    if (CurrentlyPlaying == P_NOTHING)
    {
        if (!(SDCardStat & STA_NOINIT))
        { // the card is supposed to be initialised - lets check
            char buff[4];
            if (disk_ioctl(0, MMC_GET_OCR, buff) != RES_OK)
            {
                BYTE s;
                s = SDCardStat;
                s |= (STA_NODISK | STA_NOINIT);
                SDCardStat = s;
                ShowCursor(false);
                MMPrintString("Warning: SDcard Removed\r\n> ");
                FatFSFileSystem=0;
            }
        }
        diskchecktimer = DISKCHECKRATE;
    }
    else if (CurrentlyPlaying == P_WAV)
        checkWAVinput();
}
void LoadOptions(void)
{
    int i = sizeof(struct option_s);
    char *pp = (char *)flash_option_contents;
    char *qq = (char *)&Option;
    while (i--)
        *qq++ = *pp++;
}

void ResetOptions(void)
{
    disable_sd();
    disable_audio();
    disable_systemi2c();
    disable_systemspi();
    memset((void *)&Option.Magic, 0, sizeof(struct option_s));
    Option.Magic = MagicKey;
    Option.Height = SCREENHEIGHT;
    Option.Width = SCREENWIDTH;
    Option.Tab = 2;
    Option.DefaultFont = 0x01;
    Option.DefaultBrightness = 100;
    Option.Baudrate = CONSOLE_BAUDRATE;
#ifdef PICOMITEVGA
    Option.CPU_Speed = 126000;
    Option.DISPLAY_CONSOLE = 1;
    Option.DISPLAY_TYPE = MONOVGA;
    Option.KeyboardConfig = CONFIG_US;
    Option.VGABC = 0x0000;
    Option.VGAFC = 0xFFFF;
#else
    Option.CPU_Speed = 133000;
    Option.KeyboardConfig = NO_KEYBOARD;
#endif
    Option.AUDIO_SLICE = 99;
    Option.SDspeed = 10;
    Option.DISPLAY_ORIENTATION = DISPLAY_LANDSCAPE;
    Option.DefaultFont = 0x01;
    Option.DefaultFC = WHITE;
    Option.DefaultBC = BLACK;
    Option.LCDVOP = 0xB1;
    Option.PROG_FLASH_SIZE = MAX_PROG_SIZE;
    Option.HEAP_SIZE = MAX_PROG_SIZE;
    Option.INT1pin = 9;
    Option.INT2pin = 10;
    Option.INT3pin = 11;
    Option.INT4pin = 12;
    Option.DefaultBrightness = 100;
    Option.numlock = 1;
    Option.repeat = 0b101100;
    uint8_t txbuf[4] = {0x9f};
    uint8_t rxbuf[4] = {0};
    flash_do_cmd(txbuf, rxbuf, 4);
    Option.FlashSize= 1 << rxbuf[3];
    SaveOptions();
    uSec(250000);
}
void ResetAllFlash(void)
{
    ClearSavedVars();
    ResetOptions();
    disable_interrupts();
    for (int i = 0; i < MAXFLASHSLOTS + 1; i++)
    {
        uint32_t j = FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (i * MAX_PROG_SIZE);
        flash_range_erase(j, MAX_PROG_SIZE);
    }
    enable_interrupts();
    FlashWriteInit(PROGRAM_FLASH);
    flash_range_erase(realflashpointer, MAX_PROG_SIZE);
    FlashWriteByte(0);
    FlashWriteByte(0);
    FlashWriteByte(0); // terminate the program in flash
    FlashWriteClose();
}
void FlashWriteInit(int region)
{
    for (int i = 0; i < 64; i++)
        MemWord.i32[i] = 0xFFFFFFFF;
    mi8p = 0;
    if (region == PROGRAM_FLASH)
        realflashpointer = (uint32_t)PROGSTART;
    else if (region == SAVED_VARS_FLASH)
        realflashpointer = (uint32_t)(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE);
    disable_interrupts();
}
void FlashWriteBlock(void)
{
    int i;
    uint32_t address = realflashpointer - 256;
    //    if(address % 32)error("Memory write address");
    flash_range_program((const uint32_t)address, (const char *)&MemWord.i64[0], 256);
    for (i = 0; i < 64; i++)
        MemWord.i32[i] = 0xFFFFFFFF;
}
void FlashWriteByte(unsigned char b)
{
    realflashpointer++;
    MemWord.i8[mi8p] = b;
    mi8p++;
    mi8p %= 256;
    if (mi8p == 0)
    {
        FlashWriteBlock();
    }
}
void FlashWriteWord(unsigned int i)
{
    FlashWriteByte(i & 0xFF);
    FlashWriteByte((i >> 8) & 0xFF);
    FlashWriteByte((i >> 16) & 0xFF);
    FlashWriteByte((i >> 24) & 0xFF);
}

void FlashWriteAlign(void)
{
    while (mi8p != 0)
    {
        FlashWriteByte(0x0);
    }
    FlashWriteWord(0xFFFFFFFF);
}

void FlashWriteClose(void)
{
    while (mi8p != 0)
    {
        FlashWriteByte(0xff);
    }
    enable_interrupts();
}

/*******************************************************************************************************************
 The variables are stored in a reserved flash area (which in total is 2K).
 The first few bytes are used for the options. So we must save the options in RAM before we erase, then write the
 options back.  The variables saved by this command are then written to flash starting just after the options.
********************************************************************************************************************/
void cmd_var(void)
{
    unsigned char *p, *buf, *bufp, *varp, *vdata, lastc;
    int i, j, nbr = 1, nbr2 = 1, array, type, SaveDefaultType;
    int VarList[MAX_ARG_COUNT];
    unsigned char *VarDataList[MAX_ARG_COUNT];
    if ((p = checkstring(cmdline, "CLEAR")))
    {
        checkend(p);
        ClearSavedVars();
        return;
    }
    if ((p = checkstring(cmdline, "RESTORE")))
    {
        char b[MAXVARLEN + 3];
        checkend(p);
        //        SavedVarsFlash = (char*)FLASH_SAVED_VAR_ADDR;      // point to where the variables were saved
        if (*SavedVarsFlash == 0xFF)
            return;                             // zero in this location means that nothing has ever been saved
        SaveDefaultType = DefaultType;          // save the default type
        bufp = (unsigned char *)SavedVarsFlash; // point to where the variables were saved
        while (*bufp != 0xff)
        {                   // 0xff is the end of the variable list
            type = *bufp++; // get the variable type
            array = type & 0x80;
            type &= 0x7f;                 // set array to true if it is an array
            DefaultType = TypeMask(type); // and set the default type to this
            if (array)
            {
                strcpy(b, bufp);
                strcat(b, "()");
                vdata = findvar(b, type | V_EMPTY_OK | V_NOFIND_ERR); // find an array
            }
            else
                vdata = findvar(bufp, type | V_FIND); // find or create a non arrayed variable
            if (TypeMask(vartbl[VarIndex].type) != TypeMask(type))
                error("$ type conflict", bufp);
            if (vartbl[VarIndex].type & T_CONST)
                error("$ is a constant", bufp);
            bufp += strlen((char *)bufp) + 1; // step over the name and the terminating zero byte
            if (array)
            { // an array has the data size in the next two bytes
                nbr = *bufp++;
                nbr |= (*bufp++) << 8;
                nbr |= (*bufp++) << 16;
                nbr |= (*bufp++) << 24;
                nbr2 = 1;
                for (j = 0; vartbl[VarIndex].dims[j] != 0 && j < MAXDIM; j++)
                    nbr2 *= (vartbl[VarIndex].dims[j] + 1 - OptionBase);
                if (type & T_STR)
                    nbr2 *= vartbl[VarIndex].size + 1;
                if (type & T_NBR)
                    nbr2 *= sizeof(MMFLOAT);
                if (type & T_INT)
                    nbr2 *= sizeof(long long int);
                if (nbr2 != nbr)
                    error("Array size");
            }
            else
            {
                if (type & T_STR)
                    nbr = *bufp + 1;
                if (type & T_NBR)
                    nbr = sizeof(MMFLOAT);
                if (type & T_INT)
                    nbr = sizeof(long long int);
            }
            while (nbr--)
                *vdata++ = *bufp++; // copy the data
        }
        DefaultType = SaveDefaultType;
        return;
    }

    if ((p = checkstring(cmdline, "SAVE")))
    {
        getargs(&p, (MAX_ARG_COUNT * 2) - 1, ","); // getargs macro must be the first executable stmt in a block
        if (argc && (argc & 0x01) == 0)
            error("Invalid syntax");

        // befor we start, run through the arguments checking for errors
        // before we start, run through the arguments checking for errors
        for (i = 0; i < argc; i += 2)
        {
            checkend(skipvar(argv[i], false));
            VarDataList[i / 2] = findvar(argv[i], V_NOFIND_ERR | V_EMPTY_OK);
            VarList[i / 2] = VarIndex;
            if ((vartbl[VarIndex].type & (T_CONST | T_PTR)) || vartbl[VarIndex].level != 0)
                error("Invalid variable");
            p = &argv[i][strlen(argv[i]) - 1]; // pointer to the last char
            if (*p == ')')
            { // strip off any empty brackets which indicate an array
                p--;
                if (*p == ' ')
                    p--;
                if (*p == '(')
                    *p = 0;
                else
                    error("Invalid variable");
            }
        }
        // load the current variable save table into RAM
        // while doing this skip any variables that are in the argument list for this save
        bufp = buf = GetTempMemory(SAVEDVARS_FLASH_SIZE); // build the saved variable table in RAM
                                                          //        SavedVarsFlash = (char*)FLASH_SAVED_VAR_ADDR;      // point to where the variables were saved
        varp = (unsigned char *)SavedVarsFlash;           // point to where the variables were saved
        while (*varp != 0 && *varp != 0xff)
        {                   // 0xff is the end of the variable list, SavedVarsFlash[4] = 0 means that the flash has never been written to
            type = *varp++; // get the variable type
            array = type & 0x80;
            type &= 0x7f; // set array to true if it is an array
            vdata = varp; // save a pointer to the name
            while (*varp)
                varp++; // skip the name
            varp++;     // and the terminating zero byte
            if (array)
            { // an array has the data size in the next two bytes
                nbr = (varp[0] | (varp[1] << 8) | (varp[2] << 16) | (varp[3] << 24)) + 4;
            }
            else
            {
                if (type & T_STR)
                    nbr = *varp + 1;
                if (type & T_NBR)
                    nbr = sizeof(MMFLOAT);
                if (type & T_INT)
                    nbr = sizeof(long long int);
            }
            for (i = 0; i < argc; i += 2)
            {                                      // scan the argument list
                p = &argv[i][strlen(argv[i]) - 1]; // pointer to the last char
                lastc = *p;                        // get the last char
                if (lastc <= '%')
                    *p = 0; // remove the type suffix for the compare
                if (strncasecmp(vdata, argv[i], MAXVARLEN) == 0)
                { // does the entry have the same name?
                    while (nbr--)
                        varp++; // found matching variable, skip over the entry in flash (ie, do not copy to RAM)
                    i = 9999;   // force the termination of the for loop
                }
                *p = lastc; // restore the type suffix
            }
            // finished scanning the argument list, did we find a matching variable?
            // if not, copy this entry to RAM
            if (i < 9999)
            {
                *bufp++ = type | array;
                while (*vdata)
                    *bufp++ = *vdata++; // copy the name
                *bufp++ = *vdata++;     // and the terminating zero byte
                while (nbr--)
                    *bufp++ = *varp++; // copy the data
            }
        }

        // initialise for writing to the flash
        ClearSavedVars();
        FlashWriteInit(SAVED_VARS_FLASH);
        // now write the variables in RAM recovered from the var save list
        while (buf < bufp)
        {
            FlashWriteByte(*buf++);
        }
        // now save the variables listed in this invocation of VAR SAVE
        for (i = 0; i < argc; i += 2)
        {
            VarIndex = VarList[i / 2];                   // previously saved index to the variable
            vdata = VarDataList[i / 2];                  // pointer to the variable's data
            type = TypeMask(vartbl[VarIndex].type);      // get the variable's type
            type |= (vartbl[VarIndex].type & T_IMPLIED); // set the implied flag
            array = (vartbl[VarIndex].dims[0] != 0);

            nbr = 1; // number of elements to save
            if (array)
            { // if this is an array calculate the number of elements
                for (j = 0; vartbl[VarIndex].dims[j] != 0 && j < MAXDIM; j++)
                    nbr *= (vartbl[VarIndex].dims[j] + 1 - OptionBase);
                type |= 0x80; // an array has the top bit set
            }

            if (type & T_STR)
            {
                if (array)
                    nbr *= (vartbl[VarIndex].size + 1);
                else
                    nbr = *vdata + 1; // for a simple string variable just save the string
            }
            if (type & T_NBR)
                nbr *= sizeof(MMFLOAT);
            if (type & T_INT)
                nbr *= sizeof(long long int);
            if ((uint32_t)realflashpointer + XIP_BASE - (uint32_t)SavedVarsFlash + 36 + nbr > SAVEDVARS_FLASH_SIZE)
            {
                FlashWriteClose();
                error("Not enough memory");
            }
            FlashWriteByte(type); // save its type
            for (j = 0, p = vartbl[VarIndex].name; *p && j < MAXVARLEN; p++, j++)
                FlashWriteByte(*p); // save the name
            FlashWriteByte(0);      // terminate the name
            if (array)
            { // if it is an array save the number of data bytes
                FlashWriteByte(nbr);
                FlashWriteByte(nbr >> 8);
                FlashWriteByte(nbr >> 16);
                FlashWriteByte(nbr >> 24);
            }
            while (nbr--)
                FlashWriteByte(*vdata++); // write the data
        }
        FlashWriteClose();
        return;
    }
    error("Unknown command");
}

void ClearSavedVars(void)
{
    uSec(250000);
    disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE, SAVEDVARS_FLASH_SIZE);
    enable_interrupts();
}
void SaveOptions(void)
{
    uSec(100000);
    disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_ERASE_SIZE);
    enable_interrupts();
    uSec(10000);
    disable_interrupts();
    flash_range_program(FLASH_TARGET_OFFSET, (const uint8_t *)&Option, 512);
    enable_interrupts();
}
