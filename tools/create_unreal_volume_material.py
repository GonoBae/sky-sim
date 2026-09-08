"""Create the runtime SkySim heterogeneous-volume material.

Run this with Unreal Editor, not the system Python interpreter::

    UnrealEditor-Cmd.exe <project>.uproject -unattended -nop4 \
        -ExecutePythonScript=<repo>/tools/create_unreal_volume_material.py

The script is intentionally idempotent.  If the material already exists, its
expression graph is rebuilt in place so references to the asset remain valid.
"""

import unreal


ASSET_DIRECTORY = "/Game/SkySim"
ASSET_NAME = "M_SkySimVolume"
ASSET_PATH = f"{ASSET_DIRECTORY}/{ASSET_NAME}"
DEFAULT_VOLUME_TEXTURE = "/Engine/EngineResources/DefaultVolumeTexture"
EROSION_VOLUME_TEXTURE = "/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32"
GENERATED_VERSION = "15"


def _enum_member(enum_type, *candidate_names):
    """Resolve a reflected enum member while tolerating UE naming changes."""
    for name in candidate_names:
        if hasattr(enum_type, name):
            return getattr(enum_type, name)
    raise RuntimeError(
        f"None of {candidate_names!r} exists on Unreal enum {enum_type!r}"
    )


def _set_editor_property(obj, name, value):
    try:
        obj.set_editor_property(name, value)
    except Exception as exc:
        raise RuntimeError(
            f"Unable to set {obj.get_class().get_name()}.{name}: {exc}"
        ) from exc


def _create_expression(material, expression_class, x, y):
    expression = unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, x, y
    )
    if expression is None:
        raise RuntimeError(
            f"Failed to create material expression {expression_class.__name__}"
        )
    return expression


def _connect(from_expression, from_output, to_expression, to_input):
    editing_library = unreal.MaterialEditingLibrary
    if editing_library.connect_material_expressions(
        from_expression, from_output, to_expression, to_input
    ):
        return

    # Vector/colour parameters expose their primary RGB result as the default
    # output in UE 5.6's Python binding, even though the material editor labels
    # that pin RGB.  Falling back is safe for these primary-vector requests.
    reflected_output_name = from_output
    if from_output in ("RGB", "RGBA") and editing_library.connect_material_expressions(
        from_expression, "", to_expression, to_input
    ):
        unreal.log(
            "SkySim: connected reflected default output "
            f"{from_expression.get_name()} (requested {from_output})"
        )
        return
    if from_output in ("RGB", "RGBA"):
        reflected_output_name = ""

    # UE 5.6 exposes the sole input of a few material nodes (notably
    # TransformPosition and ComponentMask) as the reflected name ``None``.
    # The visual editor labels the same pin "Input", so tolerate that engine
    # binding quirk only when there is exactly one unambiguous destination pin.
    input_names = list(
        editing_library.get_material_expression_input_names(to_expression)
    )
    reflected_input_name = None
    if len(input_names) == 1:
        reflected_input_name = str(input_names[0])
    elif to_input == "Input" and any(str(name) == "None" for name in input_names):
        # Some multi-input nodes (for example Clamp) also expose their
        # unlabeled main value pin as ``None`` while keeping Min/Max named.
        reflected_input_name = "None"
    elif to_input == "Coordinates" and any(
        str(name) == "UVs" for name in input_names
    ):
        # TextureSampleParameterVolume renamed Coordinates to UVs in the 5.6
        # reflected API.  It is the same coordinate input shown in the graph.
        reflected_input_name = "UVs"

    if reflected_input_name is not None:
        if reflected_input_name != to_input and editing_library.connect_material_expressions(
            from_expression,
            reflected_output_name,
            to_expression,
            reflected_input_name,
        ):
            unreal.log(
                "SkySim: connected reflected sole input "
                f"{to_expression.get_name()}.{reflected_input_name} "
                f"(requested {to_input})"
            )
            return

    raise RuntimeError(
        "Failed to connect "
        f"{from_expression.get_name()}.{from_output or '<default>'} to "
        f"{to_expression.get_name()}.{to_input}; reflected inputs={input_names!r}"
    )


