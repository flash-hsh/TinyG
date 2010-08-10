/*
 * xio_rs485.c 	- RS-485 device driver for xmega family
 * 				- works with avr-gcc stdio library
 *
 * Part of TinyG project
 * Copyright (c) 2010 Alden S. Hart, Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, 
 * either version 3 of the License, or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with TinyG  
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>			// needed for blocking character reads

#include "xio.h"
#include "xmega_interrupts.h"
//#include "tinyg.h"				// needed for TG_ return codes, or provide your own
#include "signals.h"			// application specific signal handlers

// necessary structures
extern struct xioDEVICE ds[XIO_DEV_COUNT];		// ref top-level device structs
extern struct xioUSART us[XIO_DEV_USART_COUNT];	// ref USART extended IO structs
#define RS ds[XIO_DEV_RS485]					// device struct accessoor
#define RSu us[XIO_DEV_RS485]					// usart extended struct accessor

// local functions
static int _getc_char(void);		// getc character dispatch routines
static int _getc_NEWLINE(void);
static int _getc_SEMICOLON(void);
static int _getc_DELETE(void);

static int _readln_char(void);		// readln character dispatch routines
static int _readln_NEWLINE(void);
static int _readln_SEMICOLON(void);
static int _readln_DELETE(void);

static int _sig_KILL(void);			// vestigal stubs for signals
static int _sig_PAUSE(void);
static int _sig_RESUME(void);

/* 
 * USB_RX_ISR - USB receiver interrupt (RX)
 *
 *	RX buffer states can be one of:
 *	- buffer has space	(CTS should be asserted)
 *	- buffer is full 	(CTS should be not_asserted)
 *	- buffer becomes full with this character (write char and assert CTS)
 *
 *	Flow control is not implemented. Need to work RTS line.
 *	Flow control should cut off at high water mark, re-enable at low water mark
 *	High water mark should have about 4 - 8 bytes left in buffer (~95% full) 
 *	Low water mark about 50% full
 *
 * 	See end notes in xio.h for a discussion of how the circular bufers work
 */

ISR(RS485_RX_ISR_vect)	//ISR(USARTC1_RXC_vect)	// serial port C0 RX interrupt 
{
	uint8_t c = RSu.usart->DATA;				// can only read DATA once

	// trap signals - do not insert into RX queue
	if (c == ETX) {								// trap ^c signal
		RS.sig = XIO_SIG_KILL;					// set signal value
		signal_etx();							// call app-specific signal handler
		return;
	}

	// normal path
	if ((--RSu.rx_buf_head) == 0) { 			// advance buffer head with wrap
		RSu.rx_buf_head = RX_BUFFER_SIZE-1;		// -1 avoids the off-by-one error
	}
	if (RSu.rx_buf_head != RSu.rx_buf_tail) {	// write char unless buffer full
		RSu.rx_buf[RSu.rx_buf_head] = c;		// (= USARTC0.DATA;)
		return;
	}
	// buffer-full handling
	if ((++RSu.rx_buf_head) > RX_BUFFER_SIZE-1) { // reset the head
		RSu.rx_buf_head = 1;
	}
	// activate flow control here or before it gets to this level
}

/*
 * xio_rs485_queue_RX_char() - fake ISR to put a char in the RX buffer
 */

void xio_rs485_queue_RX_char(const char c)
{
	// trap signals - do not insert into RX queue
	if (c == ETX) {								// trap ^c signal
		RS.sig = XIO_SIG_KILL;					// set signal value
		signal_etx();							// call app-specific signal handler
		return;
	}

	// normal path
	if ((--RSu.rx_buf_head) == 0) { 			// wrap condition
		RSu.rx_buf_head = RX_BUFFER_SIZE-1;		// -1 avoids the off-by-one error
	}
	if (RSu.rx_buf_head != RSu.rx_buf_tail) {	// write char unless buffer full
		RSu.rx_buf[RSu.rx_buf_head] = c;		// FAKE INPUT DATA
		return;
	}
	// buffer-full handling
	if ((++RSu.rx_buf_head) > RX_BUFFER_SIZE-1) { // reset the head
		RSu.rx_buf_head = 1;
	}
}

