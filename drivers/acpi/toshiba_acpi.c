/*
 *  toshiba_acpi.c - Toshiba Laptop ACPI Extras
 *
 *
 *  Copyright (C) 2002 John Belmonte
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  The devolpment page for this driver is located at
 *  http://memebeam.org/toys/ToshibaAcpiDriver.
 *
 *  Credits:
 *	Jonathan A. Buzzard - Toshiba HCI info, and critical tips on reverse
 *		engineering the Windows drivers
 *	Yasushi Nagato - changes for linux kernel 2.4 -> 2.5
 *	Rob Miller - TV out and hotkeys help
 *
 *
 *  TODO
 *
 */

#define TOSHIBA_ACPI_VERSION	"0.13"
#define PROC_INTERFACE_VERSION	1

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/version.h>

#include <acconfig.h>
#define OLD_ACPI_INTERFACE (ACPI_CA_VERSION < 0x20020000)

#if OLD_ACPI_INTERFACE
#include <acpi.h>
extern struct proc_dir_entry* bm_proc_root;
#define acpi_root_dir bm_proc_root
#else
#include "acpi_drivers.h"
#endif

MODULE_AUTHOR("John Belmonte");
MODULE_DESCRIPTION("Toshiba Laptop ACPI Extras Driver");
MODULE_LICENSE("GPL");

/* Toshiba ACPI method paths */
#define METHOD_LCD_BRIGHTNESS	"\\_SB_.PCI0.VGA_.LCD_._BCM"
#define METHOD_HCI		"\\_SB_.VALD.GHCI"
#define METHOD_VIDEO_OUT	"\\_SB_.VALX.DSSX"

/* Toshiba HCI interface definitions
 *
 * HCI is Toshiba's "Hardware Control Interface" which is supposed to
 * be uniform across all their models.  Ideally we would just call
 * dedicated ACPI methods instead of using this primitive interface.
 * However the ACPI methods seem to be incomplete in some areas (for
 * example they allow setting, but not reading, the LCD brightness value),
 * so this is still useful.
 */

#define HCI_WORDS			6

/* operations */
#define HCI_SET				0xff00
#define HCI_GET				0xfe00

/* return codes */
#define HCI_SUCCESS			0x0000
#define HCI_FAILURE			0x1000
#define HCI_NOT_SUPPORTED		0x8000
#define HCI_EMPTY			0x8c00

/* registers */
#define HCI_FAN				0x0004
#define HCI_SYSTEM_EVENT		0x0016
#define HCI_VIDEO_OUT			0x001c
#define HCI_HOTKEY_EVENT		0x001e
#define HCI_LCD_BRIGHTNESS		0x002a

/* field definitions */
#define HCI_LCD_BRIGHTNESS_BITS		3
#define HCI_LCD_BRIGHTNESS_SHIFT	(16-HCI_LCD_BRIGHTNESS_BITS)
#define HCI_LCD_BRIGHTNESS_LEVELS	(1 << HCI_LCD_BRIGHTNESS_BITS)
#define HCI_VIDEO_OUT_LCD		0x1
#define HCI_VIDEO_OUT_CRT		0x2
#define HCI_VIDEO_OUT_TV		0x4

static int toshiba_lcd_open_fs(struct inode *inode, struct file *file);
static int toshiba_video_open_fs(struct inode *inode, struct file *file);
static int toshiba_fan_open_fs(struct inode *inode, struct file *file);
static int toshiba_keys_open_fs(struct inode *inode, struct file *file);
static int toshiba_version_open_fs(struct inode *inode, struct file *file);

