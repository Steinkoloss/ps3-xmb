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

    def test_original_glare_apply_adds_its_weighted_colour_to_the_preserved_screen(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            Image.new("RGB",(8,8),(40,80,120)).save(root / "colour.png")
            source = {"name":"source","vertex":"lines/lib/moyou/background.vpo",
                "fragment":"lines/lib/moyou/background.fpo","width":8,"height":8,
                "format":"rgba16f","mipmaps":True,"uniforms":{
                    "_MVM":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"_Colour1":[1]*4,"_Colour2":[1]*4},
                "textures":{"0":{"file":"colour.png"}}}
            apply = {"name":"apply","vertex":"lines/lib/moyou/background.vpo",
                "fragment":"lines/lib/glutils/ApplyDisplay.fpo","over":"source","blend":"add",
                "uniforms":{**source["uniforms"],"_GlareWeight":[0.5]},
                "textures":{"0":{"pass":"source","level":1}}}
            path = root / "add.json"
            path.write_text(json.dumps({"schema":"moyou.render-graph.v1","passes":[source,apply]}))
            actual = self.capture(root,["--graph",str(path)],"result.png",4,4)
            self.assertEqual(actual.tobytes(),bytes((60,120,180,0))*16)

    def test_original_dither_subtraction_repeats_rectangle_noise_in_window_coordinates(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            Image.new("RGB",(4,4),(128,128,128)).save(root / "grey.png")
            noise = Image.new("RGB",(2,2))
            noise.putdata([(16,16,16),(32,32,32),(48,48,48),(64,64,64)])
            noise.save(root / "noise.png")
            uniforms = {"_MVM":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"_Colour1":[1]*4,"_Colour2":[1]*4}
            passes = [{"name":"source","vertex":"lines/lib/moyou/background.vpo",
                "fragment":"lines/lib/moyou/background.fpo","uniforms":uniforms,
                "textures":{"0":{"file":"grey.png"}}},
                {"name":"display","vertex":"lines/lib/moyou/background.vpo",
                "fragment":"lines/lib/glutils/ToneApplyDisplay.fpo","over":"source","blend":"add",
                "uniforms":{**uniforms,"_GlareWeight":[0]},
                "textures":{"1":{"pass":"source"},"13":{"file":"noise.png","repeat":True}}}]
            path = root / "noise.json"
            path.write_text(json.dumps({"schema":"moyou.render-graph.v1","passes":passes}))
            actual = self.capture(root,["--graph",str(path)],"result.png",4,4)
            expected = bytes(v for y in range(4) for x in range(4)
                for v in ((128-2*(1+x%2+2*(y%2)),)*3+(0,)))
            self.assertEqual(actual.tobytes(),expected)

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
