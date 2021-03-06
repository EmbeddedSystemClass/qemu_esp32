// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/******************************************************************************
 * Description: A stub to make the ESP32 debuggable by GDB over the wifi 
 * at least enough to do a backtrace on panic. This gdbstub is read-only:
 * it allows inspecting the ESP32 state 
 *******************************************************************************/

#include "rom/ets_sys.h"
#include "soc/uart_reg.h"
#include "soc/io_mux_reg.h"
#include "esp_gdbstub.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "soc/cpu.h"
#include "gdb_if.h"

//Length of buffer used to reserve GDB commands. Has to be at least able to fit the G command, which
//implies a minimum size of about 320 bytes.
#define PBUFLEN 512

static unsigned char cmd[PBUFLEN];		//GDB command input buffer
static char chsum;						//Running checksum of the output packet

#define ATTR_GDBFN

extern QueueHandle_t receive_queue;
//extern QueueHandle_t send_queue;

int32_t lReceivedValue;
BaseType_t xStatus;

int gdb_socket=0;
#define IN_BUFFER_LEN 512
static unsigned char inbuf[IN_BUFFER_LEN];
static int in_buf_head = 0;
static int in_buf_received = 0;



//Receive a char from wifi, uses a frertos queue
int ATTR_GDBFN gdbRecvChar() {
	int i=0;

    if (in_buf_received==0) 
	{
		if ((i = read(gdb_socket, inbuf, IN_BUFFER_LEN)) < 0) {
				printf("%s: error reading from socket %d, closing socket\r\n", __FUNCTION__, gdb_socket);
				return 0;
			}
			inbuf[i]=0;
			printf("<-%s\n",inbuf);
		in_buf_received=i;
	}

	if (in_buf_head<in_buf_received) {
		int ret=inbuf[in_buf_head];
		in_buf_head++;
		if (in_buf_head==in_buf_received) {
			in_buf_received=0;
			in_buf_head=0;			
		}
		return(ret);
	}

	return 0;
}

#define OUT_BUFFER_LEN 800
static unsigned char outbuf[OUT_BUFFER_LEN];
static int buf_head = 0;



//Send a char to the uart.
void ATTR_GDBFN gdbSendChar(char c) {
    //while (((READ_PERI_REG(UART_STATUS_REG(0))>>UART_TXFIFO_CNT_S)&UART_TXFIFO_CNT)>=126) ;
    //WRITE_PERI_REG(UART_FIFO_REG(0), c);
    int nwrote=0;

    outbuf[buf_head]=c;
    buf_head++;
	if (c=='\n' || (c=='+') || (c=='-')  || buf_head==OUT_BUFFER_LEN) {
		if ((nwrote = write(gdb_socket, outbuf, buf_head)) < 0) {
			printf("%s: ERROR responding to gdb request. written = %d\r\n",__FUNCTION__, nwrote);
			//printf("Closing socket %d\r\n", sd);
			return;
		}
		buf_head=0;
	}   
}

//Send the start of a packet; reset checksum calculation.
static void ATTR_GDBFN gdbPacketStart() {
	chsum=0;
	gdbSendChar('$');
}

//Send a char as part of a packet
static void ATTR_GDBFN gdbPacketChar(char c) {
	if (c=='#' || c=='$' || c=='}' || c=='*') {
		gdbSendChar('}');
		gdbSendChar(c^0x20);
		chsum+=(c^0x20)+'}';
	} else {
		gdbSendChar(c);
		chsum+=c;
	}
}

//Send a string as part of a packet
static void ATTR_GDBFN gdbPacketStr(char *c) {
	while (*c!=0) {
		gdbPacketChar(*c);
		c++;
	}
}

int gdb_if_init(void) 
{
	return 0;
}
unsigned char gdb_if_getchar(void) {
	return gdbRecvChar();
}

