TOOLPATH?=/usr/bin/i686-w64-mingw32-

CC=$(TOOLPATH)gcc
AR=$(TOOLPATH)ar

CFLAGS+=-DWINVER=0x0500 -DNO_SSH -DPROGRAM_VERSION=\"git-$(shell git log -1 --format="%h" HEAD)\"
LIBS_TELNET?=-L. -liphlpapi -lws2_32
LIBS_CONNECT?=-L. -liphlpapi -lws2_32 -lcomctl32 -mwindows

# Common object files
COMMON_OBJS=protocol.o interfaces.o mndp.o utils.o mactelnet_conn.o

all: mactelnet.exe macConnect.exe

clean:
	rm -f *.a *.o *.exe

%.o: %.c
	$(CC) -Wall -I. $(CFLAGS) -o $@ -c $<

# Command-line mactelnet tool
mactelnet.exe: macssh.o pgetopt.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -s -o mactelnet.exe macssh.o pgetopt.o $(COMMON_OBJS) $(LIBS_TELNET)

# GUI macconnect tool (integrated terminal)
macConnect.exe: macconnect.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -s -o macConnect.exe macconnect.o $(COMMON_OBJS) $(LIBS_CONNECT)