/*
 * xio_rs485_queue_RX_string() - fake ISR to put a string in the RX buffer
 */

void xio_rs485_queue_RX_string(char *buf)
{
	char c;
	uint8_t i=0;

	while ((c = buf[i++]) != NUL) {
		xio_rs485_queue_RX_char(c);
	}
}

/* 
 * USB_TX_ISR - USB transmitter interrupt (TX)
 *
 * The TX interrupt dilemma: TX interrupts occur when the USART DATA register is 
 * empty (and the ISR must disable interrupts when nothing's left to read, or they 
 * keep firing). If the TX buffer is completely empty (TXCIF is set) then enabling
 * interrupts does no good. The USART won't interrupt and the TX circular buffer 
 * never empties.
 *
 * So we define a dequeue function that can be called from either the ISR or be 
 * called from the putc() if it detects TXCIfr. Care should be taken to make sure 
 * these two callers don't collide (like only enabling interrupts in putc() AFTER
 * the dequeue has occurred).
 */

ISR(RS485_TX_ISR_vect)		//ISR(USARTC1_DRE_vect)	// USARTC0 data register empty
{
	if (RSu.tx_buf_head == RSu.tx_buf_tail) {	// buffer empty - disable ints
		RSu.usart->CTRLA = CTRLA_RXON_TXOFF;	// doesn't work if you just &= it
//		PMIC_DisableLowLevel(); 				// disable USART TX interrupts
		return;
	}
	if (!TX_MUTEX(RS.flags)) {
		if (--(RSu.tx_buf_tail) == 0) {			// advance tail and wrap if needed
			RSu.tx_buf_tail = TX_BUFFER_SIZE-1;	// -1 avoids off-by-one error (OBOE)
		}
		RSu.usart->DATA = RSu.tx_buf[RSu.tx_buf_tail];	// write char to TX DATA reg
	}
}

/*
 *	xio_open_rs485() - all this does is return the stdio fdev handle
 */

struct __file * xio_open_rs485()
{
	return(RS.fdev);
}

/*
 *	xio_setflags_rs485() - check and set control flags for device
 */

int xio_setflags_rs485(const uint16_t control)
{
	xio_setflags(XIO_DEV_RS485, control);
	return (XIO_OK);									// for now it's always OK
}

/* 
 * xio_putc_rs485() - blocking and nonblocking char writer for USB device 
 *
 *	Compatible with stdio system - may be bound to a FILE handle
 *
 *	Note: Originally I had the routine advancing the buffer head and comparing 
 *		  against the buffer tail to detect buffer full (it would sleep if the 
 *		  buffer was full). This unfortunately collides with the buffer empty 
 *		  detection in the dequeue routine - causing the dequeing ISR to lock up
 *		  when the buffer was full. Using a local next_tx_buffer_head prevents this
 */

