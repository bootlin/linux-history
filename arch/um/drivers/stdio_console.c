/* 
 * Copyright (C) 2000, 2001 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/config.h"
#include "linux/posix_types.h"
#include "linux/tty.h"
#include "linux/tty_flip.h"
#include "linux/types.h"
#include "linux/major.h"
#include "linux/kdev_t.h"
#include "linux/console.h"
#include "linux/string.h"
#include "linux/sched.h"
#include "linux/list.h"
#include "linux/init.h"
#include "linux/interrupt.h"
#include "linux/slab.h"
#include "asm/current.h"
#include "asm/softirq.h"
#include "asm/hardirq.h"
#include "asm/irq.h"
#include "stdio_console.h"
#include "line.h"
#include "chan_kern.h"
#include "user_util.h"
#include "kern_util.h"
#include "irq_user.h"
#include "init.h"
#include "2_5compat.h"

#define MAX_TTYS (8)

static struct tty_driver console_driver;

static struct chan_ops init_console_ops = {
	init : 		NULL,
	open : 		NULL,
	close :		NULL,
	read : 		NULL,
	write :		NULL,
	console_write :	generic_write,
	window_size :	NULL,
	free : 		NULL,
	winch:		0,
};

static struct chan init_console_chan = {
	list : 		{ },
	primary :	1,
	input :		0,
	output :	1,
	opened :	1,
	fd :		1,
	pri :		INIT_STATIC,
	ops :		&init_console_ops,
	data :		NULL
};

void stdio_announce(char *dev_name, int dev)
{
	printk(KERN_INFO "Virtual console %d assigned device '%s'\n", dev,
	       dev_name);
}

static struct chan_opts opts = {
	announce: 	stdio_announce,
	xterm_title:	"Virtual Console #%d",
	raw:		1,
	tramp_stack :	0,
};

static struct line_driver driver = {
	name :			"UML console",
	devfs_name :		"vc/%d",
	major :			TTY_MAJOR,
	minor_start :		0,
	type :		 	TTY_DRIVER_TYPE_CONSOLE,
	subtype :	 	SYSTEM_TYPE_CONSOLE,
	read_irq :		CONSOLE_IRQ,
	read_irq_name :		"console",
	write_irq :		CONSOLE_WRITE_IRQ,
	write_irq_name :	"console-write",
	symlink_from :		"ttys",
	symlink_to :		"vc",
};

static struct lines console_lines = LINES_INIT(MAX_TTYS);

struct line vts[MAX_TTYS] = { LINE_INIT(CONFIG_CON_ZERO_CHAN, &driver),
			      [ 1 ... MAX_TTYS - 1 ] = 
			      LINE_INIT(CONFIG_CON_CHAN, &driver) };

static int open_console(struct tty_struct *tty)
{
	return(line_open(vts, tty, &opts));
}

static int con_open(struct tty_struct *tty, struct file *filp)
{
	return(open_console(tty));
}

static void con_close(struct tty_struct *tty, struct file *filp)
{
	line_close(vts, tty);
}

static int con_write(struct tty_struct *tty, int from_user, 
		     const unsigned char *buf, int count)
{
	return(line_write(vts, tty, buf, count));
}

static void set_termios(struct tty_struct *tty, struct termios * old)
{
}

static int chars_in_buffer(struct tty_struct *tty)
{
	return(0);
}

static int con_init_done = 0;

int stdio_init(void)
{
	char *new_title;

	printk(KERN_INFO "Initializing stdio console driver\n");

	console_driver = ((struct tty_driver)
		      {
			      open :	 	con_open,
			      close :	 	con_close,
			      write :	 	con_write,
			      chars_in_buffer :	chars_in_buffer,
			      set_termios :	set_termios
		      });

	line_register_devfs(&console_lines, &driver, &console_driver, vts, 
			    sizeof(vts)/sizeof(vts[0]));

	lines_init(vts, sizeof(vts)/sizeof(vts[0]));

	new_title = add_xterm_umid(opts.xterm_title);
	if(new_title != NULL) opts.xterm_title = new_title;

	open_console(NULL);
	con_init_done = 1;
	return(0);
}

__initcall(stdio_init);

static void console_write(struct console *console, const char *string, 
			  unsigned len)
{
	console_write_chan(&vts[console->index].chan_list, string, len);
}

static kdev_t console_device(struct console *c)
{
	return mk_kdev(TTY_MAJOR, c->index);
}

static int console_setup(struct console *co, char *options)
{
	return(0);
}

static struct console stdiocons = INIT_CONSOLE("tty", console_write, 
					       console_device, console_setup,
					       CON_PRINTBUFFER);

void stdio_console_init(void)
{
	INIT_LIST_HEAD(&vts[0].chan_list);
	list_add(&init_console_chan.list, &vts[0].chan_list);
	register_console(&stdiocons);
}

static int console_chan_setup(char *str)
{
	line_setup(vts, sizeof(vts)/sizeof(vts[0]), str);
	return(1);
}

__setup("con", console_chan_setup);
__channel_help(console_chan_setup, "con");

static void console_exit(void)
{
	if(!con_init_done) return;
	line_close(vts, NULL);
	close_lines(vts, sizeof(vts)/sizeof(vts[0]));
}

__uml_exitcall(console_exit);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