def _connect_property(expression, output_name, material_property):
    editing_library = unreal.MaterialEditingLibrary
    if editing_library.connect_material_property(
        expression, output_name, material_property
    ):
        return
    if output_name in ("RGB", "RGBA") and editing_library.connect_material_property(
        expression, "", material_property
    ):
        unreal.log(
            "SkySim: connected reflected default material output "
            f"{expression.get_name()} (requested {output_name})"
        )
        return
    raise RuntimeError(
        f"Failed to connect {expression.get_name()} to {material_property}"
    )


def _load_or_create_material():
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        material = unreal.EditorAssetLibrary.load_asset(ASSET_PATH)
        if not isinstance(material, unreal.Material):
            raise RuntimeError(
                f"{ASSET_PATH} already exists but is not a Material asset"
            )
        unreal.log(f"SkySim: rebuilding existing material {ASSET_PATH}")
        return material

    if not unreal.EditorAssetLibrary.does_directory_exist(ASSET_DIRECTORY):
        if not unreal.EditorAssetLibrary.make_directory(ASSET_DIRECTORY):
            raise RuntimeError(f"Unable to create {ASSET_DIRECTORY}")

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ASSET_NAME,
        ASSET_DIRECTORY,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if material is None:
        raise RuntimeError(f"Unable to create {ASSET_PATH}")
    unreal.log(f"SkySim: created material {ASSET_PATH}")
    return material


def _create_xy_offset(material, parameter_name, x, y):
    parameter = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, x, y
    )
    _set_editor_property(parameter, "parameter_name", unreal.Name(parameter_name))
    _set_editor_property(
        parameter, "default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0)
    )
    xy = _create_expression(
        material, unreal.MaterialExpressionComponentMask, x + 180, y
    )
    for channel in ("r", "g", "b", "a"):
        _set_editor_property(xy, channel, channel in ("r", "g"))
    _connect(parameter, "RGB", xy, "Input")
    return xy


def _create_offset_coordinates(material, base_xy, offset_xy, density_z, x, y):
    horizontal = _create_expression(
        material, unreal.MaterialExpressionAdd, x, y
    )
    coordinates = _create_expression(
        material, unreal.MaterialExpressionAppendVector, x + 180, y
    )
    _connect(base_xy, "", horizontal, "A")
    _connect(offset_xy, "", horizontal, "B")
    _connect(horizontal, "", coordinates, "A")
    _connect(density_z, "", coordinates, "B")
    return coordinates


def _create_density_sample(material, parameter_name, texture, coordinates, x, y):
    sample = _create_expression(
        material, unreal.MaterialExpressionTextureSampleParameterVolume, x, y
    )
    _set_editor_property(sample, "parameter_name", unreal.Name(parameter_name))
    _set_editor_property(sample, "texture", texture)
    _set_editor_property(
        sample,
        "mip_value_mode",
        _enum_member(unreal.TextureMipValueMode, "TMVM_MIP_LEVEL", "MIP_LEVEL"),
    )
    _set_editor_property(sample, "const_mip_value", 0)
    _connect(coordinates, "", sample, "Coordinates")
    return sample


