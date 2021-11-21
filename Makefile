CC=gcc
CFLAGS_O=-O2 
CFLAGS=-Wall -Wextra -fPIC -O2 -I./include
LFLAGS=-fPIC -shared -O2 -ldl -lrt
OBJS=obj/ooops.o obj/hook.o obj/decode.o obj/itab.o obj/syn-att.o obj/syn-intel.o obj/syn.o obj/udis86.o
HEADERS=include/udis86.h libudis86/decode.h libudis86/extern.h libudis86/itab.h libudis86/syn.h libudis86/types.h libudis86/udint.h src/hook_int.h
RM=rm -rf

# in cmd of windows
ifeq ($(SHELL),sh.exe)
    RM := del /f/q
endif

#all: ooops.so get_ip mpi_aggregator msleep ooops ooopsd set_io_param
all: ooops.so get_ip msleep ooops ooopsd set_io_param test_open test_stat t_open_stat

ooops.so: ooops.o hook.o decode.o itab.o syn-att.o syn-intel.o syn.o udis86.o
	$(CC) $(LFLAGS) -o ooops.so obj/ooops.o obj/hook.o obj/decode.o obj/itab.o obj/syn-att.o obj/syn-intel.o obj/syn.o obj/udis86.o

ooops.o: src/ooops.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/ooops.o $<

hook.o: src/hook.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/hook.o $<

decode.o: libudis86/decode.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/decode.o $<

itab.o: libudis86/itab.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/itab.o $<

syn-att.o: libudis86/syn-att.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/syn-att.o $<

syn-intel.o: libudis86/syn-intel.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/syn-intel.o $<

syn.o: libudis86/syn.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/syn.o $<

udis86.o: libudis86/udis86.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o obj/udis86.o $<
get_ip:
	$(CC) $(CFLAGS_O) -o get_ip src/get_ip.c

mpi_aggregator:
	mpicc -O2 -o mpi_aggregator src/mpi_aggregator.c 

msleep:
	$(CC) $(CFLAGS_O) -o msleep src/msleep.c

ooops:
	$(CC) $(CFLAGS_O) -o ooops src/client.c -lrt -Iinclude

ooopsd:
	$(CC) $(CFLAGS_O) -o ooopsd src/server.c src/dict.c src/xxhash.c -lrt -Iinclude

set_io_param:
	$(CC) $(CFLAGS_O) -o set_io_param src/set_io_param.c -lrt

t_open_stat:
	gcc -O2 -o t_open_stat src/t_open_stat.c
	chmod u+x cal_threshhold.sh

test_open:
	gcc -O2 -o test_open src/test_open.c

test_stat:
	gcc -O2 -o test_stat src/test_stat.c

clean:
	$(RM) obj/*.o myopen.so get_ip msleep ooops ooopsd set_io_param test_open test_stat t_open_stat


$(shell   mkdir -p obj)