int xio_putc_rs485(const char c, FILE *stream)
{
	if ((RSu.next_tx_buf_head = RSu.tx_buf_head-1) == 0) { // advance head w/wrap
		RSu.next_tx_buf_head = TX_BUFFER_SIZE-1;	// -1 avoids the off-by-one error
	}
	while(RSu.next_tx_buf_head == RSu.tx_buf_tail) { // TX buffer full. sleep or ret
		if (BLOCKING(RS.flags)) {
			sleep_mode();
		} else {
			RS.sig = XIO_SIG_EAGAIN;
			return(_FDEV_ERR);
		}
	};
	// write to data register
	RSu.tx_buf_head = RSu.next_tx_buf_head;			// accept next buffer head value
	RSu.tx_buf[RSu.tx_buf_head] = c;				// ...and write char to buffer

	if (CRLF(RS.flags) && (c == '\n')) {			// detect LF and add a CR
		return xio_putc_rs485('\r', stream);		// recursion.
	}

	// dequeue the buffer if DATA register is ready
	if (RSu.usart->STATUS & 0x20) {
		if (RSu.tx_buf_head == RSu.tx_buf_tail) {	// buf may be empty if IRQ got it
			return (0);
		}
		RS.flags |= XIO_FLAG_TX_MUTEX_bm;		// claim mutual exclusion from ISR
		if (--(RSu.tx_buf_tail) == 0) {			// advance tail and wrap if needed
			RSu.tx_buf_tail = TX_BUFFER_SIZE-1;	// -1 avoids off-by-one error (OBOE)
		}
		RSu.usart->DATA = RSu.tx_buf[RSu.tx_buf_tail];// write char to TX DATA reg
		RS.flags &= ~XIO_FLAG_TX_MUTEX_bm;		// release mutual exclusion lock
	}
	// enable interrupts regardless
	RSu.usart->CTRLA = CTRLA_RXON_TXON;			// won't work if you just |= it
	PMIC_EnableLowLevel(); 						// enable USART TX interrupts
	sei();										// enable global interrupts

	return (0);	// 0 = OK
}

/* 
 * xio_getc_rs485 character dispatch functions
 *
 *  Functions take no input but use static RS.c, RS.signals, and others
 *  Returns c (may be translated depending on the function)
 */

