# LVGL 9.3 on the host, configured as the knob's phone build (lv_conf.h), to
# render UI pieces and compare them pixel by pixel -- no device needed.
#   make && ./vu_compare     (writes old*.ppm, new*.ppm, diff*.ppm here)
# vu_compare: the phone's split VU as ui.c drew it with lv_arc objects beside
# components/ui/vu_band.c, plus partial-against-full redraw checks.
#   make chooser-check OUT=dir     (writes dir/chooser-*.ppm and .png, the
#                                   250 px panel's as before-*, and fit.tsv)
# chooser: the editor panel as ui.c builds it, every value through
# components/ui/fit_text.c, on the round glass -- the fonts it took, the
# widths, the dots, the panel's margin to the glass's edge.
ROOT := ../..
LVGL := $(ROOT)/managed_components/lvgl__lvgl
UI   := $(ROOT)/components/ui
SRCS := $(shell find $(LVGL)/src -name '*.c')
OBJS := $(patsubst $(LVGL)/%.c,obj/%.o,$(SRCS))
CFLAGS := -O2 -g -DLV_CONF_INCLUDE_SIMPLE -I. -I$(LVGL) -I$(UI)/include -w
# The editor's fonts, on the knob in every build's sdkconfig: on here for
# chooser alone, lv_conf.h as it is.
FONTS := -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_28=1 -DLV_FONT_MONTSERRAT_48=1
FONT_OBJS := obj/fonts/lv_font_montserrat_20.o obj/fonts/lv_font_montserrat_28.o \
             obj/fonts/lv_font_montserrat_48.o
# Warnings on for the code under test; LVGL's headers are LVGL's.
WFLAGS := -O2 -g -DLV_CONF_INCLUDE_SIMPLE -I. -isystem $(LVGL) -I$(UI)/include $(FONTS) -Wall -Wextra
OUT ?= .
all: vu_compare chooser
obj/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
obj/vu_band.o: $(UI)/vu_band.c $(UI)/include/vu_band.h lv_conf.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -Wall -Wextra -c $< -o $@
vu_compare: vu_compare.c obj/vu_band.o $(OBJS)
	$(CC) $(CFLAGS) -Wall vu_compare.c obj/vu_band.o $(OBJS) -lm -o $@
obj/fonts/%.o: $(LVGL)/src/font/%.c lv_conf.h
	@mkdir -p obj/fonts
	@$(CC) $(CFLAGS) $(FONTS) -c $< -o $@
obj/fit_text.o: $(UI)/fit_text.c $(UI)/include/fit_text.h lv_conf.h
	@mkdir -p obj
	$(CC) $(WFLAGS) -c $< -o $@
chooser: chooser.c $(UI)/include/fit_text.h obj/fit_text.o $(FONT_OBJS) $(OBJS)
	$(CC) $(WFLAGS) chooser.c obj/fit_text.o $(FONT_OBJS) $(OBJS) -lm -o $@
chooser-check: chooser
	mkdir -p $(OUT)
	./chooser $(OUT); s=$$?; \
	for f in $(OUT)/chooser-*.ppm $(OUT)/before-*.ppm; do \
	    [ -e "$$f" ] && magick "$$f" "$${f%.ppm}.png"; \
	done; exit $$s
clean:
	rm -rf obj vu_compare chooser *.ppm *.png fit.tsv
.PHONY: all clean chooser-check
