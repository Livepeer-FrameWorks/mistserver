#!/bin/bash
# Run clang-tidy on all C++ files and save output to reports/lint-report.txt
# Similar to monorepo's lint-report.sh for Go

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
REPORT_DIR="$ROOT_DIR/reports"
REPORT_FILE="$REPORT_DIR/lint-report.txt"

# Find clang-tidy
CLANG_TIDY="${CLANG_TIDY:-$(command -v clang-tidy 2>/dev/null || echo "/opt/homebrew/Cellar/llvm/21.1.8/bin/clang-tidy")}"

if [ ! -x "$CLANG_TIDY" ]; then
    echo "Error: clang-tidy not found"
    echo "Set CLANG_TIDY env var or install via: brew install llvm"
    exit 1
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "Error: compile_commands.json not found in $BUILD_DIR"
    echo "Run: meson setup build"
    exit 1
fi

mkdir -p "$REPORT_DIR"
> "$REPORT_FILE"

echo "Running clang-tidy on all C++ source files..."
echo "Using: $CLANG_TIDY"
echo ""

# Define source directories to analyze (like Go modules in monorepo)
COMPONENTS=(
    "src/controller:Controller"
    "src/input:Inputs"
    "src/output:Outputs"
    "src/process:Processors"
    "src/analysers:Analyzers"
    "src/utils:Utilities"
    "lib:Core Library"
    "test:Tests"
)

for component in "${COMPONENTS[@]}"; do
    dir="${component%%:*}"
    name="${component##*:}"

    if [ ! -d "$ROOT_DIR/$dir" ]; then
        continue
    fi

    echo "==> $name ($dir)"
    echo "=== COMPONENT: $name ($dir) ===" >> "$REPORT_FILE"

    # Find .cpp files in this component
    files=$(find "$ROOT_DIR/$dir" -name "*.cpp" -type f 2>/dev/null | sort)

    for file in $files; do
        # Run clang-tidy and capture output
        output=$("$CLANG_TIDY" -p "$BUILD_DIR" "$file" 2>&1 | grep -E "warning:|error:" | grep -v "in non-user code" | grep -v "Suppressed" || true)

        if [ -n "$output" ]; then
            # Make paths relative
            echo "$output" | sed "s|$ROOT_DIR/||g" >> "$REPORT_FILE"
        fi
    done

    echo "" >> "$REPORT_FILE"
done

echo ""
echo "Report saved to: $REPORT_FILE"
echo "Run 'scripts/lint-analyze.sh' to see summary"
