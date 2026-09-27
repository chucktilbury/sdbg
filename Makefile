# Convenience wrapper. The real build is CMake.
BUILD_DIR ?= build
CMAKE ?= cmake

.PHONY: all configure build test clean install

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

install: build
	$(CMAKE) --install $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	rm -f src/*.o tests/test_backend tests/test_pty tests/test_session
