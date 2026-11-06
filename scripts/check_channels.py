"""Validate example registries against the exact SDK used to build them.

Run after a PlatformIO build, or pass --sdk /path/to/TraceCraftCore.
Uses the SDK's own generator to validate names, types and view masks.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--sdk", type=Path)
args = parser.parse_args()
sdk = args.sdk
if sdk is None:
    candidates = sorted(ROOT.glob("*/.pio/libdeps/*/TraceCraftCore"))
    sdk = next((p for p in candidates if json.loads((p / "library.json").read_text())["version"] in {"0.1.13", "0.1.14"}), None)
if sdk is None:
    parser.error("Build an example with SDK 0.1.13/0.1.14 first, or supply --sdk")
spec = importlib.util.spec_from_file_location("sdk_generator", sdk / "tools/gen_channels.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
manifest = json.loads((sdk / "protocol/channel-manifest.json").read_text(encoding="utf-8"))

for project in sorted(ROOT.iterdir()):
    if not (project / "platformio.ini").exists():
        continue
    sources = sorted(project.glob("*.channels.json"))
    if project.name == "t4-s3-eez-dash":
        sources.insert(0, project / "ui/tracecraft-dash.eez-project")
    groups = [[source] for source in sources] if project.name == "channel-monitor" else [sources]
    for group in groups:
        assert group, f"No channel declarations: {project.name}"
        channels, masks = generator.load_board([str(p) for p in group])
        assert len(channels) <= 255, f"8-bit registry overflow: {project.name}"
        print(f"{project.name}: {len(channels)} channels, {len(masks)} views ({', '.join(p.name for p in group)})")

monitor = ROOT / "channel-monitor"
primary = json.loads((monitor / "monitor.channels.json").read_text())
extended = json.loads((monitor / "monitor-extended.channels.json").read_text())
assert len(primary) == len(set(primary)) and len(extended) == len(set(extended)), "Duplicate monitor channel"
assert not set(primary) & set(extended), "Overlapping monitor groups"
local_only = {c["name"] for c in manifest["channels"] if re.search(r"local dashboards only|local.only|not sent over USB", c["desc"], re.I)}
local_only.add("voice_active")  # HID service supplies no voice-capture state.
expected = {c["name"] for c in manifest["channels"]} - local_only
assert set(primary + extended) == expected, f"Monitor coverage drift: {set(primary + extended) ^ expected}"

# A page must subscribe to every channel it reads, not merely declare it in
# another page's view. Otherwise its values silently freeze while it is visible.
p4 = ROOT / "waveshare-p4-deck"
declarations = json.loads((p4 / "waveshare_p4_lcd5_deck.channels.json").read_text())
for view, names in declarations["views"].items():
    folder = {"system": "sys"}.get(view, view)
    source = (p4 / f"src/ui_p4{folder}/p4{folder}.cpp").read_text()
    used = {n.lower() for n in re.findall(r"TC_CH_([A-Z][A-Z0-9_]*[A-Z0-9])\b", source)}
    missing = used - set(names) - set(declarations.get("always", []))
    assert not missing, f"P4 {view} reads unsubscribed channels: {sorted(missing)}"

macropad = ROOT / "macropad-dash"
views = json.loads((macropad / "macropad.channels.json").read_text())["views"]
pages = re.findall(r'\{ "([^"]+)",\s*TC_VIEW_(\w+), \{(.*?)\n  \} \},',
                   (macropad / "src/main.cpp").read_text(), re.S)
assert {view.lower() for _, view, _ in pages} == set(views), "MacroPad page/view mismatch"
for title, view, body in pages:
    used = {n.lower() for n in re.findall(r"TC_CH_([A-Z0-9_]+)", body)}
    assert used == set(views[view.lower()]), f"MacroPad {title} cells and subscriptions differ"
print(f"PASS: SDK names/types/views and all {len(expected)} USB catalog channels covered")