unsigned char gdb_if_getchar_to(int timeout) {
	fd_set readset;
    int ret=1;

    const TickType_t xTicksToWait = pdMS_TO_TICKS(timeout);
	struct timeval tv;
	FD_ZERO(&readset);

    tv.tv_sec = timeout/1000;
	tv.tv_usec = 1000*(timeout-tv.tv_sec*1000);

    FD_SET(gdb_socket, &readset);

	// Only wait if no char in buffer
    if (in_buf_received==0) {
  	   ret = lwip_select(gdb_socket + 1, &readset, NULL, NULL, &tv);
	}

	if (ret==1) {
 	   return gdbRecvChar();
	}
	else {
		return 0;
	}
}



void gdb_if_putchar(unsigned char c, int flush)
{
	int  nwrote;
	if (gdb_socket>0) {
		outbuf[buf_head++] = c;
		if ((flush) || (buf_head==OUT_BUFFER_LEN)) {
			//send(gdb_if_conn, (void *)outbuf, buf_head, 0);
			outbuf[buf_head]=0;
			printf("->%s\r\n", outbuf);
			if ((nwrote = write(gdb_socket, outbuf, buf_head)) < 0) {
				printf("%s: ERROR responding to gdb request. written = %d\r\n",__FUNCTION__, nwrote);
				//printf("Closing socket %d\r\n", sd);
				return;
		    }
			if (nwrote!=buf_head) {
				printf("%s: ERROR responding to gdb request. written = %d\r\n",__FUNCTION__, nwrote);				
			}
		    buf_head=0;
		}
	}
}

// See also
//gdbSendChar(c);
//gdbPacketChar(c);

//Send a hex val as part of a packet. 'bits'/4 dictates the number of hex chars sent.
static void ATTR_GDBFN gdbPacketHex(int val, int bits) {
	char hexChars[]="0123456789abcdef";
	int i;
	for (i=bits; i>0; i-=4) {
		gdbPacketChar(hexChars[(val>>(i-4))&0xf]);
	}
}

void gdbPacketFlush() {
    int  nwrote;

    //printf("(%c)",c);
	if (gdb_socket>0) {
		// Flush buffered data
		if (buf_head>0) {
			outbuf[buf_head]=0;
			printf("-> %s\r\n", outbuf);
			if ((nwrote = write(gdb_socket, outbuf, buf_head)) < 0) {
				printf("%s: ERROR responding to gdb request. written = %d\r\n",__FUNCTION__, nwrote);
				//printf("Closing socket %d\r\n", sd);
				return;
			}
			buf_head=0;
		}
	}

}

//Finish sending a packet.
static void ATTR_GDBFN gdbPacketEnd() {
	gdbSendChar('#');
	gdbPacketHex(chsum, 8);
	gdbPacketFlush();
}

//Error states used by the routines that grab stuff from the incoming gdb packet
#define ST_ENDPACKET -1
#define ST_ERR -2
#define ST_OK -3
#define ST_CONT -4

//Grab a hex value from the gdb packet. Ptr will get positioned on the end
//of the hex string, as far as the routine has read into it. Bits/4 indicates
//the max amount of hex chars it gobbles up. Bits can be -1 to eat up as much
//hex chars as possible.
static long ATTR_GDBFN gdbGetHexVal(unsigned char **ptr, int bits) {
	int i;
	int no;
	unsigned int v=0;
	char c;
	no=bits/4;
	if (bits==-1) no=64;
	for (i=0; i<no; i++) {
		c=**ptr;
		(*ptr)++;
		if (c>='0' && c<='9') {
			v<<=4;
			v|=(c-'0');
		} else if (c>='A' && c<='F') {
			v<<=4;
			v|=(c-'A')+10;
		} else if (c>='a' && c<='f') {
			v<<=4;
			v|=(c-'a')+10;
		} else if (c=='#') {
			if (bits==-1) {
				(*ptr)--;
				return v;
			}
			return ST_ENDPACKET;
		} else {
			if (bits==-1) {
				(*ptr)--;
				return v;
			}
			return ST_ERR;
		}
	}
	return v;
}

//Swap an int into the form gdb wants it
static int ATTR_GDBFN iswap(int i) {
	int r;
	r=((i>>24)&0xff);
	r|=((i>>16)&0xff)<<8;
	r|=((i>>8)&0xff)<<16;
	r|=((i>>0)&0xff)<<24;
	return r;
}

