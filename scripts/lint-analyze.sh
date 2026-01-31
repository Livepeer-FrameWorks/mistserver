#!/bin/bash
# Analyze lint report and show summary by linter and by file
# Similar to analyzing golangci-lint output in monorepo

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
REPORT_FILE="$ROOT_DIR/reports/lint-report.txt"

if [ ! -f "$REPORT_FILE" ]; then
    echo "No report found. Run scripts/lint-report.sh first."
    exit 1
fi

echo "======================================"
echo "  MistServer Lint Report Analysis"
echo "======================================"
echo ""

# Count by linter
echo "## By Linter"
echo ""
printf "%-45s %s\n" "Linter" "Count"
printf "%-45s %s\n" "---------------------------------------------" "-----"
grep -oE "\[[-a-zA-Z]+\]" "$REPORT_FILE" | sort | uniq -c | sort -rn | while read count linter; do
    printf "%-45s %5d\n" "$linter" "$count"
done
echo ""

# Count by component
echo "## By Component"
echo ""
printf "%-30s %s\n" "Component" "Warnings"
printf "%-30s %s\n" "------------------------------" "--------"
current_component=""
count=0
while IFS= read -r line; do
    if [[ "$line" =~ ^"=== COMPONENT:" ]]; then
        if [ -n "$current_component" ] && [ $count -gt 0 ]; then
            printf "%-30s %5d\n" "$current_component" "$count"
        fi
        current_component=$(echo "$line" | sed 's/=== COMPONENT: \(.*\) ===/\1/')
        count=0
    elif [[ "$line" =~ warning: ]]; then
        ((count++))
    fi
done < "$REPORT_FILE"
# Print last component
if [ -n "$current_component" ] && [ $count -gt 0 ]; then
    printf "%-30s %5d\n" "$current_component" "$count"
fi
echo ""

# Count by file (top 20)
echo "## Top 20 Files by Warning Count"
echo ""
printf "%-60s %s\n" "File" "Warnings"
printf "%-60s %s\n" "------------------------------------------------------------" "--------"
grep -oE "^[^:]+\.cpp:[0-9]+:[0-9]+:" "$REPORT_FILE" | cut -d: -f1 | sort | uniq -c | sort -rn | head -20 | while read count file; do
    printf "%-60s %5d\n" "$file" "$count"
done
echo ""

# High priority issues
echo "## High Priority Issues"
echo ""
echo "### Potential Infinite Loops [bugprone-infinite-loop]"
grep "bugprone-infinite-loop" "$REPORT_FILE" | head -10 || echo "  (none found)"
echo ""

echo "### Unhandled Self-Assignment [bugprone-unhandled-self-assignment]"
grep "bugprone-unhandled-self-assignment" "$REPORT_FILE" | head -10 || echo "  (none found)"
echo ""

# Total
total=$(grep -c "warning:" "$REPORT_FILE" 2>/dev/null || echo "0")
echo "======================================"
echo "  Total Warnings: $total"
echo "======================================"
