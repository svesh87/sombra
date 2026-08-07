CMAKE ?= cmake
CTEST ?= ctest
GCOVR ?= gcovr
JOBS ?= $(shell nproc)

BUILD_DIR ?= build
COVERAGE_DIR ?= build-coverage
PACKAGE_DIR ?= build-package
BUILD_TYPE ?= Debug
COVERAGE_MINIMUM ?= 80

.PHONY: all configure build test coverage configure-coverage install clean gates lint package

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DBUILD_TESTING=ON \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

configure-coverage:
	$(CMAKE) -S . -B $(COVERAGE_DIR) \
		-DBUILD_TESTING=ON \
		-DENABLE_COVERAGE=ON \
		-DCMAKE_BUILD_TYPE=Debug

coverage: configure-coverage
	$(CMAKE) --build $(COVERAGE_DIR) -j$(JOBS)
	$(CTEST) --test-dir $(COVERAGE_DIR) --output-on-failure
	@if command -v $(GCOVR) >/dev/null 2>&1; then \
		$(GCOVR) --root . \
			--filter 'src/.*\.cpp$$' \
			--exclude '(^|.*/)tests/.*' \
			--exclude '(^|.*/)build[^/]*/.*' \
			--exclude '.*mocs_compilation\.cpp$$' \
			--fail-under-line $(COVERAGE_MINIMUM) \
			--print-summary; \
	else \
		./scripts/check_coverage.sh $(COVERAGE_DIR) $(COVERAGE_MINIMUM); \
	fi

lint:
	./scripts/lint.sh

# Builds the deb and checks what came out of it. Needs no root: the package is
# inspected, never installed.
package:
	./scripts/check_package.sh $(PACKAGE_DIR)

# Everything that has to be green before work is handed over.
gates: lint test coverage
	git diff --check

install: build
	$(CMAKE) --install $(BUILD_DIR)

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR) $(COVERAGE_DIR) $(PACKAGE_DIR)
