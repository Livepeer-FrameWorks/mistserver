#!/bin/bash
# Run clang-tidy on MistServer source files
#
# Usage:
#   ./scripts/run-clang-tidy.sh [files...]
#   ./scripts/run-clang-tidy.sh              # runs on all source files
#   ./scripts/run-clang-tidy.sh src/foo.cpp  # runs on specific file(s)
#
# Prerequisites:
#   - clang-tidy installed
#   - compile_commands.json generated via: meson setup builddir
#
# Options:
#   --fix    Apply suggested fixes automatically
#   --diff   Only check files changed since main branch

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/builddir}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check for clang-tidy
if ! command -v clang-tidy &> /dev/null; then
    echo -e "${RED}Error: clang-tidy not found${NC}"
    echo "Install with:"
    echo "  macOS:  brew install llvm"
    echo "  Ubuntu: apt install clang-tidy"
    exit 1
fi

# Check for compile_commands.json
if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo -e "${YELLOW}Warning: compile_commands.json not found${NC}"
    echo "Generating with meson..."
    meson setup "$BUILD_DIR" "$ROOT_DIR" || {
        echo -e "${RED}Failed to generate compile_commands.json${NC}"
        exit 1
    }
fi

# Parse arguments
FIX_MODE=""
DIFF_MODE=""
FILES=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --fix)
            FIX_MODE="--fix"
            shift
            ;;
        --diff)
            DIFF_MODE="1"
            shift
            ;;
        *)
            FILES+=("$1")
            shift
            ;;
    esac
done

# If diff mode, get changed files
if [ -n "$DIFF_MODE" ]; then
    echo -e "${YELLOW}Checking files changed since main branch...${NC}"
    CHANGED=$(git diff --name-only --diff-filter=ACMR origin/master...HEAD -- '*.cpp' '*.h' '*.hpp' 2>/dev/null || \
              git diff --name-only --diff-filter=ACMR HEAD~10...HEAD -- '*.cpp' '*.h' '*.hpp')
    if [ -z "$CHANGED" ]; then
        echo -e "${GREEN}No C++ files changed${NC}"
        exit 0
    fi
    FILES=($CHANGED)
fi

# If no files specified, find all source files (excluding certain directories)
if [ ${#FILES[@]} -eq 0 ]; then
    echo -e "${YELLOW}Finding all source files...${NC}"
    FILES=($(find "$ROOT_DIR" \
        -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
        -not -path '*/lsp/*' \
        -not -path '*/embed/*' \
        -not -path '*/subprojects/*' \
        -not -path '*/builddir/*' \
        -not -path '*/.git/*' \
        | sort))
fi

echo -e "${GREEN}Running clang-tidy on ${#FILES[@]} files...${NC}"

# Run clang-tidy
FAILED=0
for file in "${FILES[@]}"; do
    # Convert to absolute path if needed
    if [[ ! "$file" = /* ]]; then
        file="$ROOT_DIR/$file"
    fi

    # Skip if file doesn't exist
    if [ ! -f "$file" ]; then
        continue
    fi

    echo "Checking: $(basename "$file")"
    if ! clang-tidy -p "$BUILD_DIR" $FIX_MODE "$file" 2>&1; then
        FAILED=1
    fi
done

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC}"
else
    echo -e "${RED}Some checks failed${NC}"
    exit 1
fi
