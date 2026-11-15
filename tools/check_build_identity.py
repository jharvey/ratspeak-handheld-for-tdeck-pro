"""Check included identity and bind each PlatformIO application to its source."""
from pathlib import Path
import sys

Import("env")
project = Path(env.subst("$PROJECT_DIR")).resolve()
root = project if (project / "tools/release_identity.json").is_file() else project.parent
sys.path.insert(0, str(root / "tools"))
from release_identity import check_local, source_identity

check_local(root)

# --- DEV: keep defines stable so PlatformIO does incremental builds ---
# Comment these two lines back in for release builds if you need real revision.
env.Append(CPPDEFINES=[
    ("RATSPEAK_BUILD_REVISION", "dev"),
    ("RATSPEAK_BUILD_DIRTY", 0),
])
# revision, dirty = source_identity(root)
# env.Append(CPPDEFINES=[("RATSPEAK_BUILD_REVISION", revision),
#                        ("RATSPEAK_BUILD_DIRTY", int(dirty))])