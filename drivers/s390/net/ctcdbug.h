/*
 *
 * linux/drivers/s390/net/ctcdbug.h ($Revision: 1.2 $)
 *
 * CTC / ESCON network driver - s390 dbf exploit.
 *
 * Copyright 2000,2003 IBM Corporation
 *
 *    Author(s): Original Code written by
 *			  Peter Tiedemann (ptiedem@de.ibm.com)
 *
 *    $Revision: 1.2 $	 $Date: 2004/07/15 16:03:08 $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <asm/debug.h>
/**
 * Debug Facility stuff
 */
#define CTC_DBF_SETUP_NAME "ctc_setup"
#define CTC_DBF_SETUP_LEN 16
#define CTC_DBF_SETUP_INDEX 3
#define CTC_DBF_SETUP_NR_AREAS 1
#define CTC_DBF_SETUP_LEVEL 3

#define CTC_DBF_DATA_NAME "ctc_data"
#define CTC_DBF_DATA_LEN 128
#define CTC_DBF_DATA_INDEX 3
#define CTC_DBF_DATA_NR_AREAS 1
#define CTC_DBF_DATA_LEVEL 2

#define CTC_DBF_TRACE_NAME "ctc_trace"
#define CTC_DBF_TRACE_LEN 16
#define CTC_DBF_TRACE_INDEX 2
#define CTC_DBF_TRACE_NR_AREAS 2
#define CTC_DBF_TRACE_LEVEL 3

#define DBF_TEXT(name,level,text) \
	do { \
		debug_text_event(dbf_##name,level,text); \
	} while (0)

#define DBF_HEX(name,level,addr,len) \
	do { \
		debug_event(dbf_##name,level,(void*)(addr),len); \
	} while (0)

extern DEFINE_PER_CPU(char[256], dbf_txt_buf);
extern debug_info_t *dbf_setup;
extern debug_info_t *dbf_data;
extern debug_info_t *dbf_trace;


#define DBF_TEXT_(name,level,text...)				\
	do {								\
		char* dbf_txt_buf = get_cpu_var(dbf_txt_buf);	\
		sprintf(dbf_txt_buf, text);			  	\
		debug_text_event(dbf_##name,level,dbf_txt_buf);	\
		put_cpu_var(dbf_txt_buf);				\
	} while (0)

#define DBF_SPRINTF(name,level,text...) \
	do { \
		debug_sprintf_event(dbf_trace, level, ##text ); \
		debug_sprintf_event(dbf_trace, level, text ); \
	} while (0)


int register_dbf_views(void);

void unregister_dbf_views(void);

/**
 * some more debug stuff
 */

#define HEXDUMP16(importance,header,ptr) \
PRINT_##importance(header "%02x %02x %02x %02x  %02x %02x %02x %02x  " \
		   "%02x %02x %02x %02x  %02x %02x %02x %02x\n", \
		   *(((char*)ptr)),*(((char*)ptr)+1),*(((char*)ptr)+2), \
		   *(((char*)ptr)+3),*(((char*)ptr)+4),*(((char*)ptr)+5), \
		   *(((char*)ptr)+6),*(((char*)ptr)+7),*(((char*)ptr)+8), \
		   *(((char*)ptr)+9),*(((char*)ptr)+10),*(((char*)ptr)+11), \
		   *(((char*)ptr)+12),*(((char*)ptr)+13), \
		   *(((char*)ptr)+14),*(((char*)ptr)+15)); \
PRINT_##importance(header "%02x %02x %02x %02x  %02x %02x %02x %02x  " \
		   "%02x %02x %02x %02x  %02x %02x %02x %02x\n", \
		   *(((char*)ptr)+16),*(((char*)ptr)+17), \
		   *(((char*)ptr)+18),*(((char*)ptr)+19), \
		   *(((char*)ptr)+20),*(((char*)ptr)+21), \
		   *(((char*)ptr)+22),*(((char*)ptr)+23), \
		   *(((char*)ptr)+24),*(((char*)ptr)+25), \
		   *(((char*)ptr)+26),*(((char*)ptr)+27), \
		   *(((char*)ptr)+28),*(((char*)ptr)+29), \
		   *(((char*)ptr)+30),*(((char*)ptr)+31));

static inline void
hex_dump(unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (i && !(i % 16))
			printk("\n");
		printk("%02x ", *(buf + i));
	}
	printk("\n");
}

