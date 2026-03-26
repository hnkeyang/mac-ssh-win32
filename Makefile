TOOLPATH?=/usr/bin/i686-w64-mingw32-

CC=$(TOOLPATH)gcc
AR=$(TOOLPATH)ar

CFLAGS+=-DWINVER=0x0500 -DNO_SSH -DPROGRAM_VERSION=\"git-$(shell git log -1 --format="%h" HEAD)\"
LIBS_TELNET?=-L. -liphlpapi -lws2_32
LIBS_CONNECT?=-L. -liphlpapi -lws2_32 -lcomctl32 -mwindows

all: mactelnet.exe macConnect.exe

clean:
	rm -f *.a *.o *.exe

%.o: %.c
	$(CC) -Wall -I. $(CFLAGS) -o $@ -c $<

mactelnet.exe: macssh.o protocol.o interfaces.o mndp.o pgetopt.o utils.o
	$(CC) $(LDFLAGS) -s -o mactelnet.exe macssh.o protocol.o interfaces.o pgetopt.o utils.o mndp.o $(LIBS_TELNET)

macConnect.exe: macconnect.o protocol.o interfaces.o mndp.o utils.o
	$(CC) $(LDFLAGS) -s -o macConnect.exe macconnect.o protocol.o interfaces.o mndp.o utils.o $(LIBS_CONNECT)
