# LVGL 9.3 on the host, configured as the knob's phone build (lv_conf.h), to
# render UI pieces and compare them pixel by pixel -- no device needed.
#   make && ./vu_compare     (writes old*.ppm, new*.ppm, diff*.ppm here)
# vu_compare: the phone's split VU as ui.c drew it with lv_arc objects beside
# components/ui/vu_band.c, plus partial-against-full redraw checks.
ROOT := ../..
LVGL := $(ROOT)/managed_components/lvgl__lvgl
UI   := $(ROOT)/components/ui
SRCS := $(shell find $(LVGL)/src -name '*.c')
OBJS := $(patsubst $(LVGL)/%.c,obj/%.o,$(SRCS))
CFLAGS := -O2 -g -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(UI)/include -w
all: vu_compare
obj/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
obj/vu_band.o: $(UI)/vu_band.c $(UI)/include/vu_band.h lv_conf.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -Wall -Wextra -c $< -o $@
vu_compare: vu_compare.c obj/vu_band.o $(OBJS)
	$(CC) $(CFLAGS) -Wall vu_compare.c obj/vu_band.o $(OBJS) -lm -o $@
clean:
	rm -rf obj vu_compare *.ppm
