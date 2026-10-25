"""Runs the Python suites and prints one PASS/FAIL/SKIP line per test (used by run.sh)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


class LineResult(unittest.TextTestResult):
    def _emit(self, status, name):
        print("%s %s" % (status, name), flush=True)

    def addSuccess(self, test):
        super().addSuccess(test)
        self._emit("PASS", test.id())

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._emit("SKIP", "%s (%s)" % (test.id(), reason))

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._emit("FAIL", test.id())

    def addError(self, test, err):
        super().addError(test, err)
        self._emit("FAIL", test.id() + " (error)")

    def addSubTest(self, test, subtest, err):
        if err is not None:
            self._emit("FAIL", subtest.id())
        super().addSubTest(test, subtest, err)


def main():
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for name in ("test_host_protocol", "test_host_fullscreen", "test_host_mpv"):
        suite.addTests(loader.loadTestsFromName(name))
    runner = unittest.TextTestRunner(stream=open(os.devnull, "w"), resultclass=LineResult, verbosity=0)
    result = runner.run(suite)
    skipped = len(result.skipped)
    passed = result.testsRun - len(result.failures) - len(result.errors) - skipped
    print("PYTHON SUMMARY: %d PASS, %d FAIL, %d SKIP" % (max(passed, 0), len(result.failures) + len(result.errors), skipped),
          flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