static struct file_operations toshiba_lcd_fops = {
	.open		= toshiba_lcd_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations toshiba_video_fops = {
	.open		= toshiba_video_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations toshiba_fan_fops = {
	.open		= toshiba_fan_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations toshiba_keys_fops = {
	.open		= toshiba_keys_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations toshiba_version_fops = {
	.open		= toshiba_version_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* utility
 */

static __inline__ void
_set_bit(u32* word, u32 mask, int value)
{
	*word = (*word & ~mask) | (mask * value);
}

/* an sscanf that takes explicit string length */
static int
snscanf(const char* str, int n, const char* format, ...)
{
	va_list args;
	int result;
	char* str2 = kmalloc(n + 1, GFP_KERNEL);
	if (str2 == 0) return 0;
	strncpy(str2, str, n);
	str2[n] = 0;
	va_start(args, format);
	result = vsscanf(str2, format, args);
	va_end(args);
	kfree(str2);
	return result;
}

/* acpi interface wrappers
 */

static int
write_acpi_int(const char* methodName, int val)
{
	struct acpi_object_list params;
	union acpi_object in_objs[1];
	acpi_status status;

	params.count = sizeof(in_objs)/sizeof(in_objs[0]);
	params.pointer = in_objs;
	in_objs[0].type = ACPI_TYPE_INTEGER;
	in_objs[0].integer.value = val;

	status = acpi_evaluate_object(0, (char*)methodName, &params, 0);
	return (status == AE_OK);
}

#if 0
static int
read_acpi_int(const char* methodName, int* pVal)
{
	acpi_buffer results;
	acpi_object out_objs[1];
	acpi_status status;

	results.length = sizeof(out_objs);
	results.pointer = out_objs;

	status = acpi_evaluate_object(0, (char*)methodName, 0, &results);
	*pVal = out_objs[0].integer.value;

	return (status == AE_OK) && (out_objs[0].type == ACPI_TYPE_INTEGER);
}
#endif

/* Perform a raw HCI call.  Here we don't care about input or output buffer
 * format.
 */
static acpi_status
hci_raw(const u32 in[HCI_WORDS], u32 out[HCI_WORDS])
{
	struct acpi_object_list params;
	union acpi_object in_objs[HCI_WORDS];
	struct acpi_buffer results;
	union acpi_object out_objs[HCI_WORDS+1];
	acpi_status status;
	int i;

	params.count = HCI_WORDS;
	params.pointer = in_objs;
	for (i = 0; i < HCI_WORDS; ++i) {
		in_objs[i].type = ACPI_TYPE_INTEGER;
		in_objs[i].integer.value = in[i];
		/*printk("%04x ", in[i]);*/
	}
	/*printk("\n");*/

	results.length = sizeof(out_objs);
	results.pointer = out_objs;

	status = acpi_evaluate_object(0, METHOD_HCI, &params,
		&results);
	if ((status == AE_OK) && (out_objs->package.count <= HCI_WORDS)) {
		for (i = 0; i < out_objs->package.count; ++i) {
			out[i] = out_objs->package.elements[i].integer.value;
			/*printk("%04x ", out[i]);*/
		}
		/*printk("\n");*/
	}

	return status;
}

/* common hci tasks (get or set one value)
 *
 * In addition to the ACPI status, the HCI system returns a result which
 * may be useful (such as "not supported").
 */

static acpi_status
hci_write1(u32 reg, u32 in1, u32* result)
{
	u32 in[HCI_WORDS] = { HCI_SET, reg, in1, 0, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(in, out);
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

static acpi_status
hci_read1(u32 reg, u32* out1, u32* result)
{
	u32 in[HCI_WORDS] = { HCI_GET, reg, 0, 0, 0, 0 };
	u32 out[HCI_WORDS];
	acpi_status status = hci_raw(in, out);
	*out1 = out[2];
	*result = (status == AE_OK) ? out[0] : HCI_FAILURE;
	return status;
}

#define PROC_TOSHIBA		"toshiba"
#define PROC_LCD		"lcd"
#define PROC_VIDEO		"video"
#define PROC_FAN		"fan"
#define PROC_KEYS		"keys"
#define PROC_VERSION		"version"

static struct proc_dir_entry*	toshiba_proc_dir = NULL;
static int			force_fan;
static int			last_key_event;
static int			key_event_valid;

/* proc file handlers
 */

static int toshiba_lcd_seq_show(struct seq_file *seq, void *offset)
{
	u32 hci_result;
	u32 value;

	hci_read1(HCI_LCD_BRIGHTNESS, &value, &hci_result);
	if (hci_result == HCI_SUCCESS) {
		value = value >> HCI_LCD_BRIGHTNESS_SHIFT;
		seq_printf(seq, "brightness:              %d\n"
				"brightness_levels:       %d\n",
				value,
				HCI_LCD_BRIGHTNESS_LEVELS);
	} else
		seq_puts(seq, "ERROR\n");

	return 0;
}

static int toshiba_lcd_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, toshiba_lcd_seq_show, NULL);
}

static int
proc_write_lcd(struct file* file, const char* buffer, unsigned long count,
	void* data)
{
	int value;
	/*int byte_count;*/
	u32 hci_result;

	/* ISSUE: %i doesn't work with hex values as advertised */
	if (snscanf(buffer, count, " brightness : %i", &value) == 1 &&
			value >= 0 && value < HCI_LCD_BRIGHTNESS_LEVELS) {
		value = value << HCI_LCD_BRIGHTNESS_SHIFT;
		hci_write1(HCI_LCD_BRIGHTNESS, value, &hci_result);
		if (hci_result != HCI_SUCCESS)
			return -EFAULT;
	} else {
		return -EINVAL;
	}

	return count;
}

static int toshiba_video_seq_show(struct seq_file *seq, void *offset)
{
	u32 hci_result;
	u32 value;

	hci_read1(HCI_VIDEO_OUT, &value, &hci_result);
	if (hci_result == HCI_SUCCESS) {
		int is_lcd = (value & HCI_VIDEO_OUT_LCD) ? 1 : 0;
		int is_crt = (value & HCI_VIDEO_OUT_CRT) ? 1 : 0;
		int is_tv  = (value & HCI_VIDEO_OUT_TV ) ? 1 : 0;
		seq_printf(seq, "lcd_out:                 %d\n"
				"crt_out:                 %d\n"
				"tv_out:                  %d\n",
				is_lcd,
				is_crt,
				is_tv);
	} else
		seq_puts(seq, "ERROR\n");

	return 0;
}

static int toshiba_video_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, toshiba_video_seq_show, NULL);
}

static int
proc_write_video(struct file* file, const char* buffer, unsigned long count,
	void* data)
{
	int value;
	const char* buffer_end = buffer + count;
	int lcd_out = -1;
	int crt_out = -1;
	int tv_out = -1;
	u32 hci_result;
	int video_out;

	/* scan expression.  Multiple expressions may be delimited with ; */
	do {
		if (snscanf(buffer, count, " lcd_out : %i", &value) == 1)
			lcd_out = value & 1;
		else if (snscanf(buffer, count, " crt_out : %i", &value) == 1)
			crt_out = value & 1;
		else if (snscanf(buffer, count, " tv_out : %i", &value) == 1)
			tv_out = value & 1;
		/* advance to one character past the next ; */
		do ++buffer;
		while ((buffer < buffer_end) && (*(buffer-1) != ';'));
	} while (buffer < buffer_end);

	hci_read1(HCI_VIDEO_OUT, &video_out, &hci_result);
	if (hci_result == HCI_SUCCESS) {
		int new_video_out = video_out;
		if (lcd_out != -1)
			_set_bit(&new_video_out, HCI_VIDEO_OUT_LCD, lcd_out);
		if (crt_out != -1)
			_set_bit(&new_video_out, HCI_VIDEO_OUT_CRT, crt_out);
		if (tv_out != -1)
			_set_bit(&new_video_out, HCI_VIDEO_OUT_TV, tv_out);
		/* To avoid unnecessary video disruption, only write the new
		 * video setting if something changed. */
		if (new_video_out != video_out)
			write_acpi_int(METHOD_VIDEO_OUT, new_video_out);
	}

	return count;
}

static int toshiba_fan_seq_show(struct seq_file *seq, void *offset)
{
	u32 hci_result;
	u32 value;

	hci_read1(HCI_FAN, &value, &hci_result);
	if (hci_result == HCI_SUCCESS) {
		seq_printf(seq, "running:                 %d\n"
				"force_on:                %d\n",
				(value > 0),
				force_fan);
	} else
		seq_puts(seq, "ERROR\n");

	return 0;
}

static int toshiba_fan_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, toshiba_fan_seq_show, NULL);
}

static int
proc_write_fan(struct file* file, const char* buffer, unsigned long count,
	void* data)
{
	int value;
	u32 hci_result;

	if (snscanf(buffer, count, " force_on : %i", &value) == 1 &&
			value >= 0 && value <= 1) {
		hci_write1(HCI_FAN, value, &hci_result);
		if (hci_result != HCI_SUCCESS)
			return -EFAULT;
		else
			force_fan = value;
	} else {
		return -EINVAL;
	}

	return count;
}

static int toshiba_keys_seq_show(struct seq_file *seq, void *offset)
{
	u32 hci_result;
	u32 value;

	if (!key_event_valid) {
		hci_read1(HCI_SYSTEM_EVENT, &value, &hci_result);
		if (hci_result == HCI_SUCCESS) {
			key_event_valid = 1;
			last_key_event = value;
		} else if (hci_result == HCI_EMPTY) {
			/* better luck next time */
		} else {
			seq_puts(seq, "ERROR\n");
			goto end;
		}
	}

	seq_printf(seq, "hotkey_ready:            %d\n"
			"hotkey:                  0x%04x\n",
			key_event_valid,
			last_key_event);

end:
	return 0;
}

static int toshiba_keys_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, toshiba_keys_seq_show, NULL);
}

