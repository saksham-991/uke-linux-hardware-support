CXXFLAGS ?= -O2 -Wall -Wextra
DESTDIR ?=
BINDIR ?= /usr/libexec/uke-focus-pen
UNITDIR ?= /usr/lib/systemd/system
LICENSEDIR ?= /usr/share/licenses/uke-focus-pen
BUILD_DIR ?= build

.PHONY: all install test
all: $(BUILD_DIR)/input $(BUILD_DIR)/pair

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/input: focus-pen.cpp tip-pressure.hpp reference/nvt_stylus.cpp reference/nvt_stylus.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -Ireference focus-pen.cpp reference/nvt_stylus.cpp $(LDFLAGS) -o $@

$(BUILD_DIR)/pair: focus-pen-pair.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 $< $(LDFLAGS) -lsystemd -o $@

$(BUILD_DIR)/test-tip-pressure: test-tip-pressure.cpp tip-pressure.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 $< $(LDFLAGS) -o $@

test: $(BUILD_DIR)/test-tip-pressure
	$(BUILD_DIR)/test-tip-pressure

install: all
	install -Dm755 $(BUILD_DIR)/input $(DESTDIR)$(BINDIR)/input
	install -Dm755 $(BUILD_DIR)/pair $(DESTDIR)$(BINDIR)/pair
	install -Dm644 uke-focus-pen.service $(DESTDIR)$(UNITDIR)/uke-focus-pen.service
	install -Dm644 uke-focus-pen-pair.service $(DESTDIR)$(UNITDIR)/uke-focus-pen-pair.service
	install -Dm644 reference/LICENSE $(DESTDIR)$(LICENSEDIR)/LICENSE
	install -Dm644 reference/README.md $(DESTDIR)$(LICENSEDIR)/ORIGIN.md
