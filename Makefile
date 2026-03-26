TOOLPATH?=/usr/bin/i686-w64-mingw32-

CC=$(TOOLPATH)gcc
AR=$(TOOLPATH)ar

# Paths to built libraries
MBEDTLS_DIR=./build/mbedtls
LIBSSH_DIR=./build/libssh

# Include paths
CFLAGS+=-DWINVER=0x0500 -DPROGRAM_VERSION=\"git-$(shell git log -1 --format=\"%h\" HEAD)\" \
	-DLIBSSH_STATIC -I$(MBEDTLS_DIR)/include -I$(LIBSSH_DIR)/include

# Library paths and libs
LIBS_TELNET?=-L. -liphlpapi -lws2_32
LIBS_SSH=-L$(LIBSSH_DIR)/lib -L$(MBEDTLS_DIR)/lib -lssh -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -lws2_32 -lbcrypt
LIBS_CONNECT?=-L. -liphlpapi -lws2_32 -lcomctl32 -mwindows $(LIBS_SSH)

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

macssh.o: macssh.c
	$(CC) -Wall -I. $(CFLAGS) -DNO_SSH -o macssh.o -c macssh.c

# GUI macconnect tool (integrated terminal)
macConnect.exe: macconnect.o ssh_conn.o winpthreads.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -s -o macConnect.exe macconnect.o ssh_conn.o winpthreads.o $(COMMON_OBJS) $(LIBS_CONNECT)
