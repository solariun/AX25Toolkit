# =============================================================================
# Makefile — ax25lib + BBS + BASIC interpreter + tests
# =============================================================================
CXX      ?= g++
CXXFLAGS  = -std=c++11 -O2 -Wall -Wextra -Wpedantic
UNAME    := $(shell uname)

# ── Platform detection ────────────────────────────────────────────────────────
ifeq ($(UNAME), Linux)
    LDUTIL    = -lutil
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
else
    LDUTIL    =
    SQLITE3   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo "-lsqlite3")
    SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
endif

# ── SQLite feature flag ───────────────────────────────────────────────────────
HAVE_SQLITE := $(shell pkg-config --exists sqlite3 2>/dev/null && echo yes || \
               { [ -f /usr/include/sqlite3.h ] || [ -f /opt/homebrew/include/sqlite3.h ]; } && echo yes)
ifeq ($(HAVE_SQLITE), yes)
    SQLITE_FLAGS = -DHAVE_SQLITE3 $(SQLITE3_CFLAGS)
    SQLITE_LIBS  = $(SQLITE3)
else
    SQLITE_FLAGS =
    SQLITE_LIBS  =
endif

# ── GoogleTest detection ──────────────────────────────────────────────────────
GTEST_CFLAGS  := $(shell pkg-config --cflags gtest 2>/dev/null)
GTEST_LDFLAGS := $(shell pkg-config --libs gtest_main 2>/dev/null)
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /opt/homebrew/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/opt/homebrew/include
        GTEST_LDFLAGS := -L/opt/homebrew/lib -lgtest -lgtest_main
    endif
endif
ifeq ($(GTEST_LDFLAGS),)
    ifneq ($(wildcard /usr/local/lib/libgtest.a),)
        GTEST_CFLAGS  := -I/usr/local/include
        GTEST_LDFLAGS := -L/usr/local/lib -lgtest -lgtest_main
    endif
endif
ifeq ($(GTEST_LDFLAGS),)
    GTEST_LDFLAGS := -lgtest -lgtest_main
endif
GTEST_LDFLAGS += -lpthread

# ── Sources ───────────────────────────────────────────────────────────────────
LIB_SRC   = ax25lib.cpp
LIB_OBJ   = ax25lib.o
BASIC_SRC = basic.cpp
BASIC_OBJ = basic.o

# ── Native BLE (BlueZ D-Bus on Linux, CoreBluetooth on macOS) ────────────────
ifeq ($(UNAME), Linux)
    DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
    DBUS_LIBS   := $(shell pkg-config --libs   dbus-1 2>/dev/null || echo "-ldbus-1")
    BLE_OBJ      = bt_ble_linux.o
    BLE_SYS      = $(DBUS_LIBS) -lpthread
    BLUETOOTH_LIBS = -lbluetooth
    # ARM 32-bit needs libatomic for 64-bit atomic ops (e.g. std::atomic<uint64_t>)
    ifneq ($(filter arm%,$(shell uname -m)),)
        BLUETOOTH_LIBS += -latomic
    endif
else
    DBUS_CFLAGS :=
    BLE_OBJ      = bt_ble_macos.o
    BLE_SYS      = -framework CoreBluetooth -framework Foundation \
                   -framework IOKit -framework IOBluetooth -lpthread
    BLUETOOTH_LIBS =
endif

# ── Targets ───────────────────────────────────────────────────────────────────
PREFIX    ?= /usr/local
BINDIR     = $(PREFIX)/bin

.PHONY: all clean test install uninstall install-deps

all: bbs ax25kiss ax25tnc basic_tool bt_kiss_bridge

$(LIB_OBJ): $(LIB_SRC) ax25lib.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BASIC_OBJ): $(BASIC_SRC) basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -c -o $@ $<

bbs: bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ bbs.cpp $(LIB_OBJ) $(BASIC_OBJ) $(LDUTIL) $(SQLITE_LIBS)

ax25kiss: ax25kiss.cpp
	$(CXX) $(CXXFLAGS) -o $@ ax25kiss.cpp

ax25tnc: ax25tnc.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ ax25tnc.cpp $(LIB_OBJ) $(BASIC_OBJ) $(SQLITE_LIBS)

basic_tool: basic_tool.cpp $(BASIC_OBJ) basic.hpp
	$(CXX) $(CXXFLAGS) $(SQLITE_FLAGS) -o $@ basic_tool.cpp $(BASIC_OBJ) $(SQLITE_LIBS)

