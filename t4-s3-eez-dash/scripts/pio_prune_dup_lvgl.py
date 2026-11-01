"""Pre-build: prune duplicate lvgl copies from the env's libdeps.

LilyGo-AMOLED-Series' library.json declares its own lvgl dependency, which
PlatformIO installs SIDE BY SIDE with ours as `lvgl@<version>` (e.g.
lvgl@8.4.0). `lib_ignore` can't exclude it — both copies are named "lvgl" —
and depending on LDF ordering the 8.x headers can WIN the include path,
producing baffling "lv_xxx not declared" errors against 9.x-only APIs.
Deleting the duplicate before the LDF runs keeps exactly one lvgl: ours.
"""
Import("env")  # noqa: F821  (SCons construction environment)
import pathlib
import shutil

libdeps = pathlib.Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
for dup in libdeps.glob("lvgl@*"):
    print(f"[prune] removing duplicate LVGL: {dup.name}")
    shutil.rmtree(dup, ignore_errors=True)

# LilyGo-AMOLED-Series also SHIPS ITS OWN lv_conf.h inside src/ — with
# LV_CONF_INCLUDE_SIMPLE the include path can resolve to THEIRS instead of
# include/lv_conf.h, silently building all of LVGL against a foreign config
# (snapshot off, different memory settings, 8.x-era assumptions). Delete it
# so exactly one lv_conf exists: ours.
shadow = libdeps / "LilyGo-AMOLED-Series" / "src" / "lv_conf.h"
if shadow.exists():
    print("[prune] removing LilyGo's shadowing lv_conf.h")
    shadow.unlink()
