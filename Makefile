# Jumping Jack - builds the same C core for the browser (WASM) and macOS.

LLVM_BIN ?= /opt/homebrew/opt/llvm@22/bin
LLD_BIN  ?= /opt/homebrew/opt/lld@22/bin
WASM_CC  ?= $(LLVM_BIN)/clang
WASM_LD  ?= $(LLD_BIN)/wasm-ld
CC       ?= clang
PORT     ?= 8000

APP      := build/JumpingJack.app
APP_BIN  := $(APP)/Contents/MacOS/JumpingJack
WASM     := web/jumpingjack.wasm
CORE     := src/game.c src/game.h

CFLAGS   := -std=c11 -O2 -Wall -Wextra
WASM_CFLAGS := $(CFLAGS) --target=wasm32 -ffreestanding -nostdlib -fno-builtin
EXPORTS  := jj_init jj_tick jj_set_sample_rate jj_framebuffer jj_audio \
            jj_audio_len jj_hiscore jj_set_hiscore

.PHONY: all web mac serve run-mac test clean

all: web mac

web: $(WASM)

$(WASM): $(CORE) src/wasm_rt.c
	@mkdir -p build/wasm
	$(WASM_CC) $(WASM_CFLAGS) -c src/game.c -o build/wasm/game.o
	$(WASM_CC) $(WASM_CFLAGS) -c src/wasm_rt.c -o build/wasm/wasm_rt.o
	$(WASM_LD) --no-entry --strip-all --lto-O3 $(addprefix --export=,$(EXPORTS)) \
		-o $@ build/wasm/game.o build/wasm/wasm_rt.o

serve: web
	@echo "Jumping Jack: http://localhost:$(PORT)/"
	@(sleep 1 && open "http://localhost:$(PORT)/") &
	python3 -m http.server $(PORT) --directory web

mac: $(APP_BIN)

$(APP_BIN): $(CORE) mac/main.m mac/Info.plist
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(CC) $(CFLAGS) -fobjc-arc -mmacosx-version-min=11.0 \
		-framework Cocoa -framework AudioToolbox -framework QuartzCore \
		-o $(APP_BIN) src/game.c mac/main.m
	cp mac/Info.plist $(APP)/Contents/Info.plist
	codesign --force --sign - $(APP)

run-mac: mac
	open $(APP)

test:
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/test_game tests/test_game.c
	./build/test_game

clean:
	rm -rf build $(WASM)
