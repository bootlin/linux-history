# 
# Copyright (C) 2000, 2002, 2003 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

# pcap is broken in 2.5 because kbuild doesn't allow pcap.a to be linked
# in to pcap.o

slip-objs := slip_kern.o slip_user.o
slirp-objs := slirp_kern.o slirp_user.o
daemon-objs := daemon_kern.o daemon_user.o
mcast-objs := mcast_kern.o mcast_user.o
#pcap-objs := pcap_kern.o pcap_user.o $(PCAP)
net-objs := net_kern.o net_user.o
mconsole-objs := mconsole_kern.o mconsole_user.o
hostaudio-objs := hostaudio_kern.o
ubd-objs := ubd_kern.o ubd_user.o
port-objs := port_kern.o port_user.o
harddog-objs := harddog_kern.o harddog_user.o

obj-y := stdio_console.o fd.o chan_kern.o chan_user.o line.o
obj-$(CONFIG_SSL) += ssl.o
obj-$(CONFIG_STDERR_CONSOLE) += stderr_console.o

obj-$(CONFIG_UML_NET_SLIP) += slip.o
obj-$(CONFIG_UML_NET_SLIRP) += slirp.o
obj-$(CONFIG_UML_NET_DAEMON) += daemon.o 
obj-$(CONFIG_UML_NET_MCAST) += mcast.o 
#obj-$(CONFIG_UML_NET_PCAP) += pcap.o $(PCAP)
obj-$(CONFIG_UML_NET) += net.o 
obj-$(CONFIG_MCONSOLE) += mconsole.o
obj-$(CONFIG_MMAPPER) += mmapper_kern.o 
obj-$(CONFIG_BLK_DEV_UBD) += ubd.o 
obj-$(CONFIG_HOSTAUDIO) += hostaudio.o
obj-$(CONFIG_NULL_CHAN) += null.o 
obj-$(CONFIG_PORT_CHAN) += port.o
obj-$(CONFIG_PTY_CHAN) += pty.o
obj-$(CONFIG_TTY_CHAN) += tty.o 
obj-$(CONFIG_XTERM_CHAN) += xterm.o xterm_kern.o
obj-$(CONFIG_UML_WATCHDOG) += harddog.o
obj-$(CONFIG_BLK_DEV_COW_COMMON) += cow_user.o
obj-$(CONFIG_UML_RANDOM) += random.o

USER_SINGLE_OBJS = $(foreach f,$(patsubst %.o,%,$(obj-y) $(obj-m)),$($(f)-objs))

USER_OBJS := $(filter %_user.o,$(obj-y) $(obj-m) $(USER_SINGLE_OBJS)) fd.o \
	null.o pty.o tty.o xterm.o
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<
