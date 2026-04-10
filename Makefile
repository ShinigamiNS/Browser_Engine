# Makefile — Scratch Browser with Lexbor HTML5 parser
# MinGW GCC on Windows
#
# SETUP (one time):
#   make setup   <- clones + builds Lexbor automatically
#   make         <- builds browser
#
# OR manually:
#   git clone https://github.com/lexbor/lexbor.git third_party/lexbor
#   cd third_party/lexbor
#   cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
#         -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_TESTS=OFF .
#   mingw32-make
#   cd ../..
#   make

CC       = gcc
CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wno-unused-variable -DUNICODE -D_UNICODE -DLEXBOR_STATIC
# QuickJS C files have many warnings — suppress them at C level
# -DCONFIG_VERSION: version string used in JS_DumpMemoryUsage
# -D_GNU_SOURCE: enables alloca/strdup on some MinGW configurations
CFLAGS   = -O2 -Wno-unused-variable -Wno-unused-function \
           -Wno-sign-compare -Wno-misleading-indentation \
           -D_GNU_SOURCE \
           "-DCONFIG_VERSION=\"2024-01-01\""

LEXBOR_DIR = third_party/lexbor
LEXBOR_INC = -I$(LEXBOR_DIR)/source
LEXBOR_LIB = $(LEXBOR_DIR)/liblexbor_static.a

# QuickJS (full ES2020 engine) — compiled as C
QJS_DIR  = third_party/quickjs
QJS_INC  = -I$(QJS_DIR)
QJS_SRCS = $(QJS_DIR)/quickjs.c   \
           $(QJS_DIR)/libregexp.c  \
           $(QJS_DIR)/libunicode.c \
           $(QJS_DIR)/cutils.c     \
           $(QJS_DIR)/dtoa.c
QJS_OBJS = $(QJS_SRCS:.c=.o)

WIN_LIBS = -lgdiplus -lwininet -lgdi32 -luser32 -lkernel32 -lole32 -lshlwapi

SRC_DIR  = src
SRCS     = $(SRC_DIR)/main.cpp              \
           $(SRC_DIR)/lexbor_adapter.cpp     \
           $(SRC_DIR)/layout.cpp             \
           $(SRC_DIR)/layout_inline.cpp      \
           $(SRC_DIR)/style.cpp              \
           $(SRC_DIR)/paint.cpp              \
           $(SRC_DIR)/renderer.cpp           \
           $(SRC_DIR)/css_parser.cpp         \
           $(SRC_DIR)/css_values.cpp         \
           $(SRC_DIR)/color_utils.cpp        \
           $(SRC_DIR)/selector.cpp           \
           $(SRC_DIR)/dom.cpp                \
           $(SRC_DIR)/font_metrics.cpp       \
           $(SRC_DIR)/html_parser.cpp        \
           $(SRC_DIR)/quickjs_adapter.cpp    \
           $(SRC_DIR)/js.cpp                 \
           $(SRC_DIR)/net.cpp                \
           $(SRC_DIR)/page_loader.cpp        \
           $(SRC_DIR)/image_cache.cpp        \
           $(SRC_DIR)/svg_rasterizer.cpp     \
           $(SRC_DIR)/ua_styles.cpp          \
           $(SRC_DIR)/browser_ui.cpp         \
           $(SRC_DIR)/browser_tabs.cpp       \
           $(SRC_DIR)/browser_address.cpp

OBJS   = $(SRCS:.cpp=.o)
TARGET = browser.exe

all: $(TARGET)

$(TARGET): $(OBJS) $(QJS_OBJS) $(LEXBOR_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(WIN_LIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(LEXBOR_INC) $(QJS_INC) -I$(SRC_DIR) -c $< -o $@

# Compile QuickJS C sources with CC (not CXX)
$(QJS_DIR)/%.o: $(QJS_DIR)/%.c
	$(CC) $(CFLAGS) $(QJS_INC) -c $< -o $@

setup:
	git clone --depth=1 https://github.com/lexbor/lexbor.git $(LEXBOR_DIR)
	cd $(LEXBOR_DIR) && cmake -G "MinGW Makefiles" \
	    -DCMAKE_BUILD_TYPE=Release -DLEXBOR_BUILD_SHARED=OFF \
	    -DLEXBOR_BUILD_TESTS=OFF -DLEXBOR_BUILD_EXAMPLES=OFF . \
	    && mingw32-make -j4

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)

.PHONY: all setup clean