static int (*getcFuncs[])(void) PROGMEM = { 	// use if you want it in FLASH
//static int (*getcFuncs[])(void) = {			// ALTERNATE: put table in SRAM

							// dec  hex symbol
		_getc_NEWLINE, 		//	0	00	NUL	(Null char)		(TREATED AS NEWLINE)
		_getc_char, 		//	1	01	SOH	(Start of Header)
		_getc_char, 		//	2	02	STX	(Start of Text)
		_sig_KILL,		 	//	3	03	ETX (End of Text) ^c
		_getc_char, 		//	4	04	EOT	(End of Transmission)
		_getc_char, 		//	5	05	ENQ	(Enquiry)
		_getc_char, 		//	6	06	ACK	(Acknowledgment)
		_getc_char, 		//	7	07	BEL	(Bell)
		_getc_DELETE, 		//	8	08	BS	(Backspace)
		_getc_char, 		//	9	09	HT	(Horizontal Tab)
		_getc_NEWLINE, 		//	10	0A	LF	(Line Feed)
		_getc_char, 		//	11	0B	VT	(Vertical Tab)
		_getc_char, 		//	12	0C	FF	(Form Feed)
		_getc_NEWLINE, 		//	13	0D	CR	(Carriage Return)
		_getc_char,			//	14	0E	SO	(Shift Out)
		_getc_char, 		//	15	0F	SI	(Shift In)
		_getc_char, 		//	16	10	DLE	(Data Link Escape)
		_sig_RESUME, 		//	17	11	DC1 (XON) (Device Control 1) ^q	
		_getc_char, 		//	18	12	DC2	(Device Control 2)
		_sig_PAUSE,		 	//	19	13	DC3 (XOFF)(Device Control 3) ^s	
		_getc_char, 		//	20	14	DC4	(Device Control 4)
		_getc_char, 		//	21	15	NAK (Negativ Acknowledgemnt)	
		_getc_char, 		//	22	16	SYN	(Synchronous Idle)
		_getc_char, 		//	23	17	ETB	(End of Trans. Block)
		_sig_KILL,	 		//	24	18	CAN	(Cancel) ^x
		_getc_char, 		//	25	19	EM	(End of Medium)
		_getc_char, 		//	26	1A	SUB	(Substitute)
		_sig_KILL, 			//	27	1B	ESC	(Escape)
		_getc_char, 		//	28	1C	FS	(File Separator)
		_getc_char, 		//	29	1D	GS	(Group Separator)
		_getc_char, 		//	30	1E	RS  (Reqst to Send)(Record Sep.)	
		_getc_char, 		//	31	1F	US	(Unit Separator)
		_getc_char, 		//	32	20	SP	(Space)
		_getc_char, 		//	33	21	!	(exclamation mark)
		_getc_char, 		//	34	22	,	(double quote)	
		_getc_char, 		//	35	23	#	(number sign)
		_getc_char, 		//	36	24	$	(dollar sign)
		_getc_char, 		//	37	25	%	(percent)
		_getc_char, 		//	38	26	&	(ampersand)
		_getc_char, 		//	39	27	'	(single quote)
		_getc_char, 		//	40	28	(	(left/open parenthesis)
		_getc_char, 		//	41	29	)	(right/closing parenth.)
		_getc_char, 		//	42	2A	*	(asterisk)
		_getc_char, 		//	43	2B	+	(plus)
		_getc_char, 		//	44	2C		(comma)
		_getc_char,	 		//	45	2D	-	(minus or dash)
		_getc_char, 		//	46	2E	.	(dot)
		_getc_char,	 		//	47	2F	/	(forward slash)
		_getc_char, 		//	48	30	0	
		_getc_char, 		//	49	31	1	
		_getc_char, 		//	50	32	2	
		_getc_char, 		//	51	33	3	
		_getc_char, 		//	52	34	4	
		_getc_char, 		//	53	35	5	
		_getc_char, 		//	54	36	6	
		_getc_char, 		//	55	37	7	
		_getc_char, 		//	56	38	8	
		_getc_char, 		//	57	39	9	
		_getc_char, 		//	58	3A	:	(colon)
		_getc_SEMICOLON,	//	59	3B	;	(semi-colon)
		_getc_char, 		//	60	3C	<	(less than)
		_getc_char, 		//	61	3D	=	(equal sign)
		_getc_char, 		//	62	3E	>	(greater than)
		_getc_char, 		//	63	3F	?	(question mark)
		_getc_char, 		//	64	40	@	(AT symbol)
		_getc_char,			//	65	41	A	
		_getc_char,			//	66	42	B	
		_getc_char,			//	67	43	C	
		_getc_char,			//	68	44	D	
		_getc_char,			//	69	45	E	
		_getc_char,			//	70	46	F	
		_getc_char,			//	71	47	G	
		_getc_char,			//	72	48	H	
		_getc_char,			//	73	49	I	
		_getc_char,			//	74	4A	J	
		_getc_char,			//	75	4B	K	
		_getc_char,			//	76	4C	L	
		_getc_char,			//	77	4D	M	
		_getc_char,			//	78	4E	N	
		_getc_char,			//	79	4F	O	
		_getc_char,			//	80	50	P	
		_getc_char,			//	81	51	Q	
		_getc_char,			//	82	52	R	
		_getc_char,			//	83	53	S	
		_getc_char,			//	84	54	T	
		_getc_char,			//	85	55	U	
		_getc_char,			//	86	56	V	
		_getc_char,			//	87	57	W	
		_getc_char,			//	88	58	X	
		_getc_char,			//	89	59	Y	
		_getc_char,			//	90	5A	Z	
		_getc_char,			//	91	5B	[	(left/opening bracket)
		_getc_char,			//	92	5C	\	(back slash)
		_getc_char,			//	93	5D	]	(right/closing bracket)
		_getc_char,			//	94	5E	^	(caret/circumflex)
		_getc_char,			//	95	5F	_	(underscore)
		_getc_char,			//	96	60	`	
		_getc_char,			//	97	61	a	
		_getc_char,			//	98	62	b	
		_getc_char,			//	99	63	c	
		_getc_char,			//	100	64	d	
		_getc_char,			//	101	65	e	
		_getc_char,			//	102	66	f	
		_getc_char,			//	103	67	g	
		_getc_char,			//	104	68	h	
		_getc_char,			//	105	69	i	
		_getc_char,			//	106	6A	j	
		_getc_char,			//	107	6B	k	
		_getc_char,			//	108	6C	l	
		_getc_char,			//	109	6D	m	
		_getc_char,			//	110	6E	n	
		_getc_char,			//	111	6F	o	
		_getc_char,			//	112	70	p	
		_getc_char,			//	113	71	q	
		_getc_char,			//	114	72	r	
		_getc_char,			//	115	73	s	
		_getc_char,			//	116	74	t	
		_getc_char,			//	117	75	u	
		_getc_char,			//	118	76	v	
		_getc_char,			//	119	77	w	
		_getc_char,			//	120	78	x	
		_getc_char,			//	121	79	y	
		_getc_char,			//	122	7A	z	
		_getc_char,			//	123	7B	{	(left/opening brace)
		_getc_char,			//	124	7C	|	(vertical bar)
		_getc_char,			//	125	7D	}	(right/closing brace)
		_getc_char,			//	126	7E	~	(tilde)
		_getc_DELETE		//	127	7F	DEL	(delete)
};

