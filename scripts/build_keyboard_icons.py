"""Build the Keyboard button-icon DDS files from the imported Xelu key art.

Resizes each 100x100 RGBA keycap PNG under thirdparty/xelu-prompts/Light to a
64x64 legacy B8G8R8A8 DDS named after the SDK key name. KEYS is the single art
list and must match the key table in src/keyboard_icons.cpp.

    python scripts/build_keyboard_icons.py          # rewrite the DDS files
    python scripts/build_keyboard_icons.py --check  # compare, write nothing
"""

import argparse
import pathlib
import struct
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO / "thirdparty" / "xelu-prompts" / "Light"
OUTPUT_DIR = REPO / "assets" / "button-icons" / "xelu-light"

SOURCE_SIZE = 100
ICON_SIZE = 64
DDS_SIZE = 128 + ICON_SIZE * ICON_SIZE * 4

# No art for Comma, Period, Backslash, F13-F24, numpad, Pause, ScrollLock: the retail A icon stays.
KEYS = {
    **{letter: letter for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
    **{digit: digit for digit in "0123456789"},
    **{f"F{n}": f"F{n}" for n in range(1, 13)},
    "Backtick": "Tilda",
    "Minus": "Minus",
    "Plus": "Plus",
    "Semicolon": "Semicolon",
    "Slash": "Slash",
    "LBracket": "Bracket_Left",
    "RBracket": "Bracket_Right",
    "Quote": "Quote",
    "Escape": "Esc",
    "Return": "Enter",
    "Space": "Space",
    "Tab": "Tab",
    "Backspace": "Backspace",
    "Delete": "Del",
    "Insert": "Insert",
    "Home": "Home",
    "End": "End",
    "PageUp": "Page_Up",
    "PageDown": "Page_Down",
    "Left": "Arrow_Left",
    "Right": "Arrow_Right",
    "Up": "Arrow_Up",
    "Down": "Arrow_Down",
    "Shift": "Shift",
    "Control": "Ctrl",
    "Alt": "Alt",
    "CapsLock": "Caps_Lock",
    "NumLock": "Num_Lock",
    "PrintScreen": "Print_Screen",
    "LMB": "Mouse_Left",
    "RMB": "Mouse_Right",
    "MMB": "Mouse_Middle",
}


def source_path(sdk_name):
    return SOURCE_DIR / f"{KEYS[sdk_name]}_Key_Light.png"


def render(sdk_name):
    """Return the DDS bytes for one key."""
    from PIL import Image

    image = Image.open(source_path(sdk_name))
    if image.size != (SOURCE_SIZE, SOURCE_SIZE) or image.mode != "RGBA":
        raise SystemExit(
            f"error: {source_path(sdk_name)} must be {SOURCE_SIZE}x{SOURCE_SIZE} "
            f"RGBA, got {image.size} {image.mode}"
        )
    # Pillow premultiplies RGBA around the filter, so alpha stays straight.
    image = image.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
    pixels = image.tobytes("raw", "BGRA")
    return build_dds(pixels)


def build_dds(pixels):
    if len(pixels) != ICON_SIZE * ICON_SIZE * 4:
        raise ValueError("unexpected pixel byte count")
    data = bytearray()
    data += b"DDS "
    data += struct.pack("<I", 124)  # header size
    data += struct.pack("<I", 0x100F)  # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
    data += struct.pack("<I", ICON_SIZE)
    data += struct.pack("<I", ICON_SIZE)
    data += struct.pack("<I", ICON_SIZE * 4)
    data += struct.pack("<I", 0)  # depth
    data += struct.pack("<I", 0)  # mip count
    data += bytes(44)  # reserved
    data += struct.pack("<I", 32)  # pixel format size
    data += struct.pack("<I", 0x41)  # RGB|ALPHAPIXELS
    data += struct.pack("<I", 0)  # FourCC
    data += struct.pack("<I", 32)  # bits per pixel
    data += struct.pack("<4I", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    data += struct.pack("<I", 0x1000)  # caps: texture
    data += bytes(16)  # caps2, caps3, caps4, reserved
    data += pixels
    assert len(data) == DDS_SIZE
    return bytes(data)


def generate():
    return {f"{name}.dds": render(name) for name in KEYS}


def check():
    expected = generate()
    problems = []
    present = {path.name for path in OUTPUT_DIR.glob("*") if path.is_file()}
    for name in sorted(expected):
        path = OUTPUT_DIR / name
        if not path.is_file():
            problems.append(f"missing: {name}")
        elif path.read_bytes() != expected[name]:
            problems.append(f"differs: {name}")
    for name in sorted(present - set(expected)):
        problems.append(f"unexpected: {name}")
    return problems


def write():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    expected = generate()
    for name, data in expected.items():
        (OUTPUT_DIR / name).write_bytes(data)
    print(f"wrote {len(expected)} files to {OUTPUT_DIR}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="regenerate in memory and compare with the checked-in files",
    )
    args = parser.parse_args()
    if args.check:
        problems = check()
        for problem in problems:
            print(problem)
        if problems:
            return 1
        print(f"ok: {len(KEYS)} files match")
        return 0
    write()
    return 0


if __name__ == "__main__":
    sys.exit(main())
