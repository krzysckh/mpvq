TARGET=mpvq
OFILES=mpvq.o
PREFIX=/usr/local

CFLAGS=-Wall -Wextra -std=c99 `pkg-config --cflags mpv` -ggdb
LDFLAGS=`pkg-config --libs mpv` -lm -lpthread

all: $(TARGET) mpvq.1
.c.o:
	$(CC) $(CFLAGS) -c $<
mpvq.1:
	pod2man -s 1 -c $(TARGET) -n $(TARGET) < mpvq.pod > mpvq.1
$(TARGET): $(OFILES)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OFILES)
clean:
	rm -f $(TARGET) $(OFILES) *.core *.1
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
