"""Validate a release tag against the ReRevved project version."""

import argparse

from package import read_version


def classify_tag(tag, version):
    release_tag = f"v{version}"
    if tag == release_tag:
        return False
    if tag.startswith(release_tag + "-") and len(tag) > len(release_tag) + 1:
        return True
    raise ValueError(
        f"tag {tag} does not match project version {version}; "
        f"expected {release_tag} or {release_tag}-<suffix>"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    args = parser.parse_args()

    try:
        prerelease = classify_tag(args.tag, read_version())
    except ValueError as error:
        parser.error(str(error))

    print(str(prerelease).lower())


if __name__ == "__main__":
    main()
