"""Applies the Java-side fixes to an apktool-decoded Stardunk 2.3.9 tree.

1. Main.onRequestPermissionsResult no longer re-requests location forever when it is
   denied; the game initializes either way.
2. Renderer's constructor tolerates getLastKnownLocation() throwing SecurityException.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1]) / "smali" / "com" / "gzl" / "stardunk"

main = root / "Main.smali"
s = main.read_text()
sig = ".method public onRequestPermissionsResult(I[Ljava/lang/String;[I)V"
start = s.index(sig)
end = s.index(".end method", start) + len(".end method")
s = s[:start] + sig + """
    .locals 0

    invoke-virtual {p0}, Lcom/gzl/stardunk/Main;->initialize()V

    return-void
.end method""" + s[end:]
main.write_text(s)

renderer = root / "glue" / "Renderer.smali"
s = renderer.read_text()
call = """    invoke-virtual {v2, v4}, Landroid/location/LocationManager;->getLastKnownLocation(Ljava/lang/String;)Landroid/location/Location;

    move-result-object v4

    iput-object v4, p0, Lcom/gzl/stardunk/glue/Renderer;->location:Landroid/location/Location;
"""
if s.count(call) != 1:
    sys.exit("Renderer.smali does not match Stardunk 2.3.9")
s = s.replace(call, "    :sd_try_start\n" + call + """    :sd_try_end
    .catch Ljava/lang/Exception; {:sd_try_start .. :sd_try_end} :sd_catch

    :sd_after
""")
ctor = s.index(".method public constructor <init>(Lcom/gzl/stardunk/Main;Landroid/opengl/GLSurfaceView;)V")
end = s.index(".end method", ctor)
s = s[:end] + """
    :sd_catch
    move-exception v4

    goto :sd_after
""" + s[end:]
renderer.write_text(s)
print("Java patches applied")
