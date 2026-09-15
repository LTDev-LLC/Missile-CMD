.PHONY: all build build-all repro-check install sdk-update sdk-status sdk-lint format-check test screenshots screenshots-check lint format clang-format clean

FIRMWARE ?= official
export FIRMWARE

CORE_SOURCES = $(shell python3 tools/sources.py core)
UI_SOURCES = $(shell python3 tools/sources.py ui)
TEST_FLAGS = -DMC_HOST_TEST -std=c11 -Wall -Wextra -Werror -Isrc/include -Ibuild/generated $(TEST_SANITIZERS)

all: build

.PHONY: help-assets
help-assets:
	python3 tools/help_assets.py

.PHONY: web-test
web-test:
	node --test tests/web/*.test.mjs
	python3 -m unittest discover -s tests -p 'test_pages.py'
	python3 -m unittest discover -s tests -p 'test_build_config.py'

test screenshots screenshots-check: help-assets

build build-all repro-check install sdk-update sdk-status sdk-lint:
	python3 tools/firmware.py $@ --firmware "$(FIRMWARE)"

test:
	mkdir -p build
	$(CC) $(TEST_FLAGS) -Itests/stubs tests/test_canvas.c tests/host_canvas.c -o build/test_canvas
	./build/test_canvas
	$(CC) $(TEST_FLAGS) tests/test_core.c $(CORE_SOURCES) -o build/test_core
	./build/test_core
	$(CC) $(TEST_FLAGS) tests/test_compatibility.c $(CORE_SOURCES) -o build/test_compatibility
	./build/test_compatibility tests/fixtures/pre-sharing.bin
	$(CC) $(TEST_FLAGS) -Itests/stubs tests/test_migration.c tests/host_platform.c tests/host_canvas.c $(CORE_SOURCES) $(UI_SOURCES) -o build/test_migration
	./build/test_migration
	$(CC) $(TEST_FLAGS) -Itests/stubs tests/test_app.c tests/host_platform.c tests/host_canvas.c $(CORE_SOURCES) $(UI_SOURCES) -o build/test_app
	./build/test_app
	$(CC) $(TEST_FLAGS) -DMC_HOST_THREADS -pthread -Itests/stubs tests/test_threaded.c tests/host_platform.c tests/host_canvas.c $(CORE_SOURCES) $(UI_SOURCES) -o build/test_threaded
	./build/test_threaded
	python3 -m unittest discover -s tests -p 'test_*.py'

screenshots:
	CC="$(CC)" python3 tools/screenshots.py

screenshots-check:
	CC="$(CC)" python3 tools/screenshots.py --check

lint:
	python3 tools/firmware.py lint --firmware "$(FIRMWARE)"

format: clang-format

clang-format:
	python3 tools/firmware.py format --firmware "$(FIRMWARE)"

clean:
	rm -rf build dist build_metadata tests/__pycache__ tools/__pycache__

format-check:
	python3 tools/format_sources.py --check