static int
proc_write_keys(struct file* file, const char* buffer, unsigned long count,
	void* data)
{
	int value;

	if (snscanf(buffer, count, " hotkey_ready : %i", &value) == 1 &&
			value == 0) {
		key_event_valid = 0;
	} else {
		return -EINVAL;
	}

	return count;
}

static int toshiba_version_seq_show(struct seq_file *seq, void *offset)
{
	seq_printf(seq, "driver:                  %s\n"
			"proc_interface:          %d\n",
			TOSHIBA_ACPI_VERSION,
			PROC_INTERFACE_VERSION);

	return 0;
}

static int toshiba_version_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, toshiba_version_seq_show, NULL);
}

/* proc and module init
 */

static acpi_status
add_device(void)
{
	struct proc_dir_entry* proc;

	proc = create_proc_entry(PROC_LCD, S_IFREG | S_IRUGO | S_IWUSR,
		toshiba_proc_dir);
	if (proc) {
		proc->proc_fops = &toshiba_lcd_fops;
		proc->write_proc = proc_write_lcd;
	}

	proc = create_proc_entry(PROC_VIDEO, S_IFREG | S_IRUGO | S_IWUSR,
		toshiba_proc_dir);
	if (proc) {
		proc->proc_fops = &toshiba_video_fops;
		proc->write_proc = proc_write_video;
	}

	proc = create_proc_entry(PROC_FAN, S_IFREG | S_IRUGO | S_IWUSR,
		toshiba_proc_dir);
	if (proc) {
		proc->proc_fops = &toshiba_fan_fops;
		proc->write_proc = proc_write_fan;
	}

	proc = create_proc_entry(PROC_KEYS, S_IFREG | S_IRUGO | S_IWUSR,
		toshiba_proc_dir);
	if (proc) {
		proc->proc_fops = &toshiba_keys_fops;
		proc->write_proc = proc_write_keys;
	}

	proc = create_proc_entry(PROC_VERSION, S_IFREG | S_IRUGO | S_IWUSR,
		toshiba_proc_dir);
	if (proc)
		proc->proc_fops = &toshiba_version_fops;

	return(AE_OK);
}

