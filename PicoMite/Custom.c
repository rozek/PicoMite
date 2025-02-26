/***********************************************************************************************************************
PicoMite MMBasic

custom.c

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
#include <stdio.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/dma.h"

#define STATIC static

/*************************************************************************************************************************
**************************************************************************************************************************
IMPORTANT:
This module is empty and should be used for your special functions and commands.  In the standard distribution this file  
will never be changed, so your code should be safe here.  You should avoid placing commands and functions in other files as
they may be changed and you would then need to re insert your changes in a new release of the source.

**************************************************************************************************************************
**************************************************************************************************************************/


/********************************************************************************************************************************************
 custom commands and functions
 each function is responsible for decoding a command
 all function names are in the form cmd_xxxx() (for a basic command) or fun_xxxx() (for a basic function) so, if you want to search for the
 function responsible for the NAME command look for cmd_name

 There are 4 items of information that are setup before the command is run.
 All these are globals.

 int cmdtoken	This is the token number of the command (some commands can handle multiple
				statement types and this helps them differentiate)

 unsigned char *cmdline	This is the command line terminated with a zero unsigned char and trimmed of leading
				spaces.  It may exist anywhere in memory (or even ROM).

 unsigned char *nextstmt	This is a pointer to the next statement to be executed.  The only thing a
				command can do with it is save it or change it to some other location.

 unsigned char *CurrentLinePtr  This is read only and is set to NULL if the command is in immediate mode.

 The only actions a command can do to change the program flow is to change nextstmt or
 execute longjmp(mark, 1) if it wants to abort the program.

 ********************************************************************************************************************************************/
/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/claim.h"
#define PIO_NUM(pio) ((pio) == pio0 ? 0 : 1)
#define CLKMIN ((Option.CPU_Speed*125)>>13)
#define CLKMAX (Option.CPU_Speed *1000)

#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#ifndef PICOMITEVGA
char *pioRXinterrupts[4][2]={0};
char *pioTXinterrupts[4][2]={0};
uint8_t pioTXlast[4][2]={0};
#else
char *pioRXinterrupts[4]={0};
char *pioTXinterrupts[4]={0};
uint8_t pioTXlast[4]={0};
#endif
char *DMAinterruptRX=NULL;
char *DMAinterruptTX=NULL;
uint32_t dma_rx_chan;
uint32_t dma_tx_chan;
int dma_pio;
int dma_sm;

int piointerrupt=0;
static inline uint32_t pio_sm_calc_wrap(uint wrap_target, uint wrap) {
    uint32_t calc=0;
    valid_params_if(PIO, wrap < PIO_INSTRUCTION_COUNT);
    valid_params_if(PIO, wrap_target < PIO_INSTRUCTION_COUNT);
    return  (calc & ~(PIO_SM0_EXECCTRL_WRAP_TOP_BITS | PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS)) |
            (wrap_target << PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB) |
            (wrap << PIO_SM0_EXECCTRL_WRAP_TOP_LSB);
}

