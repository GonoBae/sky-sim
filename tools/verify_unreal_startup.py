"""Load the Unreal project long enough to run startup validation modules."""

import unreal


material = unreal.EditorAssetLibrary.load_asset("/Game/SkySim/M_SkySimVolume")
if material is None:
    raise RuntimeError("SkySim volume material was not generated during editor startup")

unreal.log("SkySim: startup verification loaded M_SkySimVolume successfully")