//Read a byte from ESP32 memory.
static unsigned char ATTR_GDBFN readbyte(unsigned int p) {
	int *i=(int*)(p&(~3));
	if (p<0x20000000 || p>=0x80000000) return -1;
	return *i>>((p&3)*8);
}


//Register file in the format exp108 gdb port expects it.
//Inspired by gdb/regformats/reg-xtensa.dat
typedef struct {
	uint32_t pc;
	uint32_t a[64];
	uint32_t lbeg;
	uint32_t lend;
	uint32_t lcount;
	uint32_t sar;
	uint32_t windowbase;
	uint32_t windowstart;
	uint32_t configid0;
	uint32_t configid1;
	uint32_t ps;
	uint32_t threadptr;
	uint32_t br;
	uint32_t scompare1;
	uint32_t acclo;
	uint32_t acchi;
	uint32_t m0;
	uint32_t m1;
	uint32_t m2;
	uint32_t m3;
	uint32_t expstate;  //I'm going to assume this is exccause...
	uint32_t f64r_lo;
	uint32_t f64r_hi;
	uint32_t f64s;
	uint32_t f[16];
	uint32_t fcr;
	uint32_t fsr;

	// More registers used by qemu
	//uint32_t qemu[64];
} GdbRegFile;


GdbRegFile gdbRegFile;

/* 
//Register format as the Xtensa HAL has it:
STRUCT_FIELD (long, 4, XT_STK_EXIT,     exit)
STRUCT_FIELD (long, 4, XT_STK_PC,       pc)
STRUCT_FIELD (long, 4, XT_STK_PS,       ps)
STRUCT_FIELD (long, 4, XT_STK_A0,       a0)
[..]
STRUCT_FIELD (long, 4, XT_STK_A15,      a15)
STRUCT_FIELD (long, 4, XT_STK_SAR,      sar)
STRUCT_FIELD (long, 4, XT_STK_EXCCAUSE, exccause)
STRUCT_FIELD (long, 4, XT_STK_EXCVADDR, excvaddr)
STRUCT_FIELD (long, 4, XT_STK_LBEG,   lbeg)
STRUCT_FIELD (long, 4, XT_STK_LEND,   lend)
STRUCT_FIELD (long, 4, XT_STK_LCOUNT, lcount)
// Temporary space for saving stuff during window spill 
STRUCT_FIELD (long, 4, XT_STK_TMP0,   tmp0)
STRUCT_FIELD (long, 4, XT_STK_TMP1,   tmp1)
STRUCT_FIELD (long, 4, XT_STK_TMP2,   tmp2)
STRUCT_FIELD (long, 4, XT_STK_VPRI,   vpri)
STRUCT_FIELD (long, 4, XT_STK_OVLY,   ovly)
#endif
STRUCT_END(XtExcFrame)
*/


static void dumpHwToRegfile(XtExcFrame *frame) {
	int i;
	long *frameAregs=&frame->a0;
	gdbRegFile.pc=frame->pc;
	for (i=0; i<16; i++) gdbRegFile.a[i]=frameAregs[i];
	for (i=16; i<64; i++) gdbRegFile.a[i]=0xDEADBEEF;
	gdbRegFile.lbeg=frame->lbeg;
	gdbRegFile.lend=frame->lend;
	gdbRegFile.lcount=frame->lcount;
	gdbRegFile.sar=frame->sar;
	//All windows have been spilled to the stack by the ISR routines. The following values should indicate that.
	gdbRegFile.sar=frame->sar;
	gdbRegFile.windowbase=0; //0
	gdbRegFile.windowstart=0x1; //1
	gdbRegFile.configid0=0xdeadbeef; //ToDo
	gdbRegFile.configid1=0xdeadbeef; //ToDo
	gdbRegFile.ps=frame->ps-PS_EXCM_MASK;
	gdbRegFile.threadptr=0xdeadbeef; //ToDo
	gdbRegFile.br=0xdeadbeef; //ToDo
	gdbRegFile.scompare1=0xdeadbeef; //ToDo
	gdbRegFile.acclo=0xdeadbeef; //ToDo
	gdbRegFile.acchi=0xdeadbeef; //ToDo
	gdbRegFile.m0=0xdeadbeef; //ToDo
	gdbRegFile.m1=0xdeadbeef; //ToDo
	gdbRegFile.m2=0xdeadbeef; //ToDo
	gdbRegFile.m3=0xdeadbeef; //ToDo
	gdbRegFile.expstate=frame->exccause; //ToDo
}



