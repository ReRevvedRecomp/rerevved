import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import check_release_tag


class ReleaseTagTest(unittest.TestCase):
    def test_stable_tag_matches_project_version(self):
        self.assertFalse(check_release_tag.classify_tag("v1.2.3", "1.2.3"))

    def test_prerelease_suffix_is_permitted(self):
        self.assertTrue(check_release_tag.classify_tag("v1.2.3-rc.1", "1.2.3"))

    def test_mismatched_or_malformed_tags_are_rejected(self):
        for tag in ("v1.2.4", "1.2.3", "v1.2.3rc1", "v1.2.3-"):
            with self.subTest(tag=tag):
                with self.assertRaisesRegex(ValueError, "does not match project version"):
                    check_release_tag.classify_tag(tag, "1.2.3")

    def test_release_workflow_enforces_and_uses_classification(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(
            encoding="ascii"
        )
        self.assertIn("python .\\scripts\\check_release_tag.py $env:TAG", workflow)
        self.assertIn(
            "prerelease: ${{ steps.release-tag.outputs.prerelease }}", workflow
        )
        self.assertIn("needs: [verify, build]", workflow)
        self.assertIn(
            "prerelease: ${{ needs.verify.outputs.prerelease == 'true' }}", workflow
        )


if __name__ == "__main__":
    unittest.main()
