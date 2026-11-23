CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc
LDFLAGS  := -pthread -ldl

SRCS := \
	src/core/arena.cpp \
	src/core/json.cpp \
	src/core/memory_governor.cpp \
	src/ipc/ipc_channel.cpp \
	src/privacy/shield.cpp \
	src/net/http_cache.cpp \
	src/net/http_client.cpp \
	src/dom/html_parser.cpp \
	src/style/css_engine.cpp \
	src/layout/layout_engine.cpp \
	src/js/astra_js.cpp \
	src/ext/extension_runtime.cpp \
	src/gfx/image_decoder.cpp \
	src/gfx/rasterizer.cpp \
	src/gfx/astra3d.cpp \
	src/docs/doc_viewer.cpp \
	src/media/media_engine.cpp \
	src/browser/browser_app.cpp \
	src/main.cpp

OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))
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