/*
 *  xio_getc_rs485() - char reader for USB device
 *
 *	Compatible with stdio system - may be bound to a FILE handle
 *
 *  Get next character from RX buffer.
 *	See "Notes on the circular buffers" at end of xio.h for buffer details.
 *
 *	This routine returns a single character from the RX buffer to the caller.
 *	It's typically called by fgets() and is useful for single-threaded IO cases.
 *	Cases with multiple concurrent IO streams may want to use the readln() function
 *	which is incompatible with the stdio system. 
 *
 *  Flags that affect behavior:
 *
 *  BLOCKING behaviors
 *	 	- execute blocking or non-blocking read depending on controls
 *		- return character or -1 & XIO_SIG_WOULDBLOCK if non-blocking
 *		- return character or sleep() if blocking
 *
 *  ECHO behaviors
 *		- if ECHO is enabled echo character to stdout
 *		- echo all line termination chars as newlines ('\n')
 *		- Note: putc is responsible for expanding newlines to <cr><lf> if needed
 *
 *  SPECIAL CHARACTERS 
 *		- special characters such as EOL and control chars are handled by the
 *		  character helper routines. See them for behaviors
 */

int xio_getc_rs485(FILE *stream)
{
	while (RSu.rx_buf_head == RSu.rx_buf_tail) {// RX ISR buffer empty
		if (BLOCKING(RS.flags)) {
			sleep_mode();
		} else {
			RS.sig = XIO_SIG_EAGAIN;
			return(_FDEV_ERR);
		}
	}
	if (--(RSu.rx_buf_tail) == 0) {				// advance RX tail (RXQ read ptr)
		RSu.rx_buf_tail = RX_BUFFER_SIZE-1;		// -1 avoids off-by-one error (OBOE)
	}
	RS.c = (RSu.rx_buf[RSu.rx_buf_tail] & 0x007F);	// get char from RX & mask MSB
	// 	call action procedure from dispatch table in FLASH (see xio.h for typedef)
	return (((fptr_int_void)(pgm_read_word(&getcFuncs[RS.c])))());
	//return (getcFuncs[c]()); // call action procedure from dispatch table in RAM
}

/* xio_rs485_getc helper routines */

static int _getc_char(void)
{
	if (ECHO(RS.flags)) xio_putc_rs485(RS.c, stdout);
	return(RS.c);
}

static int _getc_NEWLINE(void)		// convert CRs and LFs to newlines if line mode
{
	if (LINEMODE(RS.flags)) RS.c = '\n';
	if (ECHO(RS.flags)) xio_putc_rs485(RS.c, stdout);
	return(RS.c);
}

static int _getc_SEMICOLON(void)
{
	if (SEMICOLONS(RS.flags)) {
		return (_getc_NEWLINE());			// if semi mode treat as an EOL
	} 
	return (_getc_char());					// else treat as any other character
}

static int _getc_DELETE(void)				// can't handle a delete very well
{
	RS.sig = XIO_SIG_DELETE;
	return(_FDEV_ERR);
}

