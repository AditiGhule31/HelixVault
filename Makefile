# =============================================================================
# HelixVault Makefile
#
# Targets:
#   make          — build the helixvault binary (default)
#   make debug    — build with debug symbols and ASan/UBSan
#   make clean    — remove build artefacts
#   make test     — run the built-in self-test
#   make demo     — run an end-to-end demonstration
#   make check    — run self-test + verify exit code
# =============================================================================

# ── Compiler & Flags ──────────────────────────────────────────────────────────

CC      := gcc
BINARY  := helixvault

SRC_DIR := src
INC_DIR := include

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, build/%.o, $(SRCS))

# Standard flags: C11, warnings, include path, POSIX feature-test macro
CFLAGS_BASE := -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-function \
               -I$(INC_DIR) -D_POSIX_C_SOURCE=200809L

# Release flags
CFLAGS_REL  := $(CFLAGS_BASE) -O2 -DNDEBUG

# Debug flags: no optimisation, full debug info, sanitisers (GCC/Clang)
CFLAGS_DBG  := $(CFLAGS_BASE) -O0 -g3 \
               -fsanitize=address,undefined \
               -fno-omit-frame-pointer

LDFLAGS     :=

# Windows detection (MinGW)
ifeq ($(OS), Windows_NT)
  BINARY  := helixvault.exe
  LDFLAGS += -lkernel32
  CFLAGS_BASE := $(filter-out -D_POSIX_C_SOURCE=200809L, $(CFLAGS_BASE))
  # MinGW does not need _POSIX_C_SOURCE; it uses Win32 APIs directly.
endif

# ── Default: release build ────────────────────────────────────────────────────

CFLAGS ?= $(CFLAGS_REL)

.PHONY: all debug clean test demo check

all: $(BINARY)

# ── Link ─────────────────────────────────────────────────────────────────────

$(BINARY): $(OBJS)
	@echo "[LD]  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "Build complete: ./$@"
	@echo "  Run './$(BINARY) selftest' to verify."

# ── Compile individual translation units ─────────────────────────────────────

build/%.o: $(SRC_DIR)/%.c | build
	@echo "[CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

# ── Debug build ───────────────────────────────────────────────────────────────

debug:
	$(MAKE) CFLAGS="$(CFLAGS_DBG)" all

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	rm -rf build $(BINARY) helixvault.exe storage_root
	@echo "Clean."

# ── Tests & Demo ─────────────────────────────────────────────────────────────

test: $(BINARY)
	@echo "=== Running self-test ==="
	./$(BINARY) selftest

check: test
	@echo "Self-test passed."

# demo: create bucket, upload this Makefile and main.c, list, retrieve, delete
demo: $(BINARY)
	@echo "=== HelixVault Demo ==="
	./$(BINARY) mb demo-bucket
	./$(BINARY) put demo-bucket Makefile      Makefile
	./$(BINARY) put demo-bucket src/main.c    $(SRC_DIR)/main.c
	./$(BINARY) put demo-bucket src/chunker.c $(SRC_DIR)/chunker.c
	./$(BINARY) ls  demo-bucket
	./$(BINARY) info demo-bucket Makefile
	./$(BINARY) get demo-bucket Makefile /tmp/Makefile.retrieved || true
	./$(BINARY) del demo-bucket Makefile
	./$(BINARY) del demo-bucket src/main.c
	./$(BINARY) del demo-bucket src/chunker.c
	./$(BINARY) rb  demo-bucket
	./$(BINARY) stats
	@echo "=== Demo complete ==="

# ── Help ──────────────────────────────────────────────────────────────────────

help:
	@echo "Targets:"
	@echo "  make         Build release binary"
	@echo "  make debug   Build with ASan/UBSan debug flags"
	@echo "  make clean   Remove build artefacts and storage_root"
	@echo "  make test    Run built-in self-test"
	@echo "  make check   Run self-test and return non-zero on failure"
	@echo "  make demo    End-to-end demonstration"
