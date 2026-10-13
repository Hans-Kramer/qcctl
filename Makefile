# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer

CC      ?= gcc
# The board is aarch64 and has no toolchain, so binaries are cross-built here.
CROSS   ?= aarch64-linux-gnu-gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Werror
# -MMD -MP: header dependencies, so editing a header rebuilds what includes it.
CFLAGS  += -MMD -MP
LDFLAGS ?=

# The version every binary reports with --version. One line to change for a release, and
# `make VERSION=0.2.0` overrides it without editing anything.
#
# Once this tree is under git, the literal can become a describe:
#   VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo 0.1.0-pre)
# which is the point of having it here rather than in a header - the build knows what the
# source cannot.
VERSION ?= 0.1.0-pre
CFLAGS  += -DQCCTL_VERSION=\"$(VERSION)\"

# Every build artifact lands under $(B); nothing is written to the source tree.
B := build

SRC_COMMON := src/io.c src/surface.c src/config.c src/apply.c src/fancurve.c src/stats.c src/fanown.c src/control.c src/fanbase.c src/envid.c
OBJ_COMMON := $(patsubst %.c,$(B)/%.o,$(SRC_COMMON))
DEPS       := $(OBJ_COMMON:.o=.d) $(B)/src/surface_dump.d $(B)/src/profile_check.d \
              $(B)/src/main.d $(B)/src/daemon.d

BIN  := $(B)/surface-dump
BIN2 := $(B)/qcctl-check
BIN3 := $(B)/qcctl
BIN4 := $(B)/qcctld

# Where `make stage` assembles the flat directory the installer wants, and where it finds
# the cross-built qcstats it cannot build itself.
STAGE   ?= /tmp/qcctl-stage
QCSTATS ?= ../qcstats/qcstats

CONF    ?= etc/profiles.conf
FANCONF ?= etc/fan.config

all: $(BIN) $(BIN2) $(BIN3) $(BIN4)