/* 
 * xio_readln_rs485 character dispatch functions
 *
 *  Functions take no input but use static 'c', RS.signals, and others
 *  Returns c (may be translated depending on the function)
 */

static int (*readlnFuncs[])(void) PROGMEM = { 	// use if you want it in FLASH
//static int (*readlnFuncs[])(void) = {		// ALTERNATE: put table in SRAM

							// dec  hex symbol
		_readln_NEWLINE, 	//	0	00	NUL	(Null char)  	(TREAT AS NEWLINE)
		_readln_char, 		//	1	01	SOH	(Start of Header)
		_readln_char, 		//	2	02	STX	(Start of Text)
		_sig_KILL,	 		//	3	03	ETX (End of Text) ^c
		_readln_char, 		//	4	04	EOT	(End of Transmission)
		_readln_char, 		//	5	05	ENQ	(Enquiry)
		_readln_char, 		//	6	06	ACK	(Acknowledgment)
		_readln_char, 		//	7	07	BEL	(Bell)
		_readln_DELETE, 	//	8	08	BS	(Backspace)
		_readln_char, 		//	9	09	HT	(Horizontal Tab)
		_readln_NEWLINE, 	//	10	0A	LF	(Line Feed)
		_readln_char, 		//	11	0B	VT	(Vertical Tab)
		_readln_char, 		//	12	0C	FF	(Form Feed)
		_readln_NEWLINE, 	//	13	0D	CR	(Carriage Return)
		_readln_char,		//	14	0E	SO	(Shift Out)
		_readln_char, 		//	15	0F	SI	(Shift In)
		_readln_char, 		//	16	10	DLE	(Data Link Escape)
		_sig_RESUME, 		//	17	11	DC1 (XON) (Device Control 1) ^q	
		_readln_char, 		//	18	12	DC2	(Device Control 2)
		_sig_PAUSE, 		//	19	13	DC3 (XOFF)(Device Control 3) ^s	
		_readln_char, 		//	20	14	DC4	(Device Control 4)
		_readln_char, 		//	21	15	NAK (Negativ Acknowledgemnt)	
		_readln_char, 		//	22	16	SYN	(Synchronous Idle)
		_readln_char, 		//	23	17	ETB	(End of Trans. Block)
		_sig_KILL,		 	//	24	18	CAN	(Cancel) ^x
		_readln_char, 		//	25	19	EM	(End of Medium)
		_readln_char, 		//	26	1A	SUB	(Substitute)
		_sig_KILL, 			//	27	1B	ESC	(Escape)
		_readln_char, 		//	28	1C	FS	(File Separator)
		_readln_char, 		//	29	1D	GS	(Group Separator)
		_readln_char, 		//	30	1E	RS  (Reqst to Send)(Record Sep.)	
		_readln_char, 		//	31	1F	US	(Unit Separator)
		_readln_char, 		//	32	20	SP	(Space)
		_readln_char, 		//	33	21	!	(exclamation mark)
		_readln_char, 		//	34	22	,	(double quote)	
		_readln_char, 		//	35	23	#	(number sign)
		_readln_char, 		//	36	24	$	(dollar sign)
		_readln_char, 		//	37	25	%	(percent)
		_readln_char, 		//	38	26	&	(ampersand)
		_readln_char, 		//	39	27	'	(single quote)
		_readln_char, 		//	40	28	(	(left/open parenthesis)
		_readln_char, 		//	41	29	)	(right/closing parenth.)
		_readln_char, 		//	42	2A	*	(asterisk)
		_readln_char, 		//	43	2B	+	(plus)
		_readln_char, 		//	44	2C		(comma)
		_readln_char,	 	//	45	2D	-	(minus or dash)
		_readln_char, 		//	46	2E	.	(dot)
		_readln_char,	 	//	47	2F	/	(forward slash)
		_readln_char, 		//	48	30	0	
		_readln_char, 		//	49	31	1	
		_readln_char, 		//	50	32	2	
		_readln_char, 		//	51	33	3	
		_readln_char, 		//	52	34	4	
		_readln_char, 		//	53	35	5	
		_readln_char, 		//	54	36	6	
		_readln_char, 		//	55	37	7	
		_readln_char, 		//	56	38	8	
		_readln_char, 		//	57	39	9	
		_readln_char, 		//	58	3A	:	(colon)
		_readln_SEMICOLON, //	59	3B	;	(semi-colon)
		_readln_char, 		//	60	3C	<	(less than)
		_readln_char, 		//	61	3D	=	(equal sign)
		_readln_char, 		//	62	3E	>	(greater than)
		_readln_char, 		//	63	3F	?	(question mark)
		_readln_char, 		//	64	40	@	(AT symbol)
		_readln_char,		//	65	41	A	
		_readln_char,		//	66	42	B	
		_readln_char,		//	67	43	C	
		_readln_char,		//	68	44	D	
		_readln_char,		//	69	45	E	
		_readln_char,		//	70	46	F	
		_readln_char,		//	71	47	G	
		_readln_char,		//	72	48	H	
		_readln_char,		//	73	49	I	
		_readln_char,		//	74	4A	J	
		_readln_char,		//	75	4B	K	
		_readln_char,		//	76	4C	L	
		_readln_char,		//	77	4D	M	
		_readln_char,		//	78	4E	N	
		_readln_char,		//	79	4F	O	
		_readln_char,		//	80	50	P	
		_readln_char,		//	81	51	Q	
		_readln_char,		//	82	52	R	
		_readln_char,		//	83	53	S	
		_readln_char,		//	84	54	T	
		_readln_char,		//	85	55	U	
		_readln_char,		//	86	56	V	
		_readln_char,		//	87	57	W	
		_readln_char,		//	88	58	X	
		_readln_char,		//	89	59	Y	
		_readln_char,		//	90	5A	Z	
		_readln_char,		//	91	5B	[	(left/opening bracket)
		_readln_char,		//	92	5C	\	(back slash)
		_readln_char,		//	93	5D	]	(right/closing bracket)
		_readln_char,		//	94	5E	^	(caret/circumflex)
		_readln_char,		//	95	5F	_	(underscore)
		_readln_char,		//	96	60	`	
		_readln_char,		//	97	61	a	
		_readln_char,		//	98	62	b	
		_readln_char,		//	99	63	c	
		_readln_char,		//	100	64	d	
		_readln_char,		//	101	65	e	
		_readln_char,		//	102	66	f	
		_readln_char,		//	103	67	g	
		_readln_char,		//	104	68	h	
		_readln_char,		//	105	69	i	
		_readln_char,		//	106	6A	j	
		_readln_char,		//	107	6B	k	
		_readln_char,		//	108	6C	l	
		_readln_char,		//	109	6D	m	
		_readln_char,		//	110	6E	n	
		_readln_char,		//	111	6F	o	
		_readln_char,		//	112	70	p	
		_readln_char,		//	113	71	q	
		_readln_char,		//	114	72	r	
		_readln_char,		//	115	73	s	
		_readln_char,		//	116	74	t	
		_readln_char,		//	117	75	u	
		_readln_char,		//	118	76	v	
		_readln_char,		//	119	77	w	
		_readln_char,		//	120	78	x	
		_readln_char,		//	121	79	y	
		_readln_char,		//	122	7A	z	
		_readln_char,		//	123	7B	{	(left/opening brace)
		_readln_char,		//	124	7C	|	(vertical bar)
		_readln_char,		//	125	7D	}	(right/closing brace)
		_readln_char,		//	126	7E	~	(tilde)
		_readln_DELETE		//	127	7F	DEL	(delete)
};

