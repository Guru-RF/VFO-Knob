# The phone firmware's G.722 (components/phone_client/g722.c) on the host,
# checked against ffmpeg's adpcm_g722 run as a black box -- see g722test.c,
# which includes g722.c itself, for its blocks.
#   make test                (needs ffmpeg; its scratch files go under $TMPDIR)
#   make itu ITU=dir         and the ITU-T G.722 Appendix II test sequences in dir
ROOT := ../..
PC   := $(ROOT)/components/phone_client
CFLAGS := -O2 -std=c11 -Wall -Wextra -Wpedantic -I$(PC)/include
all: g722test
g722test: g722test.c $(PC)/g722.c $(PC)/include/g722.h
	$(CC) $(CFLAGS) g722test.c -lm -o $@
test: g722test
	./g722test
itu: g722test
	./g722test -i "$(ITU)"
clean:
	rm -f g722test
.PHONY: all test itu clean