typedef struct tskTaskControlBlock {
	volatile portSTACK_TYPE * pxTopOfStack;
} tskTCB;

extern tskTCB * volatile pxCurrentTCB;


static void wifiDumpHwToRegfile() {
	int i;
	long *frameAregs=pxCurrentTCB->pxTopOfStack;
	gdbRegFile.pc=*(unsigned int *)(pxCurrentTCB->pxTopOfStack+4);
	for (i=0; i<16; i++) gdbRegFile.a[i]=frameAregs[i];
	for (i=16; i<64; i++) gdbRegFile.a[i]=0xDEADBEEF;
	gdbRegFile.lbeg=0xdeadbeef;  // frame->lbeg;
	RSR(LBEG, gdbRegFile.lbeg);

	gdbRegFile.lend=0xdeadbeef; //frame->lend;
	RSR(LEND, gdbRegFile.lend);

	gdbRegFile.lcount=0xdeadbeef; //frame->lcount;
	gdbRegFile.sar=0xdeadbeef; //frame->sar;
	//All windows have been spilled to the stack by the ISR routines. The following values should indicate that.
	gdbRegFile.sar=0xdeadbeef; //frame->sar;
	RSR(SAR, gdbRegFile.sar);

	gdbRegFile.windowbase=0; //0
	gdbRegFile.windowstart=0x1; //1
	gdbRegFile.configid0=0xdeadbeef; //ToDo
	gdbRegFile.configid1=0xdeadbeef; //ToDo
	gdbRegFile.ps=0xdeadbeef; //frame->ps-PS_EXCM_MASK;

	gdbRegFile.threadptr=0xdeadbeef; //ToDo
	gdbRegFile.br=0xdeadbeef; //ToDo
	gdbRegFile.scompare1=0xdeadbeef; //ToDo
	gdbRegFile.acclo=0xdeadbeef; //ToDo
	gdbRegFile.acchi=0xdeadbeef; //ToDo
	gdbRegFile.m0=0xdeadbeef; //ToDo
	gdbRegFile.m1=0xdeadbeef; //ToDo
	gdbRegFile.m2=0xdeadbeef; //ToDo
	gdbRegFile.m3=0xdeadbeef; //ToDo
	gdbRegFile.expstate=0; //frame->exccause; //ToDo
}


//Send the reason execution is stopped to GDB.
static void sendReason() {
	//exception-to-signal mapping
	char exceptionSignal[]={4,31,11,11,2,6,8,0,6,7,0,0,7,7,7,7};
	int i=0;
	gdbPacketStart();
	gdbPacketChar('T');
	i=gdbRegFile.expstate&0x7f;
	if (i<sizeof(exceptionSignal)) {
		gdbPacketHex(exceptionSignal[i], 8); 
	} else {
		gdbPacketHex(11, 8);
	}
	gdbPacketEnd();
}

extern int ATTR_GDBFN gdb_handle_command(uint8_t * cmd, size_t len);

