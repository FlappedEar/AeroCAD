# SPDX-License-Identifier: GPL-3.0-or-later
"""CAD-93: validation success requires every reader, not just the available ones.

The command runner and external readers are controlled fixtures here. The STEP text
check uses actual files; no native reader packages or C++ build are needed.
"""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[2] / "tools" / "validate_exports.py"
SPEC = importlib.util.spec_from_file_location("validate_exports", SOURCE)
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


class ExportValidationTests(unittest.TestCase):
    def run_validator(self, *, import_errors=None, reader_errors=None, valid_step=True):
        import_errors = import_errors or {}
        reader_errors = reader_errors or {}
        output = io.StringIO()

        def load(name):
            if name in import_errors:
                raise import_errors[name]
            return object()

        def reader(name):
            def validate(path, model):
                if name in reader_errors:
                    raise reader_errors[name]
                validator.check(path.is_file(), f"fixture reader {name} opened {path.name}")
            return validate

        def fixture_models():
            return [{"name": "fixture", "steps": [], "volume": 1.0,
                     "size": [1, 1, 1], "parts": 1, "formats": ["stl", "3mf", "step"]}]

        def export(cmd, steps):
            result = [{"command": "document.parts", "result": {
                "parts": [{"bounds": {"size": [1, 1, 1]}}]}}]
            for step in steps:
                if step["name"] != "document.export":
                    continue
                path = Path(step["params"]["path"])
                path.write_text("ISO-10303-21; SI_UNIT(.MILLI.,.METRE.); AUTOMOTIVE_DESIGN"
                                if valid_step else "invalid export")
                result.append({"command": "document.export", "result": {
                    "format": path.suffix[1:], "warnings": [], "checks": [{"volume": 1.0}]}})
            return result

        with tempfile.TemporaryDirectory() as directory:
            cmd = Path(directory) / "aerocad-cmd"
            cmd.touch()
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(output))
                stack.enter_context(patch.object(validator, "models", fixture_models))
                stack.enter_context(patch.object(validator, "run_script", export))
                stack.enter_context(patch.object(validator.importlib, "import_module", load))
                for name, function in (("lib3mf", "validate_3mf_lib3mf"),
                                       ("trimesh", "validate_mesh_trimesh"),
                                       ("gmsh", "validate_step_gmsh")):
                    stack.enter_context(patch.object(validator, function, reader(name)))
                code = validator.main(["--cmd", str(cmd)])
        return code, output.getvalue(), list(validator.COVERAGE)

    def assert_incomplete(self, result):
        code, output, _ = result
        self.assertEqual(code, 1, output)
        self.assertNotIn("All required checks executed and passed", output)
        self.assertNotIn("All checks passed", output)

    def test_all_readers_missing(self):
        result = self.run_validator(import_errors={name: ModuleNotFoundError(name)
                                                   for name in ("lib3mf", "trimesh", "gmsh")})
        self.assert_incomplete(result)
        _, output, rows = result
        self.assertIn("Validation INCOMPLETE: 4 required reader invocation(s) missing", output)
        self.assertEqual(sum(r["executed"] for r in rows), 1)  # STEP text alone is not independent validation.
        self.assertEqual(sum(r["status"] == "missing" for r in rows), 4)
        self.assertIn("STL  trimesh: required 1, executed 0, passed 0, failed 0, missing 1", output)

    def test_one_reader_missing_still_runs_the_others(self):
        result = self.run_validator(import_errors={"gmsh": ImportError("gmsh package missing")})
        self.assert_incomplete(result)
        self.assertEqual(sum(r["status"] == "passed" for r in result[2]), 4)
        self.assertIn("STEP gmsh: required 1, executed 0, passed 0, failed 0, missing 1", result[1])

    def test_native_library_cannot_load(self):
        result = self.run_validator(import_errors={"gmsh": OSError("libGLU.so not found")})
        self.assert_incomplete(result)
        self.assertIn("gmsh cannot load for fixture.step: OSError: libGLU.so not found", result[1])
        self.assertIn("STEP gmsh: required 1, executed 0, passed 0, failed 1, missing 0", result[1])

    def test_native_library_fails_when_reader_is_initialized(self):
        result = self.run_validator(reader_errors={"lib3mf": OSError("native lib3mf unavailable")})
        self.assert_incomplete(result)
        self.assertIn("lib3mf stopped for fixture.3mf: OSError: native lib3mf unavailable", result[1])

    def test_dependency_imported_during_read_is_missing(self):
        result = self.run_validator(reader_errors={"trimesh": ModuleNotFoundError("networkx")})
        self.assert_incomplete(result)
        self.assertIn("MISSING dependency of trimesh", result[1])
        self.assertEqual(sum(r["status"] == "missing" for r in result[2]), 2)

    def test_invalid_export_fails_even_with_every_reader_available(self):
        result = self.run_validator(valid_step=False)
        self.assert_incomplete(result)
        self.assertIn("FAIL  STEP: ISO-10303-21 header", result[1])
        self.assertEqual(sum(r["executed"] for r in result[2]), 5)
        self.assertEqual(sum(r["status"] == "failed" for r in result[2]), 1)

    def test_reader_exception_is_reported_and_other_checks_continue(self):
        result = self.run_validator(reader_errors={"lib3mf": ValueError("malformed model")})
        self.assert_incomplete(result)
        self.assertIn("lib3mf stopped for fixture.3mf: ValueError: malformed model", result[1])
        self.assertEqual(sum(r["status"] == "passed" for r in result[2]), 4)

    def test_reader_cannot_exit_successfully_before_validation_finishes(self):
        result = self.run_validator(reader_errors={"gmsh": SystemExit(0)})
        self.assert_incomplete(result)
        self.assertIn("gmsh stopped for fixture.step: SystemExit: 0", result[1])

    def test_complete_success_requires_all_checks(self):
        code, output, rows = self.run_validator()
        self.assertEqual(code, 0, output)
        self.assertIn("All required checks executed and passed.", output)
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(r["executed"] and r["status"] == "passed" for r in rows))
        self.assertNotIn("INCOMPLETE", output)


if __name__ == "__main__":
    unittest.main()
