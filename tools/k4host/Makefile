# The k4 firmware's Elecraft K4 client on the PC, against tools/mock_k4.py:
# the knob's own code -- components/k4_client's k4_client.c and k4_parse.c,
# vfo_tune, the PTT state machine -- with ESP-IDF stood in for by stubs.c,
# stub/ and uberhost's stub/, libopus the host's, built with the sanitizers.
# No knob, no radio, nothing keyed: the mock refuses to transmit.
#   make test                    every scenario, each with a mock of its own
#                                (logs in ./logs, or $K4HOST_LOGS)
#   ./k4host basic 9205 log      one, against a mock already running
ROOT := ../..
C    := $(ROOT)/components
SRC  := main.c stubs.c \
        $(C)/k4_client/k4_client.c $(C)/k4_client/k4_parse.c \
        $(C)/vfo_core/src/vfo_tune.c $(C)/vfo_core/src/ptt_fsm.c
CFLAGS := -std=gnu11 -g -O1 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -Istub -I../flexhost/stub -I../uberhost/stub -I. -I$(C)/k4_client \
          -I$(C)/radio/include -I$(C)/vfo_core/include -I$(C)/audio/include \
          $(shell pkg-config --cflags opus)
LDLIBS := -fsanitize=address,undefined $(shell pkg-config --libs opus) -lcrypto -lpthread -lm
k4host: $(SRC) $(wildcard stub/*.h stub/*/*.h ../flexhost/stub/*/*.h ../uberhost/stub/*.h ../uberhost/stub/*/*.h) \
        $(wildcard $(C)/k4_client/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
test: k4host
	./run.sh
clean:
	rm -rf k4host logs
.PHONY: test clean