//Handle a command as received from GDB.
static int gdbHandleCommand(unsigned char *cmd, int len) {
	//Handle a command
	int i, j, k;
	unsigned char *data=cmd+1;
	if (cmd[0]=='g') {		//send all registers to gdb
		int *p=(int*)&gdbRegFile;
		gdbPacketStart();
		for (i=0; i<sizeof(GdbRegFile)/4; i++) gdbPacketHex(iswap(*p++), 32);
		gdbPacketEnd();
	} else if (cmd[0]=='G') {	//receive content for all registers from gdb
		int *p=(int*)&gdbRegFile;
		for (i=0; i<sizeof(GdbRegFile)/4; i++) *p++=iswap(gdbGetHexVal(&data, 32));;
		gdbPacketStart();
		gdbPacketStr("OK");
		gdbPacketEnd();
	} else if (cmd[0]=='m') {	//read memory to gdb
		i=gdbGetHexVal(&data, -1);
		data++;
		j=gdbGetHexVal(&data, -1);
		gdbPacketStart();
		for (k=0; k<j; k++) {
			gdbPacketHex(readbyte(i++), 8);
		}
		gdbPacketEnd();
	} else if (cmd[0]=='?') {	//Reply with stop reason
		sendReason();
	}/*
	 else if (cmd[0]=='q') {	//Look for qSupported
		return gdb_handle_command(cmd,len);
		}*/
	else {
		// ??? Dont think this will work so well.. 
		//return gdb_handle_command(cmd,len);
		//We don't recognize or support whatever GDB just sent us.
		gdbPacketStart();
		gdbPacketEnd();
		return ST_ERR;
	}
	return ST_OK;
}


//Lower layer: grab a command packet and check the checksum
//Calls gdbHandleCommand on the packet if the checksum is OK
//Returns ST_OK on success, ST_ERR when checksum fails, a 
//character if it is received instead of the GDB packet
//start char.
static int gdbReadCommand() {
	unsigned char c;
	unsigned char chsum=0, rchsum;
	unsigned char sentchs[2];
	int p=0;
	unsigned char *ptr;
	c=gdbRecvChar();
	if (c!='$') return c;
	while(1) {
		c=gdbRecvChar();
		if (c=='#') {	//end of packet, checksum follows
			cmd[p]=0;
			break;
		}
		chsum+=c;
		if (c=='$') {
			//Wut, restart packet?
			chsum=0;
			p=0;
			continue;
		}
		if (c=='}') {		//escape the next char
			c=gdbRecvChar();
			chsum+=c;
			c^=0x20;
		}
		cmd[p++]=c;
		if (p>=PBUFLEN) return ST_ERR;
	}
	//A # has been received. Get and check the received chsum.
	sentchs[0]=gdbRecvChar();
	sentchs[1]=gdbRecvChar();
	ptr=&sentchs[0];
	rchsum=gdbGetHexVal(&ptr, 8);
	if (rchsum!=chsum) {
		gdbSendChar('-');
		return ST_ERR;
	} else {
		gdbSendChar('+');
		return gdbHandleCommand(cmd, p);
	}
}


void set_gdb_socket(int socket) {
   gdb_socket=socket;
}

// esp_gdbstub
// This will probably not work 
void wifi_gdb_handler(int socket) {
	// TODO!! Steal regsters from stack!
	wifiDumpHwToRegfile();
	//Make sure txd/rxd are enabled
	//gpio_pullup_dis(1);
	//PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD_U0RXD);
	//PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD_U0TXD);
	gdb_socket=socket;

	sendReason();
	while(gdbReadCommand()!=ST_CONT);
	//while(1);
}



// This will probably not work, I assume processes switchin will stop on exception 
void wifi_panic_handler(XtExcFrame *frame) {
	dumpHwToRegfile(frame);
	//Make sure txd/rxd are enabled
	//gpio_pullup_dis(1);
	//PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD_U0RXD);
	//PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD_U0TXD);

	sendReason();
	while(gdbReadCommand()!=ST_CONT);
	while(1);
}


#include <stdint.h>
#include <freertos/xtensa_api.h>
#include <xtensa/corebits.h>
//#include "common/platforms/esp/src/esp_mmap.h"

extern void xt_unhandled_exception(XtExcFrame *frame);


void esp32_exception_handler(XtExcFrame *frame) {
#if 0
  if ((void *) frame->excvaddr >= MMAP_BASE) {
    int off = esp_mmap_exception_handler(frame->excvaddr, (uint8_t *) frame->pc,
                                         (long *) &frame->a2);
    if (off > 0) {
      frame->pc += off;
      return;
    }
  }
#endif
  xt_unhandled_exception(frame);
}

void set_all_exception_handlers(void) {
	int i=0;
	for (i=0;i<64;i++) {
       xt_set_exception_handler(i, wifi_panic_handler);
	}
}
