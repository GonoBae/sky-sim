"""Print SkySim material expression inputs for command-line diagnostics."""

import unreal


material = unreal.EditorAssetLibrary.load_asset("/Game/SkySim/M_SkySimVolume")
if material is None:
    raise RuntimeError("M_SkySimVolume is missing")

for property_name in (
    "material_domain",
    "blend_mode",
    "shading_model",
    "used_with_heterogeneous_volumes",
):
    try:
        unreal.log(
            f"SKYSIM_MATERIAL {property_name}="
            f"{material.get_editor_property(property_name)}"
        )
    except Exception as error:
        unreal.log(f"SKYSIM_MATERIAL {property_name}=<unavailable:{error}>")

get_objects_with_outer = getattr(unreal, "get_objects_with_outer", None)
if get_objects_with_outer is None:
    unreal.log_warning(
        "SKYSIM_GRAPH enumeration is unavailable in this Unreal Python build"
    )
    material_objects = []
else:
    material_objects = get_objects_with_outer(material, include_nested_objects=True)

for obj in material_objects:
    if not isinstance(obj, unreal.MaterialExpression):
        continue
    names = unreal.MaterialEditingLibrary.get_material_expression_input_names(obj)
    inputs = unreal.MaterialEditingLibrary.get_inputs_for_material_expression(material, obj)
    unreal.log(
        "SKYSIM_GRAPH "
        + obj.get_name()
        + " class="
        + obj.get_class().get_name()
        + " input_names="
        + repr(list(names))
        + " connected="
        + repr([item.get_name() for item in inputs])
    )
