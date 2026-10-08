# LVGL 9.3 on the host, configured as the knob's phone build (lv_conf.h), to
# render UI pieces and compare them pixel by pixel -- no device needed.
#   make && ./vu_compare     (writes old*.ppm, new*.ppm, diff*.ppm here)
# vu_compare: the phone's split VU as ui.c drew it with lv_arc objects beside
# components/ui/vu_band.c, plus partial-against-full redraw checks.
#   make chooser-check OUT=dir     (writes dir/chooser-*.ppm and .png, the
#                                   250 px panel's as before-*, and fit.tsv)
#   make slab-check OUT=dir        (components/ui/ui.c itself, built for the
#                                   FlexRadio's face and the Icoms', its touch
#                                   driven: the slab's tap, its hold for the
#                                   antennas; and for every face, the reflector's,
#                                   the telephone's, the receivers' -- UberSDR's
#                                   and Kiwi888's -- AetherSDR's and the Xiegus'
#                                   too, a Bluetooth device's battery beside its
#                                   logo; on UberSDR's, a guest's time left at the
#                                   slab's left end; on all of them and the setup
#                                   firmware's, the knob's own battery at the top
#                                   of the arc; on every face with a web SDR, its
#                                   reading; pictures in dir)
# chooser: the editor panel as ui.c builds it, every value through
# components/ui/fit_text.c, on the round glass -- the fonts it took, the
# widths, the dots, the panel's margin to the glass's edge.
#   make kiwi-check OUT=dir        (writes dir/kiwi-*.ppm and .png)
# kiwi_face: the kiwi face's S-meter in one vu_band with its red peak mark,
# against the lv_arc stack, pixel by pixel; its partial redraws against full
# ones; the readout with OV, NR beside it; the NOISE FILTER and SQUELCH
# editors.
#   make readout-check OUT=dir     (writes dir/readout-*.png: the face in
#                                   Montserrat 48 left, in Hack 46 right, and
#                                   -zoom.png, the readout's rows enlarged)
# readout: the frequency readout as ui.c places it, in Montserrat 48 as it was
# and in components/ui/font_hack_46.c, between its neighbours on the glass --
# its ink, the gaps, the clearances, every place rolled through 0-9, and each
# layout's envelope: every value in every place at once (readout-env-*).
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
SLABS := slab_flex slab_icom slab_svx slab_phone slab_uber slab_aether slab_xiegu slab_setup slab_kiwi slab_owrx
all: vu_compare chooser kiwi_face readout $(SLABS)
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
kiwi_face: kiwi_face.c $(UI)/include/vu_band.h $(UI)/include/fit_text.h obj/vu_band.o obj/fit_text.o $(FONT_OBJS) $(OBJS)
	$(CC) $(WFLAGS) kiwi_face.c obj/vu_band.o obj/fit_text.o $(FONT_OBJS) $(OBJS) -lm -o $@
kiwi-check: kiwi_face
	mkdir -p $(OUT)
	./kiwi_face $(OUT); s=$$?; \
	for f in $(OUT)/kiwi-*.ppm; do \
	    [ -e "$$f" ] && magick "$$f" "$${f%.ppm}.png"; \
	done; exit $$s
chooser-check: chooser
	mkdir -p $(OUT)
	./chooser $(OUT); s=$$?; \
	for f in $(OUT)/chooser-*.ppm $(OUT)/before-*.ppm; do \
	    [ -e "$$f" ] && magick "$$f" "$${f%.ppm}.png"; \
	done; exit $$s
# The firmware's own fonts beside LVGL's: the readout's, and the microphone's
# on the row under it.
UI_FONTS := obj/font_hack_46.o obj/font_mic_14.o
obj/font_%.o: $(UI)/font_%.c lv_conf.h
	@mkdir -p obj
	@$(CC) $(CFLAGS) $(FONTS) -c $< -o $@
readout: readout.c $(UI_FONTS) $(FONT_OBJS) $(OBJS)
	$(CC) $(WFLAGS) readout.c $(UI_FONTS) $(FONT_OBJS) $(OBJS) -lm -o $@
readout-check: readout
	mkdir -p $(OUT)
	./readout $(OUT); s=$$?; \
	for f in $(OUT)/readout-*-old.ppm; do \
	    n=$${f%-old.ppm}; \
	    magick "$$f" "$$n-new.ppm" +append "$$n.png"; \
	    magick "$$f" "$$n-new.ppm" -crop 360x120+0+108 +repage -append \
	        -filter point -resize 300% "$$n-zoom.png"; \
	done; exit $$s
# slab: ui.c on the host, its ESP-IDF names from stub/ -- the display, the
# touch and the lock the harness's (slab.c) -- once per face.
UI_SRCS := $(UI)/ui.c $(UI)/fit_text.c $(UI)/vu_band.c $(UI)/font_mic_14.c $(UI)/font_btmic_28.c $(UI)/font_hack_46.c \
           $(UI)/font_svx_icons_24.c
SLAB_FLAGS := -O1 -g -DLV_CONF_INCLUDE_SIMPLE -Istub -I. -isystem $(LVGL) -I$(UI) -I$(UI)/include \
              -I$(ROOT)/components/net_prov/include $(FONTS) -Wall -Wextra -Wno-unused-parameter
slab_flex: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_MULTIFLEX=1 -DVFO_HAS_SDR=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_icom: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_ICOM=1 -DVFO_HAS_SDR=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_svx: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_SVXCONNECT=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_phone: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_PHONE=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_uber: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_UBERSDR=1 -DVFO_HAS_SDR=1 -DVFO_RX_ONLY=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_aether: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_AETHERSDR=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_xiegu: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_XIEGU=1 -DVFO_HAS_SDR=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_setup: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_SETUP=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_kiwi: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_KIWI=1 -DVFO_HAS_SDR=1 -DVFO_RX_ONLY=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab_owrx: slab.c $(UI_SRCS) $(wildcard stub/*.h) $(FONT_OBJS) $(OBJS)
	$(CC) $(SLAB_FLAGS) -DVFO_RADIO_OWRX=1 -DVFO_RX_ONLY=1 slab.c $(UI_SRCS) $(FONT_OBJS) $(OBJS) -lm -o $@
slab-check: $(SLABS)
	mkdir -p $(OUT)
	for s in $(SLABS); do ./$$s $(OUT) || exit 1; done
clean:
	rm -rf obj vu_compare chooser kiwi_face readout $(SLABS) *.ppm *.png fit.tsv
.PHONY: all clean chooser-check kiwi-check readout-check slab-check
