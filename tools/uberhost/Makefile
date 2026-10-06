# The ubersdr firmware's client and the web SDR on the PC, against
# tools/mock_ubersdr.py: the knob's own code -- components/uber_client and
# components/sdr_rx on the session in components/kiwi_proto, vfo_tune -- with
# ESP-IDF stood in for by stubs.c and stub/, the knob's list of receivers
# (net_prov) by stubs.c too, libopus the host's, built with the sanitizers --
# and no TLS, the web SDR's too (test/host's notls.c). No knob, no receiver.
#   make test                    every scenario, each with a mock of its own
#                                -- two for a list of receivers (logs in
#                                ./logs, or $UBERHOST_LOGS)
#   ./uberhost lan 8080 8073     one, against a mock already running;
#                                UBERHOST_VERBOSE=1 for the debug lines too
ROOT := ../..
C    := $(ROOT)/components
SRC  := main.c stubs.c \
        $(C)/uber_client/uber_client.c $(C)/uber_client/uber_aux.c \
        $(C)/uber_client/uber_net.c $(C)/uber_client/uber_json.c \
        $(C)/sdr_rx/sdr_rx.c $(C)/kiwi_proto/kiwi_proto.c $(C)/kiwi_proto/kiwi_sess.c \
        $(C)/kiwi_proto/kiwi_mark.c $(C)/vfo_core/src/vfo_tune.c $(ROOT)/test/host/shim/notls.c
CFLAGS := -std=gnu11 -g -O1 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -Istub -I. -I$(C)/uber_client -I$(C)/uber_client/include -I$(C)/radio/include \
          -I$(C)/vfo_core/include -I$(C)/audio/include -I$(C)/sdr_rx/include -I$(C)/net_prov/include \
          -I$(C)/kiwi_proto/include -I$(C)/kiwi_proto \
          $(shell pkg-config --cflags opus)
LDLIBS := -fsanitize=address,undefined $(shell pkg-config --libs opus) -lpthread -lm
uberhost: $(SRC) uberhost.h $(wildcard stub/*.h stub/*/*.h) $(wildcard $(C)/uber_client/*.h) \
          $(C)/uber_client/include/uber.h $(C)/net_prov/include/net_prov.h \
          $(wildcard $(C)/kiwi_proto/include/*.h $(C)/sdr_rx/include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
test: uberhost
	./run.sh
clean:
	rm -rf uberhost logs
.PHONY: test clean
