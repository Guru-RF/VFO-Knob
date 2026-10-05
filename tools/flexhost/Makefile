# The multiflex firmware's FlexRadio client on the PC, against
# tools/mock_flex.py: the knob's own code -- components/flex_client's
# flex_client.c and flex_parse.c, vfo_tune, the PTT state machine -- with
# ESP-IDF stood in for by stubs.c, stub/ and uberhost's stub/, libopus the
# host's, built with the sanitizers. No knob, no radio, nothing keyed: the
# mock refuses to transmit.
#   make test                    every scenario, each with a mock of its own
#                                (logs in ./logs, or $FLEXHOST_LOGS)
#   ./flexhost basic 4992        one, against a mock already running
ROOT := ../..
C    := $(ROOT)/components
SRC  := main.c stubs.c \
        $(C)/flex_client/flex_client.c $(C)/flex_client/flex_parse.c \
        $(C)/vfo_core/src/vfo_tune.c $(C)/vfo_core/src/ptt_fsm.c
CFLAGS := -std=gnu11 -g -O1 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -Istub -I../uberhost/stub -I. -I$(C)/flex_client -I$(C)/flex_client/include \
          -I$(C)/radio/include -I$(C)/vfo_core/include -I$(C)/audio/include \
          $(shell pkg-config --cflags opus)
LDLIBS := -fsanitize=address,undefined $(shell pkg-config --libs opus) -lpthread -lm
flexhost: $(SRC) $(wildcard stub/*.h stub/*/*.h ../uberhost/stub/*.h ../uberhost/stub/*/*.h) \
          $(wildcard $(C)/flex_client/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
test: flexhost
	./run.sh
clean:
	rm -rf flexhost logs
.PHONY: test clean