/* 
 *	xio_readln_rs485() - main loop task for USB device
 *
 *	Non-blocking, run-to-completion task for handling incoming data from USB port.
 *
 *	Runs non-blocking (port scan) and retains line context across calls.
 *  Should be called each time a character is received by the RX ISR, but can be 
 *	called randomly and multiple times without damage. 
 *
 *	Reads a complete (newline terminated) line from the USB device and invokes
 *	the registered line handler function once line is complete.
 *
 *	Traps signals (e.g. ^c) and dispatches to registered signal handler(s).
 *	Signals leave the line buffer intact, so it either can continue to be collected
 *	of it should be wiped.
 *
 *  Performs the following functions:
 *		- read character from RX buffer
 *		- strip signals and dispatch them to signal handler
 *		- collect complete line and pass to line handler function
 *		- trap buffer overflow condition and return an error
 *
 *	Note: LINEMODE flag is ignored. It's ALWAYS LINEMODE here.
 */

int xio_readln_rs485(char *buf, uint8_t size)
{
	if (!IN_LINE(RS.flags)) {					// first time thru initializations
		RS.len = 0;								// zero buffer
		RS.status = 0;
		RS.size = size;
		RS.buf = buf;
		RS.sig = XIO_SIG_OK;					// no signal action
		RS.flags |= XIO_FLAG_IN_LINE_bm;		// yes, we are busy getting a line
	}
	if (RSu.rx_buf_head == RSu.rx_buf_tail) {	// RX ISR buffer empty
//		RS.sig = XIO_SIG_EAGAIN;
		return(XIO_EAGAIN);
	}
	if (--(RSu.rx_buf_tail) == 0) {				// advance RX tail (RXQ read ptr)
		RSu.rx_buf_tail = RX_BUFFER_SIZE-1;		// -1 avoids off-by-one error (OBOE)
	}
	RS.c = (RSu.rx_buf[RSu.rx_buf_tail] & 0x007F);	// get char from RX Q & mask MSB
	return (((fptr_int_void)(pgm_read_word(&readlnFuncs[RS.c])))()); // dispatch char
}