void cmd_pio(void){
    unsigned char *tp;
    tp = checkstring(cmdline, "EXECUTE");
    if(tp){
        int i;
        getargs(&tp, (MAX_ARG_COUNT * 2) - 1, (unsigned char *)",");
        if((argc & 0x01) == 0) error("Syntax");
        if(argc<5)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        for(i = 4; i < argc; i += 2) {
            pio_sm_exec(pio, sm, getint(argv[i],0,65535));
        }
        return;
    }
    tp = checkstring(cmdline, "WRITE");
    if(tp){
        int i=6;
        getargs(&tp, (MAX_ARG_COUNT * 2) - 1, (unsigned char *)",");
        if((argc & 0x01) == 0) error("Syntax");
        if(argc<5)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        int count=getint(argv[4],0,MAX_ARG_COUNT-3);
        while(count--) {
            pio_sm_put_blocking(pio, sm, getint(argv[i],0,0xFFFFFFFF));
            i+=2;
        }
        return;
    }
    tp = checkstring(cmdline, "DMA_IN");
    if(tp){
        getargs(&tp, 13, (unsigned char *)",");
        if(checkstring(argv[0],"OFF")){
                if(dma_channel_is_busy(dma_rx_chan)){
                        dma_channel_abort(dma_rx_chan);
                        dma_channel_unclaim(dma_rx_chan);
                } else error("Not active");
                return;
        }
        if(DMAinterruptRX || dma_channel_is_busy(dma_rx_chan)) error("DMA active");
        if(argc<7)error("Syntax");
#ifdef PICOMITEVGA
        int i=getint(argv[0],1,1);
#else
        int i=getint(argv[0],0,1);
#endif
        PIO pio = (i ? pio1: pio0);
        int sm=getint(argv[2],0,3);
        int nbr=getint(argv[4],0,0x7FFFFFFF);
        uint32_t *a1int=NULL;
	void *ptr1 = NULL;
        int toarraysize;
        ptr1 = findvar(argv[6], V_FIND | V_EMPTY_OK | V_NOFIND_ERR);
        if(vartbl[VarIndex].type &  T_INT) {
                if(vartbl[VarIndex].dims[1] != 0) error("Target must be an 1D integer array");
                if(vartbl[VarIndex].dims[0] <= 0) {		// Not an array
                        error("Target must be an 1D integer array");
                }
                toarraysize=vartbl[VarIndex].dims[0]-OptionBase+1;
                a1int = (uint32_t *)ptr1;
                if((uint32_t)ptr1!=(uint32_t)vartbl[VarIndex].val.s)error("Syntax");
        } else error("Target must be an 1D integer array");
        if(argc>=9 && *argv[10]){
                DMAinterruptRX=GetIntAddress(argv[8]);
                InterruptUsed=true;
        }
        int dmasize=DMA_SIZE_32;
        if(argc==11){
                dmasize=getinteger(argv[10]);
                if(!(dmasize==8 || dmasize==16 || dmasize==32))error("Invalid transfer size");
                if(dmasize==8)dmasize=DMA_SIZE_8;
                else if(dmasize==16)dmasize=DMA_SIZE_16;
                else if(dmasize==32)dmasize=DMA_SIZE_32;
        }
        dma_rx_chan = PIO_RX_DMA;
        dma_channel_claim (dma_rx_chan);

        dma_channel_config c = dma_channel_get_default_config(dma_rx_chan);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_transfer_data_size(&c, dmasize);
        channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
        if(argc==13){
                int size=getinteger(argv[12]);
                if(!(size==2 || size==4 || size==8 || size==16 || size==32 || size==64 || size==128 || size==256 || size==512 || size==1024 || size== 2048 || size==4096 || size==8192 || size==16384 || size==32768))error("Not power of 2");
                int i=0,j=size;
                if((uint32_t)a1int & (j-1))error("Data alignment error");
                while(j>>=1)i++;
                i+=dmasize;
                if((1<<i)>(toarraysize*8))error("Array size");
                channel_config_set_ring(&c,true,i);
//                PInt(i);PIntComma(dmasize);PIntComma(nbr);PIntComma(size);PRet();
        } else if((nbr<<dmasize)>(toarraysize*8))error("Array size");
        dma_channel_configure(dma_rx_chan, &c,
                a1int,        // Destination pointer
                &pio->rxf[sm],      // Source pointer
                nbr, // Number of transfers
                true                // Start immediately
        );
        pio_sm_set_enabled(pio, sm, true);
        return;
    }
    tp = checkstring(cmdline, "DMA_OUT");
    if(tp){
        getargs(&tp, 13, (unsigned char *)",");
        if(checkstring(argv[0],"OFF")){
                if(dma_channel_is_busy(dma_tx_chan)){
                        dma_channel_abort(dma_tx_chan);
                        dma_channel_unclaim(dma_tx_chan);
                } else error("Not active");
                return;
        }
        if(DMAinterruptTX || dma_channel_is_busy(dma_tx_chan)) error("DMA active");
        if(argc<7)error("Syntax");
#ifdef PICOMITEVGA
        int i=getint(argv[0],1,1);
#else
        int i=getint(argv[0],0,1);
#endif
        PIO pio = (i ? pio1: pio0);
        int sm=getint(argv[2],0,3);
        dma_pio=i;
        dma_sm=sm*8;
        int nbr=getint(argv[4],0,0x7FFFFFFF);
        uint32_t *a1int=NULL;
	void *ptr1 = NULL;
        int toarraysize;
        ptr1 = findvar(argv[6], V_FIND | V_EMPTY_OK | V_NOFIND_ERR);
        if(vartbl[VarIndex].type &  T_INT) {
                if(vartbl[VarIndex].dims[1] != 0) error("Target must be an 1D integer array");
                if(vartbl[VarIndex].dims[0] <= 0) {		// Not an array
                        error("Target must be an 1D integer array");
                }
                toarraysize=vartbl[VarIndex].dims[0]-OptionBase+1;
                a1int = (uint32_t *)ptr1;
                if((uint32_t)ptr1!=(uint32_t)vartbl[VarIndex].val.s)error("Syntax");
        } else error("Target must be an 1D integer array");
        if(argc>=9 && *argv[8]){
                DMAinterruptTX=GetIntAddress(argv[8]);
                InterruptUsed=true;
        }
        int dmasize=DMA_SIZE_32;
        if(argc>=11 && *argv[10]){
                dmasize=getinteger(argv[10]);
                if(!(dmasize==8 || dmasize==16 || dmasize==32))error("Invalid transfer size");
                if(dmasize==8)dmasize=DMA_SIZE_8;
                else if(dmasize==16)dmasize=DMA_SIZE_16;
                else if(dmasize==32)dmasize=DMA_SIZE_32;
        }
        dma_tx_chan = PIO_TX_DMA;
        dma_channel_claim (dma_tx_chan);
        dma_channel_config c = dma_channel_get_default_config(dma_tx_chan);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
        channel_config_set_transfer_data_size(&c, dmasize);
        if(argc==13){
                int size=getinteger(argv[12]);
                if(!(size==2 || size==4 || size==8 || size==16 || size==32 || size==64 || size==128 || size==256 || size==512 || size==1024 || size== 2048 || size==4096 || size==8192 || size==16384 || size==32768))error("Not power of 2");
                int i=0,j=size;
                if((uint32_t)a1int & (j-1))error("Data alignment error");
                while(j>>=1)i++;
                i+=dmasize;
                if((1<<i)>(toarraysize*8))error("Array size");
                channel_config_set_ring(&c,false,i);
//                PInt(i);PIntComma(dmasize);PIntComma(nbr);PIntComma(size);PRet();
        } else if((nbr<<dmasize)>(toarraysize*8))error("Array size");
        dma_channel_configure(dma_tx_chan,
                &c,
                &pio->txf[sm],      // Destination pointer
                a1int,        // Source pointer
                nbr, // Number of transfers
                true                // Start immediately
        );
        pio_sm_set_enabled(pio, sm, true);
        return;
    }
    tp = checkstring(cmdline, "INTERRUPT");
    if(tp){
        unsigned char *p;
        unsigned int nbr, *d;
        long long int *dd;
        int i;
        getargs(&tp, 7, (unsigned char *)",");
        if((argc & 0x01) == 0) error("Syntax");
        if(argc<5)error("Syntax");
#ifdef PICOMITEVGA
        i=getint(argv[0],1,1);
        PIO pio = (i ? pio1: pio0);
        int sm=getint(argv[2],0,3);
        if(*argv[4]){
                if(checkstring(argv[4],"0"))pioRXinterrupts[sm]=NULL;
                else pioRXinterrupts[sm]=GetIntAddress(argv[4]);
        }
        if(argc==7){
                if(checkstring(argv[6],"0"))pioTXinterrupts[sm]=NULL;
                else pioTXinterrupts[sm]=GetIntAddress(argv[6]);
        }
        piointerrupt=0;
        for(int i=0;i<4;i++){
                if(pioRXinterrupts[i] || pioTXinterrupts[i]){
                        piointerrupt=1;
                        InterruptUsed=1;
                }
        }
#else
        i=getint(argv[0],0,1);
        PIO pio = (i ? pio1: pio0);
        int sm=getint(argv[2],0,3);
        if(*argv[4]){
                if(checkstring(argv[4],"0"))pioRXinterrupts[sm][i]=NULL;
                else pioRXinterrupts[sm][i]=GetIntAddress(argv[4]);
        }
        if(argc==7){
                if(checkstring(argv[6],"0"))pioTXinterrupts[sm][i]=NULL;
                else pioTXinterrupts[sm][i]=GetIntAddress(argv[6]);
        }
        piointerrupt=0;
        for(int i=0;i<4;i++){
                for(int j=0 ;j<2;j++){
                        if(pioRXinterrupts[i][j] || pioTXinterrupts[i][j]){
                                piointerrupt=1;
                                InterruptUsed=1;
                        }
                }
        }
#endif
        return;
    }
    tp = checkstring(cmdline, "READ");
    if(tp){
        unsigned char *p;
        unsigned int nbr, *d;
        long long int *dd;
        int i;
        getargs(&tp, (MAX_ARG_COUNT * 2) - 1, (unsigned char *)",");
        if((argc & 0x01) == 0) error("Syntax");
        if(argc<5)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        nbr = getinteger(argv[4]);
        dd = findvar(argv[6], V_NOFIND_NULL | V_EMPTY_OK);
        if(dd == NULL) error("Invalid variable");
        if(((vartbl[VarIndex].type & T_INT) && vartbl[VarIndex].dims[0] > 0 && vartbl[VarIndex].dims[1] == 0))
        {		// integer array
            if( (((long long int *)dd - vartbl[VarIndex].val.ia) + nbr) > (vartbl[VarIndex].dims[0] + 1 - OptionBase) )
                error("Insufficient array size");
        }  else  if ((vartbl[VarIndex].type & T_INT) && vartbl[VarIndex].dims[0] == 0 && nbr==1){
            // single variable
        }  else error("Invalid variable");
        
        while(nbr--) {
       	    *dd = pio_sm_get(pio, sm);
            if(pio->fdebug & (1<<(sm + 16)))*dd=-1;
       	    if(nbr)dd++;
        }
        return;
    }
    tp = checkstring(cmdline, "PROGRAM LINE");
    if(tp){
        getargs(&tp,5,",");
        if(argc!=5)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int slot=getint(argv[2],0,31);
        int instruction=getint(argv[4],0,0xFFFF);
        pio->instr_mem[slot]=instruction;
        return;
    }
    
    tp = checkstring(cmdline, "CLEAR");
    if(tp){
        getargs(&tp,1,",");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        for(int sm=0;sm<4;sm++){
            hw_clear_bits(&pio->ctrl, 1 << (PIO_CTRL_SM_ENABLE_LSB + sm));
            pio->sm[sm].pinctrl=(5<<26);
            pio->sm[sm].execctrl=(0x1f<<12);
            pio->sm[sm].shiftctrl=(3<<18);
            pio_sm_clear_fifos(pio,sm);
        }
        pio_clear_instruction_memory(pio);
        return;
    }
    tp = checkstring(cmdline, "MAKE RING BUFFER");
    if(tp){
        getargs(&tp,3,",");
        if(argc<3)error("Syntax");
        int size=getinteger(argv[2]);
        if(!(size==256 || size==512 || size==1024 || size== 2048 || size==4096 || size==8192 || size==16384 || size==32768))error("Not power of 2");
        findvar(argv[0], V_FIND | V_NOFIND_ERR);
        if ((vartbl[VarIndex].type & T_INT) && vartbl[VarIndex].dims[0] == 0 && vartbl[VarIndex].level==0){
                vartbl[VarIndex].val.s =(char *)GetAlignedMemory(size);
                vartbl[VarIndex].size=255;
                vartbl[VarIndex].dims[0] = size/8-1+OptionBase;
        }  else error("Invalid variable");
        return;
    }

    tp = checkstring(cmdline, "PROGRAM");
    if(tp){
        struct pio_program program;
        getargs(&tp,3,",");
        if(argc!=3)error("Syntax");
        void *prt1;
        program.length=32;
        program.origin=0;
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
	    void *ptr1 = findvar(argv[2], V_FIND | V_EMPTY_OK | V_NOFIND_ERR);
        if(vartbl[VarIndex].type & T_INT) {
            if(vartbl[VarIndex].dims[1] != 0) error("Invalid variable");
            if(vartbl[VarIndex].dims[0] <= 0) {		// Not an array
                error("Argument 2 must be integer array");
            }
            if((vartbl[VarIndex].dims[0] - OptionBase)!=7)error("Array size");
            program.instructions = (const uint16_t *)ptr1;
        } else error("Argument 2 must be integer array");
        for(int sm=0;sm<4;sm++)hw_clear_bits(&pio->ctrl, 1 << (PIO_CTRL_SM_ENABLE_LSB + sm));
        pio_clear_instruction_memory(pio);
        int offset=pio_add_program(pio, &program);
        return;
    }
    tp = checkstring(cmdline, "START");
    if(tp){
        getargs(&tp,3,",");
        if(argc!=3)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        pio_sm_clear_fifos(pio,sm);
        pio_sm_set_enabled(pio, sm, true);
        pio_sm_restart(pio, sm);
        return;
    }
    tp = checkstring(cmdline, "STOP");
    if(tp){
        getargs(&tp,3,",");
        if(argc!=3)error("Syntax");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        pio_sm_set_enabled(pio, sm, false);
        return;
    }
    tp = checkstring(cmdline, "INIT MACHINE");
    if(tp){
        int start=0;
        getargs(&tp,13,",");
        if(argc<5)error("Syntax");
        pio_sm_config mypio=pio_get_default_sm_config();
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        int sm=getint(argv[2],0,3);
        float clock=getnumber(argv[4]);
        if(clock<CLKMIN || clock> CLKMAX)error("Clock must be in range % to %",CLKMIN,CLKMAX);
        clock=(float)Option.CPU_Speed*1000.0/clock;
        if(argc>5 && *argv[6])mypio.pinctrl = getint(argv[6],0,0xFFFFFFFF);
        if(argc>7 && *argv[8])mypio.execctrl= getint(argv[8],0,0xFFFFFFFF);
        if(argc>9 && *argv[10])mypio.shiftctrl= getint(argv[10],0,0xFFFFFFFF);
        if(argc>11) start=getint(argv[12],0,31);
        mypio.clkdiv = (uint32_t) (clock * (1 << 16));
        pio_sm_set_config(pio, sm, &mypio);
        pio_sm_init(pio, sm, start, &mypio);
        pio_sm_clear_fifos(pio,sm);
        return;
    }
    error("Syntax");
}
void fun_pio(void){
    unsigned char *tp;
    tp = checkstring(ep, "PINCTRL");
    if(tp){
        getargs(&tp,13,",");
        if(argc<3)error("Syntax");
        iret=(getint(argv[0],0,5)<<29); // no of side set pins
        if(argc>1 && *argv[2])iret|=(getint(argv[2],0,5)<<26); // no of set pins
        if(argc>3 && *argv[4])iret|=(getint(argv[4],0,29)<<20); // no of OUT pins
        if(argc>5 && *argv[6]){
            if(!toupper(*argv[6])=='G')error("Syntax");
            argv[6]++;
            if(!toupper(*argv[6])=='P')error("Syntax");
            argv[6]++;
            iret|=(getint(argv[6],0,29)<<15); // IN base
        }
        if(argc>7 && *argv[8]){
            if(!toupper(*argv[8])=='G')error("Syntax");
            argv[8]++;
            if(!toupper(*argv[8])=='P')error("Syntax");
            argv[8]++;
            iret|=(getint(argv[8],0,29)<<10); // SIDE SET base
        }
        if(argc>9 && *argv[10]){
            if(!toupper(*argv[10])=='G')error("Syntax");
            argv[10]++;
            if(!toupper(*argv[10])=='P')error("Syntax");
            argv[10]++;
            iret|=(getint(argv[10],0,29)<<5); // SET base
        }
        if(argc==13){
            if(!toupper(*argv[12])=='G')error("Syntax");
            argv[12]++;
            if(!toupper(*argv[12])=='P')error("Syntax");
            argv[12]++;
            iret|=getint(argv[12],0,29); //OUT base
        }
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "EXECCTRL");
    if(tp){
        getargs(&tp,9,",");
        if(!(argc==5 || argc==7 || argc==9))error("Syntax");
        if(!toupper(*argv[0])=='G')error("Syntax");
        argv[0]++;
        if(!toupper(*argv[0])=='P')error("Syntax");
        argv[0]++;
        iret=(getint(argv[0],0,29)<<24); // jmp pin
        iret |= pio_sm_calc_wrap(getint(argv[2],0,31), getint(argv[4],0,31));
        if(argc>=7 && *argv[6])iret|=(getint(argv[6],0,1)<<29); //SIDE_PINDIR
        if(argc==9)iret|=(getint(argv[8],0,1)<<30); // SIDE_EN
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "SHIFTCTRL");
    if(tp){
        getargs(&tp,15,",");
        if(argc<1)error("Syntax");
        iret=(getint(argv[0],0,31)<<20); // push threshold
        iret|=(getint(argv[2],0,31)<<25); // pull threshold
        if(argc>3 && *argv[4])iret|=(getint(argv[4],0,1)<<16); // autopush
        if(argc>5 && *argv[6])iret|=(getint(argv[6],0,1)<<17); // autopull
        if(argc>7 && *argv[8])iret|=(getint(argv[8],0,1)<<18); // IN_SHIFTDIR
        if(argc>9 && *argv[10])iret|=(getint(argv[10],0,1)<<19); // OUT_SHIFTDIR
        if(argc>11 && *argv[12])iret|=(getint(argv[12],0,1)<<30); // FJOIN_RX
        if(argc>13 && *argv[14])iret|=(getint(argv[14],0,1)<<31); // FJOIN_TX
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "FSTAT");
    if(tp){
        getargs(&tp,1,",");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        iret=pio->fstat; // jmp pin
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "FDEBUG");
    if(tp){
        getargs(&tp,1,",");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        iret=pio->fdebug; // jmp pin
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "FLEVEL");
    if(tp){
        getargs(&tp,5,",");
#ifdef PICOMITEVGA
        PIO pio = (getint(argv[0],1,1) ? pio1: pio0);
#else
        PIO pio = (getint(argv[0],0,1) ? pio1: pio0);
#endif
        if(argc==1)iret=pio->flevel; // jmp pin
        else {
                if(argc!=5)error("Syntax");
                int sm=getint(argv[2],0,3)*8;
                if(checkstring(argv[4],"TX"))iret=((pio->flevel)>>sm) & 0xf;
                else if(checkstring(argv[4],"RX"))iret=((pio->flevel)>>(sm+4)) & 0xf;
                else error("Syntax");
        }
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "DMA RX POINTER");
    if(tp){
        iret=dma_hw->ch[dma_rx_chan].write_addr;
        targ=T_INT;
        return;
    }
    tp = checkstring(ep, "DMA TX POINTER");
    if(tp){
        iret=dma_hw->ch[dma_tx_chan].read_addr;
        targ=T_INT;
        return;
    }
    error("Syntax");


}