$(BIN): $(B)/src/surface_dump.o $(OBJ_COMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN2): $(B)/src/profile_check.o $(OBJ_COMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN3): $(B)/src/main.o $(OBJ_COMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN4): $(B)/src/daemon.o $(OBJ_COMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# mkdir per object: the build tree mirrors the source tree.
$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# A real optimised compile under both compilers, not -fsyntax-only: the optimiser is
# what finds -Wformat-truncation and friends.
check: clean
	@echo "== gcc =="
	@$(MAKE) --no-print-directory CC=gcc all
	@$(MAKE) --no-print-directory clean
	@# Guarded: an absent clang used to abort check with exit 127 and leave build/ empty.
	@if command -v clang >/dev/null 2>&1; then \
	    echo "== clang =="; \
	    $(MAKE) --no-print-directory CC=clang all; \
	    $(MAKE) --no-print-directory clean; \
	else echo "== clang not installed, skipped"; fi
	@echo "== cross-compile for the board (aarch64) =="
	@if command -v $(CROSS) >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory CC=$(CROSS) all >/dev/null && echo "  builds for aarch64"; \
	    $(MAKE) --no-print-directory clean; \
	else echo "  SKIPPED: $(CROSS) not installed"; fi
	@# Always leave build/ native and populated, whichever branches above ran.
	@$(MAKE) --no-print-directory all >/dev/null
	@echo OK

# Assemble the flat staging directory the installer works from, cross-building first and
# restoring the native tree afterwards.
#
# Two invariants, both of which have been broken by hand before:
#   - every staged binary is aarch64, so a host binary cannot reach the board
#   - build/ is left NATIVE, so nothing here is later run or tested as a board binary
stage:
	@# $(STAGE) ends up either complete and verified or absent. Removing it first is what
	@# makes that true: a leftover directory is as deployable as a half-written one.
	@rm -rf $(STAGE)
	@# qcstats is validated before anything is built, so a failure leaves the tree alone.
	@if [ ! -f "$(QCSTATS)" ]; then \
	    echo "stage: no qcstats at $(QCSTATS)"; \
	    echo "  it is a hard dependency and a separate project. Cross-build it with:"; \
	    echo "    make -C $(dir $(QCSTATS)) cross"; \
	    echo "  or point this at one:  make stage QCSTATS=/path/to/qcstats"; \
	    exit 1; fi
	@# e_machine sits at ELF offset 18; 0xb7 is aarch64. od, not file(1): always present.
	@m=$$(od -An -tx1 -j18 -N1 "$(QCSTATS)" | tr -d ' \n'); \
	 if [ "$$m" != b7 ]; then \
	    echo "stage: $(QCSTATS) is NOT an aarch64 binary (ELF e_machine=0x$$m)"; \
	    echo "  staging a host binary installs cleanly and then fails on the board."; \
	    echo "  cross-build it:  make -C $(dir $(QCSTATS)) cross"; \
	    exit 1; fi
	@$(MAKE) --no-print-directory cross >/dev/null
	@# A copy failure takes the same exit as a verification failure below: restore the
	@# native tree and remove $(STAGE), so no partial directory is left to deploy.
	@mkdir -p $(STAGE) && \
	 cp $(BIN) $(BIN2) $(BIN3) $(BIN4) $(STAGE)/ && \
	 cp etc/profiles.conf etc/fan.config $(STAGE)/ && \
	 cp systemd/qcctld.service systemd/qcctl-resume.service $(STAGE)/ && \
	 cp polkit/io.github.hans_kramer.qcctl.policy $(STAGE)/ && \
	 cp scripts/install.sh scripts/uninstall.sh scripts/fan-release.sh $(STAGE)/ && \
	 cp docs/USAGE.md $(STAGE)/ && \
	 cp $(QCSTATS) $(STAGE)/ || { \
	    $(MAKE) --no-print-directory clean >/dev/null; \
	    $(MAKE) --no-print-directory all >/dev/null; \
	    rm -rf $(STAGE); \
	    echo "stage: could not stage a file - removed $(STAGE), tree restored to native"; \
	    exit 1; }
	@# The restore runs whatever the verdict: leaving board binaries in build/ on the way
	@# out would recreate the bug this target exists to prevent.
	@fail=0; \
	 for f in $(STAGE)/qcctl $(STAGE)/qcctld $(STAGE)/qcctl-check $(STAGE)/surface-dump $(STAGE)/qcstats; do \
	    m=$$(od -An -tx1 -j18 -N1 "$$f" | tr -d ' \n'); \
	    [ "$$m" = b7 ] || { echo "stage: $$f is not aarch64 (e_machine=0x$$m)"; fail=1; }; \
	 done; \
	 $(MAKE) --no-print-directory clean >/dev/null; \
	 $(MAKE) --no-print-directory all >/dev/null; \
	 if [ "$$fail" != 0 ]; then \
	    rm -rf $(STAGE); \
	    echo "stage: removed $(STAGE) - a half-staged directory is one someone will deploy."; \
	    exit 1; \
	 fi
	@echo "  staged $$(ls -1 $(STAGE) | wc -l) files in $(STAGE), all aarch64"
	@echo "  this tree is back to NATIVE binaries, so nothing here is left for the board"
	@echo
	@echo "  next:  scp $(STAGE)/* <board>:/tmp/stage/"
	@echo "         ssh <board> 'sudo sh /tmp/stage/install.sh --upgrade'"

# Assemble the same flat directory from a NATIVE build, for installing on the machine
# you are sitting at - which on the board means building and installing in one place.
#
# No cross-compiler and no aarch64 check: the test is that every staged binary actually
# RUNS here, which is what "native" has to mean for an installer that is about to use it.
stage-native: all
	@rm -rf $(STAGE)
	@if [ ! -f "$(QCSTATS)" ]; then \
	    echo "stage-native: no qcstats at $(QCSTATS)"; \
	    echo "  it is a hard dependency and a separate project. Build it with:"; \
	    echo "    make -C $(dir $(QCSTATS))"; \
	    echo "  or point this at one:  make stage-native QCSTATS=/path/to/qcstats"; \
	    exit 1; fi
	@mkdir -p $(STAGE) && \
	 cp $(BIN) $(BIN2) $(BIN3) $(BIN4) $(STAGE)/ && \
	 cp etc/profiles.conf etc/fan.config $(STAGE)/ && \
	 cp systemd/qcctld.service systemd/qcctl-resume.service $(STAGE)/ && \
	 cp polkit/io.github.hans_kramer.qcctl.policy $(STAGE)/ && \
	 cp scripts/install.sh scripts/uninstall.sh scripts/fan-release.sh $(STAGE)/ && \
	 cp docs/USAGE.md $(STAGE)/ && \
	 cp $(QCSTATS) $(STAGE)/ || { \
	    rm -rf $(STAGE); \
	    echo "stage-native: could not stage a file - removed $(STAGE)"; \
	    exit 1; }
	@# /bin/sh is this machine's own architecture by definition, so its e_machine is the
	@# one every staged binary has to match. Exit codes cannot do this job: qcctl-check
	@# exits 2 for a missing config, which says nothing about the architecture.
	@host=$$(od -An -tx1 -j18 -N1 /bin/sh | tr -d ' \n'); \
	 fail=0; \
	 for f in $(STAGE)/qcctl $(STAGE)/qcctld $(STAGE)/qcctl-check $(STAGE)/surface-dump $(STAGE)/qcstats; do \
	    m=$$(od -An -tx1 -j18 -N1 "$$f" | tr -d ' \n'); \
	    [ "$$m" = "$$host" ] || { echo "stage-native: $$f is e_machine=0x$$m, this machine is 0x$$host"; fail=1; }; \
	 done; \
	 if [ "$$fail" != 0 ]; then \
	    rm -rf $(STAGE); \
	    echo "stage-native: removed $(STAGE) - a half-staged directory is one someone will deploy."; \
	    exit 1; fi
	@echo "  staged $$(ls -1 $(STAGE) | wc -l) files in $(STAGE), all native to this machine"
	@echo
	@echo "  next:  sudo sh $(STAGE)/install.sh"

# Cross-build for the board. 'char' is unsigned on aarch64 and signed on x86, so a
# native-only gate cannot see a whole class of bug.
#
# Leaves the aarch64 BINARIES for deployment and removes the aarch64 objects, which a
# native build cannot link - `make all` would die with "file in wrong format".
cross:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory CC=$(CROSS) all
	@file $(BIN3) | sed 's/^/  /'
	@$(MAKE) --no-print-directory clean-objs
	@echo "  (aarch64 objects removed; the binaries in $(B)/ are now for the BOARD,"
	@echo "   so run 'make all' before testing natively)"

# Objects and dependency files only, leaving the binaries in place.
clean-objs:
	@find $(B) -name '*.o' -o -name '*.d' | xargs -r rm -f

clean:
	rm -rf $(B)

.PHONY: all check cross stage stage-native clean clean-objs
-include $(DEPS)
