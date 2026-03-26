TOOLPATH?=/usr/bin/i686-w64-mingw32-

CC=$(TOOLPATH)gcc
AR=$(TOOLPATH)ar
WINDRES=$(TOOLPATH)windres
#VERSION=\"git-$(shell git log -1 --format="%h" HEAD)\"
VERSION=\"0.2.1\"
CFLAGS+=-DWINVER=0x0500 -DNO_SSH -DPROGRAM_VERSION=$(VERSION)
LIBS_TELNET?=-L. -liphlpapi -lws2_32
LIBS_CONNECT?=-L. -liphlpapi -lws2_32 -lcomctl32 -mwindows

# Common object files
COMMON_OBJS=protocol.o interfaces.o mndp.o utils.o mactelnet_conn.o

all: mactelnet.exe macConnect.exe

macconnect.res: macconnect.rc macConnect.ico
	$(WINDRES) macconnect.rc -O coff -o macconnect.res

clean:
	rm -f *.a *.o *.exe *.res

%.o: %.c
	$(CC) -Wall -I. $(CFLAGS) -o $@ -c $<

# Command-line mactelnet tool
mactelnet.exe: macssh.o pgetopt.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -s -o mactelnet.exe macssh.o pgetopt.o $(COMMON_OBJS) $(LIBS_TELNET)

# GUI macconnect tool (integrated terminal)
macConnect.exe: macconnect.o macconnect.res $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -s -o macConnect.exe macconnect.o macconnect.res $(COMMON_OBJS) $(LIBS_CONNECT)