/* xio_rs485_readln helper routines */

static int _readln_char(void)
{
	if (RS.len > RS.size) {						// trap buffer overflow
		RS.sig = XIO_SIG_EOL;
		RS.buf[RS.size] = NUL;					// RS.len is zero based
		return (XIO_BUFFER_FULL_NON_FATAL);
	}
	RS.buf[RS.len++] = RS.c;
	if (ECHO(RS.flags)) xio_putc_rs485(RS.c, stdout);// conditional echo
	return (XIO_EAGAIN);							// line is still in process
}

static int _readln_NEWLINE(void)				// handle valid newline char
{
	RS.sig = XIO_SIG_EOL;
	RS.buf[RS.len] = NUL;
	RS.flags &= ~XIO_FLAG_IN_LINE_bm;			// clear in-line state (reset)
	if (ECHO(RS.flags)) xio_putc_rs485('\n',stdout);// echo a newline
	return 0;
}

static int _readln_SEMICOLON(void)				// semicolon is a conditional newline
{
	if (SEMICOLONS(RS.flags)) {
		return (_readln_NEWLINE());				// if semi-mode treat as an EOL
	} else {
		return (_readln_char());				// else treat as any other character
	}
}

static int _readln_DELETE(void)
{
	if (--RS.len >= 0) {
		if (ECHO(RS.flags)) xio_putc_rs485(RS.c, stdout);
	} else {
		RS.len = 0;
	}
	return (XIO_EAGAIN);							// line is still in process
}

/*
 * Signal handlers. These are vestigal stubs that have no effect.
 */

static int _sig_KILL(void)
{
	RS.sig = XIO_SIG_KILL;
	return(_FDEV_ERR);
}

static int _sig_PAUSE(void)
{
	RS.sig = XIO_SIG_PAUSE;
	return(_FDEV_ERR);
}

static int _sig_RESUME(void)
{
	RS.sig = XIO_SIG_RESUME;
	return(_FDEV_ERR);
}
