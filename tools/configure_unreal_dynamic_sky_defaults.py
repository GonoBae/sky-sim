"""Persist the live, time-driven SkySim authoring defaults in NewWorld."""

import unreal


MAP_PATH = "/Game/NewWorld"


def main():
    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
    if not world:
        raise RuntimeError(f"Could not load map: {MAP_PATH}")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    sky_actor = next(
        (
            actor
            for actor in actor_subsystem.get_all_level_actors()
            if actor.get_class().get_name() == "SkySimSystem"
        ),
        None,
    )
    if sky_actor is None:
        raise RuntimeError("NewWorld has no SkySimSystem actor")

    natural_preset = unreal.SkySimWeatherPreset.NATURAL
    settings = {
        "preview_in_editor": True,
        "animate_time_in_editor": True,
        "auto_apply_sky_controls_in_editor": True,
        "sync_authoring_settings_on_connect": True,
        "apply_advanced_weather_on_connect": False,
        "control_weather_preset": natural_preset,
        "control_time_scale": 60.0,
        "weather_seed": 55,
        "enable_wide_cloud_world": True,
        "cloud_world_horizontal_extent_km": 120.0,
        "auto_horizontal_tile_count": True,
        "wide_cloud_sampling_quality": 1.0,
        "macro_variation_strength": 0.13,
        "macro_variation_tiling": unreal.Vector(2.15, 1.65, 0.70),
        "density_coordinate_warp_strength": 0.16,
        "secondary_pattern_scale": unreal.Vector(0.83, 1.137, 1.0),
        "secondary_pattern_offset": unreal.Vector(0.37, 0.61, 0.0),
        "secondary_pattern_blend_strength": 0.72,
        "detail_erosion_strength": 0.07,
        "detail_noise_tiling": unreal.Vector(7.13, 5.77, 4.31),
        "density_shape_power": 0.92,
        "density_presentation_gain": 1.20,
        "use_physical_density_scale_for_rendering": False,
        "render_volume_size_cm": unreal.Vector(2000000.0, 2000000.0, 1400000.0),
    }
    sky_actor.modify()
    for property_name, value in settings.items():
        sky_actor.set_editor_property(property_name, value)

    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP_PATH):
        raise RuntimeError(f"Could not save map: {MAP_PATH}")

    unreal.log(
        "SkySim: saved dynamic-sky defaults "
        "(Natural seed 55, 60x time, editor preview, reconnect sync, "
        "120 x 14 km field, clustered coverage, dual-sample de-tiling, "
        "mip-safe cloud detail)"
    )


main()