test_ax25lib: test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) ax25lib.hpp basic.hpp ini.hpp
	$(CXX) -std=c++17 $(GTEST_CFLAGS) $(SQLITE_FLAGS) \
	    -o $@ test_ax25lib.cpp $(LIB_OBJ) $(BASIC_OBJ) \
	    $(GTEST_LDFLAGS) $(SQLITE_LIBS)

test: test_ax25lib
	./test_ax25lib --gtest_color=yes

# ── Native BLE object compilation ─────────────────────────────────────────────
bt_ble_linux.o: bt_ble_linux.cpp bt_ble_native.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(DBUS_CFLAGS) -c -o $@ $<

bt_ble_macos.o: bt_ble_macos.mm bt_ble_native.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -fobjc-arc -c -o $@ $<

# ── macOS: Classic BT via IOBluetooth (Objective-C++ .mm) ────────────────────
ifneq ($(UNAME), Linux)
    BT_MACOS_OBJ = bt_rfcomm_macos.o
else
    BT_MACOS_OBJ =
endif

bt_rfcomm_macos.o: bt_rfcomm_macos.mm bt_rfcomm_macos.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -fobjc-arc -c -o $@ $<

bt_kiss_bridge: bt_kiss_bridge.cpp $(BLE_OBJ) $(BT_MACOS_OBJ)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(DBUS_CFLAGS) \
	    -o $@ bt_kiss_bridge.cpp $(BLE_OBJ) $(BT_MACOS_OBJ) \
	    $(BLE_SYS) $(BLUETOOTH_LIBS)
	@echo "Built: bt_kiss_bridge"

# Backward-compatible alias: ble_kiss_bridge -> bt_kiss_bridge
ble_kiss_bridge: bt_kiss_bridge
	ln -sf bt_kiss_bridge ble_kiss_bridge
	@echo "Created symlink: ble_kiss_bridge -> bt_kiss_bridge"

clean:
	rm -f $(LIB_OBJ) $(BASIC_OBJ) bbs ax25kiss ax25tnc basic_tool test_ax25lib bt_kiss_bridge ble_kiss_bridge bt_rfcomm_macos.o bt_ble_linux.o bt_ble_macos.o

# ── Install / Uninstall ───────────────────────────────────────────────────────
# Installs all built binaries to $(PREFIX)/bin  (default: /usr/local/bin).
# ble_kiss_bridge is installed only if already built.
# Override prefix:  make install PREFIX=/usr
install:
	@echo "Installing to $(BINDIR) ..."
	install -d $(BINDIR)
	install -m 755 bbs        $(BINDIR)/bbs
	install -m 755 ax25kiss   $(BINDIR)/ax25kiss
	install -m 755 ax25tnc $(BINDIR)/ax25tnc
	install -m 755 basic_tool $(BINDIR)/basic_tool
	@if [ -f bt_kiss_bridge ]; then \
	    install -m 755 bt_kiss_bridge $(BINDIR)/bt_kiss_bridge; \
	    ln -sf bt_kiss_bridge $(BINDIR)/ble_kiss_bridge; \
	    echo "  installed: $(BINDIR)/bt_kiss_bridge (+ ble_kiss_bridge symlink)"; \
	else \
	    echo "  skipped  : bt_kiss_bridge (not built — run: make bt_kiss_bridge)"; \
	fi
	@echo "Done.  Binaries in $(BINDIR):"
	@echo "  bbs  ax25kiss  ax25tnc  basic_tool  bt_kiss_bridge"

uninstall:
	@echo "Removing from $(BINDIR) ..."
	rm -f $(BINDIR)/bbs \
	      $(BINDIR)/ax25kiss \
	      $(BINDIR)/ax25tnc \
	      $(BINDIR)/basic_tool \
	      $(BINDIR)/bt_kiss_bridge \
	      $(BINDIR)/ble_kiss_bridge
	@echo "Done."

install-deps:
	@echo "Install dependencies:"
	@echo "  macOS  : brew install googletest sqlite"
	@echo "  Ubuntu : sudo apt-get install libgtest-dev libsqlite3-dev libdbus-1-dev libbluetooth-dev"
	@echo "  Fedora : sudo dnf install gtest-devel sqlite-devel dbus-devel bluez-libs-devel"
	@echo ""
	@echo "BT/BLE bridge (bt_kiss_bridge):"
	@echo "  Linux  : libdbus-1-dev (BLE via BlueZ D-Bus)"
	@echo "  Linux  : libbluetooth-dev (Classic Bluetooth RFCOMM)"
	@echo "  macOS  : no extra deps (CoreBluetooth + IOBluetooth are system frameworks)"
	@echo "  Build  : make bt_kiss_bridge"
