"""Real-driver checks for inter-pass orientation and the connected native scene."""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(os.environ.get("MOYOU_PREVIEW") and os.environ.get("MOYOU_NATIVE_SHADERS"),
                     "Set the native renderer and current translated shader directory")
class PipelineTests(unittest.TestCase):
    def capture(self, root, arguments, name, width=2, height=2):
        from PIL import Image
        result = subprocess.run([str(Path(os.environ["MOYOU_PREVIEW"]).resolve()),
            "--shaders",str(Path(os.environ["MOYOU_NATIVE_SHADERS"]).resolve()),*arguments,
            "--width",str(width),"--height",str(height),"--output",str(root / name)],
            capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        return Image.open(root / name).convert("RGBA")

    def test_original_program_framebuffer_roundtrip_keeps_top_bottom_orientation(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = Image.new("RGBA",(2,2))
            original.putdata([(255,0,0,255),(0,255,0,255),(0,0,255,255),(255,255,0,255)])
            original.save(root / "colours.png")
            def copy(name, binding):
                return {"name":name,"vertex":"lines/lib/moyou/background.vpo",
                    "fragment":"lines/lib/moyou/background.fpo", "uniforms":{
                        "_MVM":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"_Colour1":[1]*4,"_Colour2":[1]*4},
                    "textures":{"0":binding}}
            for count in (1,2):
                passes = [copy("source",{"file":"colours.png"})]
                if count == 2: passes.append(copy("copy",{"pass":"source"}))
                path = root / f"passes-{count}.json"
                path.write_text(json.dumps({"schema":"moyou.render-graph.v1","passes":passes}))
                actual = self.capture(root,["--graph",str(path)],f"result-{count}.png")
                self.assertEqual(actual.convert("RGB").tobytes(),original.convert("RGB").tobytes())
                self.assertEqual(actual.getchannel("A").getextrema(),(0,0))

    @unittest.skipUnless(os.environ.get("MOYOU_CPU_CORE") and os.environ.get("MOYOU_NATIVE_BASELINE") and os.environ.get("MOYOU_REFERENCE"),
                         "Set the original CPU/SPU and resource inputs")
    def test_connected_original_exposure_preserves_blue_month_map_channels(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = self.capture(root,["--reference",str(Path(os.environ["MOYOU_REFERENCE"]).resolve()),
                "--cpu",str(Path(os.environ["MOYOU_CPU_CORE"]).resolve()),
                "--spline",str(Path(os.environ["MOYOU_NATIVE_BASELINE"]).resolve() / "spline/original-ls.bin"),
                "--preset","night","--frames","2"],"native.png",320,180)
            red,green,blue,_ = image.getpixel((160,10))
            self.assertGreater(green,red * 2)
            self.assertGreater(blue,red * 2)
            self.assertGreater(max(green,blue),5)


if __name__ == "__main__":
    unittest.main()
