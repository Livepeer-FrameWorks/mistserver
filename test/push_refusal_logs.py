#!/usr/bin/env python3
"""Keep publishing URLs out of every connector's refusal diagnostics."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CALL = re.compile(r"\b(?:[A-Z]+_MSG|logExitReason|onFail)\s*\((.*?)\);", re.S)


class PushRefusalLogs(unittest.TestCase):
    def test_every_output_omits_publishing_urls_on_refusal(self):
        leaks = []
        for path in sorted((ROOT / "src/output").glob("*.cpp")):
            source = path.read_text()
            for match in CALL.finditer(source):
                call = match.group(1)
                if (re.search(r"push.*(?:reject|refus|not allowed)", call, re.I | re.S)
                        and re.search(r"\b(?:reqUrl|qUrl)\b", call)):
                    line = source.count("\n", 0, match.start()) + 1
                    leaks.append(f"{path.name}:{line}: {call.strip()}")
        self.assertEqual(leaks, [], "Publishing credentials in refusal logs:\n" + "\n".join(leaks))


if __name__ == "__main__":
    unittest.main()
