# `CXX ?= g++` no bastaría: make predefine CXX como `c++`, así que `?=` nunca
# se aplicaría. Esto mantiene g++ como valor por defecto documentado y, a la
# vez, respeta un CXX exportado o pasado en la línea de órdenes.
CXX      := $(if $(filter default,$(origin CXX)),g++,$(CXX))
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc -MMD -MP
LDFLAGS  := -pthread

# -----------------------------------------------------------------------------
# Detección de plataforma (Session 8): el mismo árbol de código compila en
# Linux (glibc) y macOS/Darwin. Las divergencias de SO viven en
# src/core/platform.hpp; aquí solo cambian las banderas del enlazador, porque
# en Darwin dl*() forma parte de libSystem y no existe -ldl.
# -----------------------------------------------------------------------------
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -Wl,-dead_strip
else
  LDFLAGS += -ldl
endif

SRCS := \
	src/core/arena.cpp \
	src/core/json.cpp \
	src/core/memory_governor.cpp \
	src/ipc/ipc_channel.cpp \
	src/ipc/sandbox.cpp \
	src/privacy/shield.cpp \
	src/privacy/security_policy.cpp \
	src/net/http_cache.cpp \
	src/net/http2_client.cpp \
	src/net/http_client.cpp \
	src/net/websocket_client.cpp \
	src/dom/html_parser.cpp \
	src/style/css_engine.cpp \
	src/style/animation_engine.cpp \
	src/layout/layout_engine.cpp \
	src/js/astra_js.cpp \
	src/ext/extension_runtime.cpp \
	src/gfx/font_engine.cpp \
	src/gfx/image_decoder.cpp \
	src/gfx/svg_engine.cpp \
	src/gfx/rasterizer.cpp \
	src/gfx/astra3d.cpp \
	src/docs/doc_viewer.cpp \
	src/media/media_engine.cpp \
	src/browser/browser_app.cpp \
	src/main.cpp

OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)
TARGET := build/astra

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: $(TARGET)
	./$(TARGET) --test

clean:
	rm -rf build

-include $(DEPS)