static acpi_status
remove_device(void)
{
	remove_proc_entry(PROC_LCD, toshiba_proc_dir);
	remove_proc_entry(PROC_VIDEO, toshiba_proc_dir);
	remove_proc_entry(PROC_FAN, toshiba_proc_dir);
	remove_proc_entry(PROC_KEYS, toshiba_proc_dir);
	remove_proc_entry(PROC_VERSION, toshiba_proc_dir);
	return(AE_OK);
}

static int __init
toshiba_acpi_init(void)
{
	acpi_status status = AE_OK;
	int value;
	u32 hci_result;

	/* simple device detection: try reading an HCI register */
	hci_read1(HCI_LCD_BRIGHTNESS, &value, &hci_result);
	if (hci_result != HCI_SUCCESS)
		return -ENODEV;

	printk("Toshiba Laptop ACPI Extras version %s\n", TOSHIBA_ACPI_VERSION);

	force_fan = 0;
	key_event_valid = 0;

	/* enable event fifo */
	hci_write1(HCI_SYSTEM_EVENT, 1, &hci_result);

	toshiba_proc_dir = proc_mkdir(PROC_TOSHIBA, acpi_root_dir);
	if (!toshiba_proc_dir) {
		status = AE_ERROR;
	} else {
		status = add_device();
		if (ACPI_FAILURE(status))
			remove_proc_entry(PROC_TOSHIBA, acpi_root_dir);
	}

	return (ACPI_SUCCESS(status)) ? 0 : -ENODEV;
}

static void __exit
toshiba_acpi_exit(void)
{
	remove_device();

	if (toshiba_proc_dir)
		remove_proc_entry(PROC_TOSHIBA, acpi_root_dir);

	return;
}

module_init(toshiba_acpi_init);
module_exit(toshiba_acpi_exit);
