TARGET=mpvq
CFILES=mpvq.c
PREFIX=/usr/local

OS!=uname -s | tr '[A-Z]' '[a-z]'
CFLAGS:=-Wall -Wextra -std=c99 `pkg-config --cflags mpv` -ggdb
LDFLAGS=`pkg-config --libs mpv` -lm -lpthread

.if ${OS} == "linux"
LDFLAGS+=-lbsd
CFLAGS+=-D_XOPEN_SOURCE -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wno-format
.endif

all: $(TARGET) mpvq.1
mpvq.1:
	pod2man -s 1 -c $(TARGET) -n $(TARGET) < mpvq.pod > mpvq.1
$(TARGET):
	$(CC) $(CFLAGS) $(CFILES) $(LDFLAGS) -o $(TARGET)
clean:
	rm -f $(TARGET) *.core *.1
cloc:
	cloc `ls | grep -v termbox2`
todo:
	grep -niEo '((TODO)|(FIXME)).*' *.c
install:
	cp $(TARGET) /usr/local/bin/$(TARGET)
	cp mpvq.1 /usr/local/man/man1/mpvq.1
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/man/man1/mpvq.1