def build_material():
    material = _load_or_create_material()
    material.modify()

    unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
    _set_editor_property(material, "material_domain", unreal.MaterialDomain.MD_VOLUME)
    _set_editor_property(material, "blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    _set_editor_property(material, "use_material_attributes", False)
    _set_editor_property(material, "used_with_heterogeneous_volumes", True)

    # HeterogeneousVolumeComponent uses local coordinates in voxel units when
    # bPivotAtCentroid is false.  The runtime MID supplies 1 / (Nx, Ny, Nz).
    world_position = _create_expression(
        material, unreal.MaterialExpressionWorldPosition, -1200, -260
    )
    world_to_local = _create_expression(
        material, unreal.MaterialExpressionTransformPosition, -1000, -260
    )
    position_space = unreal.MaterialPositionTransformSource
    _set_editor_property(
        world_to_local,
        "transform_source_type",
        _enum_member(
            position_space, "TRANSFORMPOSSOURCE_WORLD", "WORLD"
        ),
    )
    _set_editor_property(
        world_to_local,
        "transform_type",
        _enum_member(
            position_space, "TRANSFORMPOSSOURCE_LOCAL", "LOCAL"
        ),
    )

    inverse_resolution = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, -1000, -80
    )
    _set_editor_property(
        inverse_resolution, "parameter_name", unreal.Name("InvVolumeResolution")
    )
    _set_editor_property(
        inverse_resolution,
        "default_value",
        unreal.LinearColor(1.0 / 64.0, 1.0 / 64.0, 1.0 / 64.0, 0.0),
    )

    # Expand one periodic server tile over a much wider HV component while
    # keeping Z inside the physical texture's first/last texel centres.
    # HorizontalTileCount is set by SkySimSystem from the authored world range
    # / simulation range ratio.
    normalized_local_position = _create_expression(
        material, unreal.MaterialExpressionMultiply, -800, -220
    )
    horizontal_coordinates = _create_expression(
        material, unreal.MaterialExpressionComponentMask, -600, -360
    )
    _set_editor_property(horizontal_coordinates, "r", True)
    _set_editor_property(horizontal_coordinates, "g", True)
    _set_editor_property(horizontal_coordinates, "b", False)
    _set_editor_property(horizontal_coordinates, "a", False)
    vertical_coordinate = _create_expression(
        material, unreal.MaterialExpressionComponentMask, -600, -220
    )
    _set_editor_property(vertical_coordinate, "r", False)
    _set_editor_property(vertical_coordinate, "g", False)
    _set_editor_property(vertical_coordinate, "b", True)
    _set_editor_property(vertical_coordinate, "a", False)
    horizontal_tile_count = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, -600, -500
    )
    _set_editor_property(
        horizontal_tile_count, "parameter_name", unreal.Name("HorizontalTileCount")
    )
    _set_editor_property(horizontal_tile_count, "default_value", 1.0)
    continuous_horizontal_coordinates = _create_expression(
        material, unreal.MaterialExpressionMultiply, -380, -380
    )
    inverse_density_texture_resolution = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, -380, -220
    )
    _set_editor_property(
        inverse_density_texture_resolution,
        "parameter_name",
        unreal.Name("InvDensityTextureResolution"),
    )
    _set_editor_property(
        inverse_density_texture_resolution,
        "default_value",
        unreal.LinearColor(1.0 / 64.0, 1.0 / 64.0, 1.0 / 64.0, 0.0),
    )
    inverse_density_texture_resolution_z = _create_expression(
        material, unreal.MaterialExpressionComponentMask, -160, -220
    )
    _set_editor_property(inverse_density_texture_resolution_z, "r", False)
    _set_editor_property(inverse_density_texture_resolution_z, "g", False)
    _set_editor_property(inverse_density_texture_resolution_z, "b", True)
    _set_editor_property(inverse_density_texture_resolution_z, "a", False)
    half_value = _create_expression(
        material, unreal.MaterialExpressionConstant, -160, -100
    )
    _set_editor_property(half_value, "r", 0.5)
    half_density_texel_z = _create_expression(
        material, unreal.MaterialExpressionMultiply, 60, -180
    )
    maximum_density_z = _create_expression(
        material, unreal.MaterialExpressionOneMinus, 280, -180
    )
    clamped_density_z = _create_expression(
        material, unreal.MaterialExpressionClamp, 500, -220
    )
    tiled_volume_coordinates = _create_expression(
        material, unreal.MaterialExpressionAppendVector, 720, -340
    )
    continuous_volume_coordinates = _create_expression(
        material, unreal.MaterialExpressionAppendVector, -160, -200
    )
    density_volume = _create_expression(
        material, unreal.MaterialExpressionTextureSampleParameterVolume, 280, -300
    )
    _set_editor_property(
        density_volume, "parameter_name", unreal.Name("DensityVolume")
    )
    default_texture = unreal.EditorAssetLibrary.load_asset(DEFAULT_VOLUME_TEXTURE)
    if default_texture is None:
        raise RuntimeError(f"Unable to load {DEFAULT_VOLUME_TEXTURE}")
    _set_editor_property(density_volume, "texture", default_texture)

    # Explicit mip 0 avoids implicit screen-space derivatives in the volume
    # compute/ray-march shaders.  The transient density texture has one mip.
    mip_modes = unreal.TextureMipValueMode
    _set_editor_property(
        density_volume,
        "mip_value_mode",
        _enum_member(mip_modes, "TMVM_MIP_LEVEL", "MIP_LEVEL"),
    )
    _set_editor_property(density_volume, "const_mip_value", 0)

    _connect(world_position, "", world_to_local, "Input")
    _connect(world_to_local, "", normalized_local_position, "A")
    _connect(inverse_resolution, "RGB", normalized_local_position, "B")
    _connect(normalized_local_position, "", horizontal_coordinates, "Input")
    _connect(normalized_local_position, "", vertical_coordinate, "Input")
    _connect(horizontal_coordinates, "", continuous_horizontal_coordinates, "A")
    _connect(horizontal_tile_count, "", continuous_horizontal_coordinates, "B")
    _connect(continuous_horizontal_coordinates, "", continuous_volume_coordinates, "A")
    _connect(vertical_coordinate, "", continuous_volume_coordinates, "B")
    _connect(
        inverse_density_texture_resolution,
        "RGB",
        inverse_density_texture_resolution_z,
        "Input",
    )
    _connect(inverse_density_texture_resolution_z, "", half_density_texel_z, "A")
    _connect(half_value, "", half_density_texel_z, "B")
    _connect(half_density_texel_z, "", maximum_density_z, "Input")
    _connect(vertical_coordinate, "", clamped_density_z, "Input")
    _connect(half_density_texel_z, "", clamped_density_z, "Min")
    _connect(maximum_density_z, "", clamped_density_z, "Max")

    erosion_texture = unreal.EditorAssetLibrary.load_asset(EROSION_VOLUME_TEXTURE)
    if erosion_texture is None:
        unreal.log_warning(
            "SkySim: engine erosion volume is unavailable; wide-range variation disabled"
        )
        erosion_texture = default_texture
        has_erosion_texture = False
    else:
        has_erosion_texture = True

    detail_noise_tiling = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 0, 20
    )
    _set_editor_property(
        detail_noise_tiling, "parameter_name", unreal.Name("DetailNoiseTiling")
    )
    _set_editor_property(
        detail_noise_tiling,
        "default_value",
        unreal.LinearColor(7.13, 5.77, 4.31, 0.0),
    )
    detail_phase_offset = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 0, 120
    )
    _set_editor_property(
        detail_phase_offset, "parameter_name", unreal.Name("DetailPhaseOffset")
    )
    _set_editor_property(
        detail_phase_offset,
        "default_value",
        unreal.LinearColor(0.37, 0.61, 0.17, 0.0),
    )
    scaled_detail_noise_coordinates = _create_expression(
        material, unreal.MaterialExpressionMultiply, 220, 70
    )
    detail_noise_coordinates = _create_expression(
        material, unreal.MaterialExpressionAdd, 440, 70
    )
    detail_noise = _create_expression(
        material, unreal.MaterialExpressionTextureSampleParameterVolume, 660, 70
    )
    _set_editor_property(
        detail_noise, "parameter_name", unreal.Name("DetailNoiseVolume")
    )
    _set_editor_property(detail_noise, "texture", erosion_texture)
    _set_editor_property(
        detail_noise,
        "mip_value_mode",
        _enum_member(mip_modes, "TMVM_MIP_LEVEL", "MIP_LEVEL"),
    )
    # At the 312.5 m wide-world voxel pitch, mip 0 aliases the 32^3 erosion
    # volume into the diagonal comb visible in top-down views. Mip 2 keeps edge
    # breakup but removes the sub-voxel moire.
    _set_editor_property(detail_noise, "const_mip_value", 2)
    if has_erosion_texture:
        _set_editor_property(
            detail_noise,
            "sampler_type",
            _enum_member(
                unreal.MaterialSamplerType, "SAMPLERTYPE_MASKS", "MASKS"
            ),
        )
    inverse_detail_noise = _create_expression(
        material, unreal.MaterialExpressionOneMinus, 880, 50
    )
    detail_erosion_strength = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 880, 150
    )
    _set_editor_property(
        detail_erosion_strength,
        "parameter_name",
        unreal.Name("DetailErosionStrength"),
    )
    _set_editor_property(
        detail_erosion_strength, "default_value", 0.07 if has_erosion_texture else 0.0
    )
    detail_erosion = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1100, 70
    )

    # A world-scale weather map crosses tile boundaries, warps density
    # coordinates, and creates connected cloudy/clear regions. Unlike detail
    # erosion, it is evaluated in 0..1 full-volume coordinates so its scale is
    # independent of the number of repeated 20 km simulation tiles.
    weather_map_offset = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 0, 300
    )
    _set_editor_property(
        weather_map_offset, "parameter_name", unreal.Name("WeatherMapOffset")
    )
    _set_editor_property(
        weather_map_offset,
        "default_value",
        unreal.LinearColor(0.0, 0.0, 0.0, 0.0),
    )
    weather_map_position = _create_expression(
        material, unreal.MaterialExpressionAdd, 440, 300
    )
    macro_variation_tiling = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 220, 420
    )
    _set_editor_property(
        macro_variation_tiling,
        "parameter_name",
        unreal.Name("MacroVariationTiling"),
    )
    _set_editor_property(
        macro_variation_tiling,
        "default_value",
        unreal.LinearColor(2.15, 1.65, 0.70, 0.0),
    )
    macro_noise_coordinates = _create_expression(
        material, unreal.MaterialExpressionMultiply, 220, 300
    )
    macro_noise = _create_expression(
        material, unreal.MaterialExpressionTextureSampleParameterVolume, 660, 300
    )
    _set_editor_property(
        macro_noise, "parameter_name", unreal.Name("MacroVariationVolume")
    )
    _set_editor_property(macro_noise, "texture", erosion_texture)
    _set_editor_property(
        macro_noise,
        "mip_value_mode",
        _enum_member(mip_modes, "TMVM_MIP_LEVEL", "MIP_LEVEL"),
    )
    _set_editor_property(macro_noise, "const_mip_value", 0)
    if has_erosion_texture:
        _set_editor_property(
            macro_noise,
            "sampler_type",
            _enum_member(
                unreal.MaterialSamplerType, "SAMPLERTYPE_MASKS", "MASKS"
            ),
        )
    macro_noise_rg = _create_expression(
        material, unreal.MaterialExpressionComponentMask, 880, 220
    )
    _set_editor_property(macro_noise_rg, "r", True)
    _set_editor_property(macro_noise_rg, "g", True)
    _set_editor_property(macro_noise_rg, "b", False)
    _set_editor_property(macro_noise_rg, "a", False)
    half_vector = _create_expression(
        material, unreal.MaterialExpressionConstant2Vector, 880, 120
    )
    _set_editor_property(half_vector, "r", 0.5)
    _set_editor_property(half_vector, "g", 0.5)
    signed_macro_noise = _create_expression(
        material, unreal.MaterialExpressionSubtract, 1100, 190
    )
    density_coordinate_warp_strength = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1100, 30
    )
    _set_editor_property(
        density_coordinate_warp_strength,
        "parameter_name",
        unreal.Name("DensityCoordinateWarpStrength"),
    )
    _set_editor_property(
        density_coordinate_warp_strength,
        "default_value",
        0.16 if has_erosion_texture else 0.0,
    )
    density_coordinate_warp = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1320, 190
    )
    warped_continuous_horizontal_coordinates = _create_expression(
        material, unreal.MaterialExpressionAdd, 1540, 190
    )
    inverse_macro_noise = _create_expression(
        material, unreal.MaterialExpressionOneMinus, 880, 380
    )
    macro_variation_strength = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 880, 480
    )
    _set_editor_property(
        macro_variation_strength,
        "parameter_name",
        unreal.Name("MacroVariationStrength"),
    )
    _set_editor_property(
        macro_variation_strength, "default_value", 0.13 if has_erosion_texture else 0.0
    )
    macro_erosion = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1100, 300
    )
    combined_erosion = _create_expression(
        material, unreal.MaterialExpressionAdd, 1320, 120
    )
    detailed_density = _create_expression(
        material, unreal.MaterialExpressionSubtract, 1320, -80
    )
    shaped_density = _create_expression(
        material, unreal.MaterialExpressionSaturate, 1520, -100
    )

    # A second continuously transformed lookup breaks the exact 20 km clone
    # period without freezing or baking the live server density. The broad
    # weather map blends between both realizations, so there are no per-tile
    # hash boundaries or hard seams.
    secondary_pattern_scale = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 1320, 560
    )
    _set_editor_property(
        secondary_pattern_scale,
        "parameter_name",
        unreal.Name("SecondaryPatternScale"),
    )
    _set_editor_property(
        secondary_pattern_scale,
        "default_value",
        unreal.LinearColor(0.83, 1.137, 1.0, 0.0),
    )
    secondary_pattern_scale_xy = _create_expression(
        material, unreal.MaterialExpressionComponentMask, 1540, 560
    )
    _set_editor_property(secondary_pattern_scale_xy, "r", True)
    _set_editor_property(secondary_pattern_scale_xy, "g", True)
    _set_editor_property(secondary_pattern_scale_xy, "b", False)
    _set_editor_property(secondary_pattern_scale_xy, "a", False)
    secondary_pattern_offset = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 1320, 680
    )
    _set_editor_property(
        secondary_pattern_offset,
        "parameter_name",
        unreal.Name("SecondaryPatternOffset"),
    )
    _set_editor_property(
        secondary_pattern_offset,
        "default_value",
        unreal.LinearColor(0.37, 0.61, 0.0, 0.0),
    )
    secondary_pattern_offset_xy = _create_expression(
        material, unreal.MaterialExpressionComponentMask, 1540, 680
    )
    _set_editor_property(secondary_pattern_offset_xy, "r", True)
    _set_editor_property(secondary_pattern_offset_xy, "g", True)
    _set_editor_property(secondary_pattern_offset_xy, "b", False)
    _set_editor_property(secondary_pattern_offset_xy, "a", False)
    secondary_scaled_horizontal_coordinates = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1760, 560
    )
    secondary_horizontal_coordinates = _create_expression(
        material, unreal.MaterialExpressionAdd, 1980, 560
    )
    secondary_volume_coordinates = _create_expression(
        material, unreal.MaterialExpressionAppendVector, 2200, 520
    )
    secondary_density_volume = _create_expression(
        material, unreal.MaterialExpressionTextureSampleParameterVolume, 2420, 500
    )
    _set_editor_property(
        secondary_density_volume,
        "parameter_name",
        unreal.Name("DensityVolumeSecondary"),
    )
    _set_editor_property(secondary_density_volume, "texture", default_texture)
    _set_editor_property(
        secondary_density_volume,
        "mip_value_mode",
        _enum_member(mip_modes, "TMVM_MIP_LEVEL", "MIP_LEVEL"),
    )
    _set_editor_property(secondary_density_volume, "const_mip_value", 0)
    secondary_pattern_blend_strength = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1760, 760
    )
    _set_editor_property(
        secondary_pattern_blend_strength,
        "parameter_name",
        unreal.Name("SecondaryPatternBlendStrength"),
    )
    _set_editor_property(
        secondary_pattern_blend_strength, "default_value", 0.72
    )
    secondary_pattern_blend = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1980, 760
    )
    blended_density = _create_expression(
        material, unreal.MaterialExpressionLinearInterpolate, 2640, 420
    )

    current_density_offset_xy = _create_xy_offset(
        material, "CurrentDensityOffset", 1540, -740
    )
    previous_density_offset_xy = _create_xy_offset(
        material, "PreviousDensityOffset", 1540, -880
    )
    secondary_motion_offset_xy = _create_xy_offset(
        material, "SecondaryMotionOffset", 1540, 900
    )
    moving_secondary_coordinates = _create_expression(
        material, unreal.MaterialExpressionAdd, 2200, 700
    )
    _connect(secondary_horizontal_coordinates, "", moving_secondary_coordinates, "A")
    _connect(secondary_motion_offset_xy, "", moving_secondary_coordinates, "B")

    # Offsets are final density UVs: the secondary pattern scale must not
    # rescale endpoint advection or its separately accumulated motion phase.
    current_primary_xy = _create_expression(
        material, unreal.MaterialExpressionAdd, 1760, -580
    )
    current_secondary_xy = _create_expression(
        material, unreal.MaterialExpressionAdd, 2420, 700
    )
    _connect(warped_continuous_horizontal_coordinates, "", current_primary_xy, "A")
    _connect(current_density_offset_xy, "", current_primary_xy, "B")
    _connect(moving_secondary_coordinates, "", current_secondary_xy, "A")
    _connect(current_density_offset_xy, "", current_secondary_xy, "B")
    previous_primary_coordinates = _create_offset_coordinates(
        material, warped_continuous_horizontal_coordinates,
        previous_density_offset_xy, clamped_density_z, 1760, -900
    )
    previous_secondary_coordinates = _create_offset_coordinates(
        material, moving_secondary_coordinates,
        previous_density_offset_xy, clamped_density_z, 2420, 920
    )
    previous_density_volume = _create_density_sample(
        material, "DensityVolumePrevious", default_texture,
        previous_primary_coordinates, 2200, -900
    )
    previous_secondary_density_volume = _create_density_sample(
        material, "DensityVolumeSecondaryPrevious", default_texture,
        previous_secondary_coordinates, 2860, 920
    )
    previous_blended_density = _create_expression(
        material, unreal.MaterialExpressionLinearInterpolate, 3080, 700
    )
    density_frame_blend = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 3080, 1000
    )
    _set_editor_property(
        density_frame_blend, "parameter_name", unreal.Name("DensityFrameBlend")
    )
    _set_editor_property(density_frame_blend, "default_value", 1.0)
    temporal_density = _create_expression(
        material, unreal.MaterialExpressionLinearInterpolate, 3300, 420
    )

    _connect(continuous_volume_coordinates, "", scaled_detail_noise_coordinates, "A")
    _connect(detail_noise_tiling, "RGB", scaled_detail_noise_coordinates, "B")
    _connect(scaled_detail_noise_coordinates, "", detail_noise_coordinates, "A")
    _connect(detail_phase_offset, "RGB", detail_noise_coordinates, "B")
    _connect(detail_noise_coordinates, "", detail_noise, "Coordinates")
    _connect(detail_noise, "R", inverse_detail_noise, "Input")
    _connect(inverse_detail_noise, "", detail_erosion, "A")
    _connect(detail_erosion_strength, "", detail_erosion, "B")
    # WeatherMapOffset is a phase in noise space, just like DetailPhaseOffset.
    _connect(normalized_local_position, "", macro_noise_coordinates, "A")
    _connect(macro_variation_tiling, "RGB", macro_noise_coordinates, "B")
    _connect(macro_noise_coordinates, "", weather_map_position, "A")
    _connect(weather_map_offset, "RGB", weather_map_position, "B")
    _connect(weather_map_position, "", macro_noise, "Coordinates")
    _connect(macro_noise, "RGB", macro_noise_rg, "Input")
    _connect(macro_noise_rg, "", signed_macro_noise, "A")
    _connect(half_vector, "", signed_macro_noise, "B")
    _connect(signed_macro_noise, "", density_coordinate_warp, "A")
    _connect(density_coordinate_warp_strength, "", density_coordinate_warp, "B")
    _connect(
        continuous_horizontal_coordinates,
        "",
        warped_continuous_horizontal_coordinates,
        "A",
    )
    _connect(
        density_coordinate_warp,
        "",
        warped_continuous_horizontal_coordinates,
        "B",
    )
    # Density XY intentionally remains continuous. The runtime volume texture
    # uses TA_Wrap, so filtering crosses every 20 km tile boundary instead of
    # jumping between clamped edge texels after a Frac operation. Only Z is
    # explicitly restricted to the first/last texel centres.
    _connect(
        current_primary_xy,
        "",
        tiled_volume_coordinates,
        "A",
    )
    _connect(clamped_density_z, "", tiled_volume_coordinates, "B")
    _connect(tiled_volume_coordinates, "", density_volume, "Coordinates")
    _connect(secondary_pattern_scale, "RGB", secondary_pattern_scale_xy, "Input")
    _connect(secondary_pattern_offset, "RGB", secondary_pattern_offset_xy, "Input")
    _connect(
        warped_continuous_horizontal_coordinates,
        "",
        secondary_scaled_horizontal_coordinates,
        "A",
    )
    _connect(
        secondary_pattern_scale_xy,
        "",
        secondary_scaled_horizontal_coordinates,
        "B",
    )
    _connect(
        secondary_scaled_horizontal_coordinates,
        "",
        secondary_horizontal_coordinates,
        "A",
    )
    _connect(
        secondary_pattern_offset_xy,
        "",
        secondary_horizontal_coordinates,
        "B",
    )
    _connect(current_secondary_xy, "", secondary_volume_coordinates, "A")
    _connect(clamped_density_z, "", secondary_volume_coordinates, "B")
    _connect(secondary_volume_coordinates, "", secondary_density_volume, "Coordinates")
    _connect(macro_noise, "G", secondary_pattern_blend, "A")
    _connect(
        secondary_pattern_blend_strength,
        "",
        secondary_pattern_blend,
        "B",
    )
    _connect(density_volume, "R", blended_density, "A")
    _connect(secondary_density_volume, "R", blended_density, "B")
    _connect(secondary_pattern_blend, "", blended_density, "Alpha")
    _connect(previous_density_volume, "R", previous_blended_density, "A")
    _connect(previous_secondary_density_volume, "R", previous_blended_density, "B")
    _connect(secondary_pattern_blend, "", previous_blended_density, "Alpha")
    _connect(previous_blended_density, "", temporal_density, "A")
    _connect(blended_density, "", temporal_density, "B")
    _connect(density_frame_blend, "", temporal_density, "Alpha")
    _connect(macro_noise, "R", inverse_macro_noise, "Input")
    _connect(inverse_macro_noise, "", macro_erosion, "A")
    _connect(macro_variation_strength, "", macro_erosion, "B")
    _connect(detail_erosion, "", combined_erosion, "A")
    _connect(macro_erosion, "", combined_erosion, "B")
    _connect(temporal_density, "", detailed_density, "A")
    _connect(combined_erosion, "", detailed_density, "B")
    _connect(detailed_density, "", shaped_density, "Input")

    density_value_scale = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1520, 20
    )
    _set_editor_property(
        density_value_scale, "parameter_name", unreal.Name("DensityValueScale")
    )
    _set_editor_property(density_value_scale, "default_value", 1.0)
    scaled_density = _create_expression(
        material, unreal.MaterialExpressionMultiply, 1740, -100
    )
    _connect(shaped_density, "", scaled_density, "A")
    _connect(density_value_scale, "", scaled_density, "B")

    density_value_bias = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1740, 20
    )
    _set_editor_property(
        density_value_bias, "parameter_name", unreal.Name("DensityValueBias")
    )
    _set_editor_property(density_value_bias, "default_value", 0.0)
    physical_density = _create_expression(
        material, unreal.MaterialExpressionAdd, 1960, -80
    )
    _connect(scaled_density, "", physical_density, "A")
    _connect(density_value_bias, "", physical_density, "B")

    extinction_scale = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1960, -240
    )
    _set_editor_property(
        extinction_scale, "parameter_name", unreal.Name("ExtinctionScale")
    )
    _set_editor_property(extinction_scale, "default_value", 0.1)
    extinction = _create_expression(
        material, unreal.MaterialExpressionMultiply, 2180, -180
    )
    _connect(physical_density, "", extinction, "A")
    _connect(extinction_scale, "", extinction, "B")

    albedo = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 2180, 0
    )
    _set_editor_property(albedo, "parameter_name", unreal.Name("CloudAlbedo"))
    _set_editor_property(
        albedo, "default_value", unreal.LinearColor(0.98, 0.98, 0.98, 1.0)
    )

    emission_scale = _create_expression(
        material, unreal.MaterialExpressionScalarParameter, 1960, 150
    )
    _set_editor_property(
        emission_scale, "parameter_name", unreal.Name("DebugEmissionScale")
    )
    _set_editor_property(emission_scale, "default_value", 0.0)
    density_emission = _create_expression(
        material, unreal.MaterialExpressionMultiply, 2180, 130
    )
    _connect(physical_density, "", density_emission, "A")
    _connect(emission_scale, "", density_emission, "B")

    emission_color = _create_expression(
        material, unreal.MaterialExpressionVectorParameter, 1960, 300
    )
    _set_editor_property(
        emission_color, "parameter_name", unreal.Name("EmissionColor")
    )
    _set_editor_property(
        emission_color, "default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0)
    )
    colored_emission = _create_expression(
        material, unreal.MaterialExpressionMultiply, 2400, 170
    )
    _connect(density_emission, "", colored_emission, "A")
    _connect(emission_color, "RGB", colored_emission, "B")

    # Legacy MD_Volume maps these material properties to volume coefficients:
    # Subsurface Color = extinction, Base Color = scattering albedo.
    _connect_property(
        extinction, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR
    )
    _connect_property(albedo, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    _connect_property(
        colored_emission, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )

    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.set_metadata_tag(
        material, unreal.Name("SkySimGeneratedVersion"), GENERATED_VERSION
    )
    if not unreal.EditorAssetLibrary.save_loaded_asset(
        material, only_if_is_dirty=False
    ):
        raise RuntimeError(f"Unable to save {ASSET_PATH}")

    unreal.log(
        "SkySim: generated /Game/SkySim/M_SkySimVolume "
        "(temporal density interpolation, de-tiling, mip-safe detail, clamped density Z)"
    )
    return material


if __name__ == "__main__":
    build_material()
