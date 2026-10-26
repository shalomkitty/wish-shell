CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

.PHONY: all test clean

all: wish

wish: wish.c
	$(CC) $(CFLAGS) -o $@ $<

test: wish
	python3 -m unittest discover -s tests -v

clean:
	rm -f wish
