CC=gcc 
CFLAGS=-Wall
LDLIBS = -lcurl
all: pingalert
pingalert: pingalert.o
pingalert.o: pingalert.c
clean:
	rm -f pingalert pingalert.o
