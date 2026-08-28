"""Print the saved SkySim level's actors and transforms without modifying it."""

import unreal


LEVEL = "/Game/NewWorld"

world = unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
if not world:
    raise RuntimeError(f"Could not load {LEVEL}")

world_settings = world.get_world_settings()
default_game_mode = world_settings.get_editor_property("default_game_mode")
unreal.log(
    "SkySimInspect: "
    f"default_game_mode={default_game_mode.get_path_name() if default_game_mode else None}"
)

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = actor_subsystem.get_all_level_actors()
unreal.log(f"SkySimInspect: actor_count={len(actors)}")
for actor in actors:
    transform = actor.get_actor_transform()
    unreal.log(
        "SkySimInspect: "
        f"name={actor.get_name()} label={actor.get_actor_label()} "
        f"class={actor.get_class().get_name()} "
        f"location={transform.translation} rotation={transform.rotation.rotator()} "
        f"scale={transform.scale3d}"
    )
    if isinstance(actor, unreal.CameraActor):
        camera_components = actor.get_components_by_class(unreal.CameraComponent)
        fov = camera_components[0].get_editor_property("field_of_view") if camera_components else None
        unreal.log(
            "SkySimInspect: "
            f"camera_auto_activate={actor.get_editor_property('auto_activate_for_player')} "
            f"camera_fov={fov}"
        )
    if actor.get_class().get_name() == "SkySimSystem":
        rendering_properties = (
            "preview_in_editor",
            "animate_time_in_editor",
            "auto_apply_sky_controls_in_editor",
            "sync_authoring_settings_on_connect",
            "apply_advanced_weather_on_connect",
            "control_local_date_time",
            "utc_offset_hours",
            "control_latitude_degrees",
            "control_longitude_degrees",
            "control_weather_preset",
            "control_time_scale",
            "weather_seed",
            "enable_volume_rendering",
            "use_server_domain_size",
            "enable_wide_cloud_world",
            "cloud_world_horizontal_extent_km",
            "wide_cloud_sampling_quality",
            "unreal_units_per_meter",
            "extinction_scale",
            "debug_emission_scale",
            "macro_variation_strength",
            "density_coordinate_warp_strength",
            "secondary_pattern_scale",
            "secondary_pattern_offset",
            "secondary_pattern_blend_strength",
            "detail_erosion_strength",
            "detail_noise_tiling",
            "density_shape_power",
            "density_presentation_gain",
            "use_physical_density_scale_for_rendering",
            "render_volume_size_cm",
            "custom_cloud_coverage",
            "estimated_cloud_source_count",
            "custom_cloud_base_altitude_agl_meters",
            "custom_cloud_top_altitude_agl_meters",
            "custom_cloud_optical_depth",
            "custom_cloud_convective_activity",
            "auto_apply_custom_cumulus",
        )
        values = []
        for property_name in rendering_properties:
            try:
                values.append(
                    f"{property_name}={actor.get_editor_property(property_name)}"
                )
            except Exception as error:
                values.append(f"{property_name}=<unavailable:{error}>")
        unreal.log("SkySimInspect: rendering " + " ".join(values))
    for component in actor.get_components_by_class(unreal.ActorComponent):
        unreal.log(
            "SkySimInspect: "
            f"component={component.get_name()} class={component.get_class().get_name()}"
        )
