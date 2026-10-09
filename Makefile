PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=spatial
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

TEST_FLAGS:=--batch-size 1 --batch-timeout 300

# Stabilize all tests in CI
ifdef CI
TEST_FLAGS+= --stabilize-tests
endif

T ?= $(TEST_FLAGS) "test/*"

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

unittest_relassert:
	build/relassert/test/run $(T)


#### Override the included format target because we have different source tree layout
format:
	find src/spatial -iname *.hpp -o -iname *.cpp | xargs clang-format --sort-includes=0 -style=file -i
	cmake-format -i CMakeLists.txt
