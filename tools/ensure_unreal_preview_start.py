import unreal


MAP_PATH = "/Game/NewWorld"
PLAYER_START_LABEL = "SkySim Preview Start"
CAMERA_LABEL = "SkySim Preview Camera"
# Keep the preview at a ground-observer height. Natural weather can lower a
# stratiform deck to roughly 200-300 m AGL, so the old 250 m camera could end
# up inside the cloud layer and make the distant field look like a flat floor.
PREVIEW_LOCATION = unreal.Vector(-375000.0, -1000000.0, 500.0)
PREVIEW_ROTATION = unreal.Rotator(pitch=14.0, yaw=90.0, roll=0.0)


def main():
    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
    if not world:
        raise RuntimeError(f"Could not load map: {MAP_PATH}")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    preview_start = None
    preview_camera = None
    sky_system = None
    for actor in actor_subsystem.get_all_level_actors():
        if isinstance(actor, unreal.PlayerStart) and actor.get_actor_label() == PLAYER_START_LABEL:
            preview_start = actor
        elif isinstance(actor, unreal.CameraActor) and actor.get_actor_label() == CAMERA_LABEL:
            preview_camera = actor
        elif actor.get_class().get_name() == "SkySimSystem":
            sky_system = actor

    if sky_system is None:
        raise RuntimeError("NewWorld has no SkySimSystem actor")

    # Keep the values authored on the placed SkySimSystem actor. This utility
    # only prepares a useful start transform; it must not overwrite cloud
    # appearance or custom layer settings chosen in the Details panel.

    if preview_start is None:
        preview_start = actor_subsystem.spawn_actor_from_class(
            unreal.PlayerStart,
            PREVIEW_LOCATION,
            PREVIEW_ROTATION,
            transient=False,
        )
        if preview_start is None:
            raise RuntimeError("Could not create the SkySim preview PlayerStart")
        preview_start.set_actor_label(PLAYER_START_LABEL)
        unreal.log("SkySim: created preview PlayerStart")
    else:
        preview_start.set_actor_location(PREVIEW_LOCATION, False, False)
        preview_start.set_actor_rotation(PREVIEW_ROTATION, False)
        unreal.log("SkySim: updated preview PlayerStart")

    if preview_camera is None:
        preview_camera = actor_subsystem.spawn_actor_from_class(
            unreal.CameraActor,
            PREVIEW_LOCATION,
            PREVIEW_ROTATION,
            transient=False,
        )
        if preview_camera is None:
            raise RuntimeError("Could not create the SkySim preview CameraActor")
        preview_camera.set_actor_label(CAMERA_LABEL)
        unreal.log("SkySim: created preview CameraActor")
    else:
        preview_camera.set_actor_location(PREVIEW_LOCATION, False, False)
        preview_camera.set_actor_rotation(PREVIEW_ROTATION, False)
        unreal.log("SkySim: updated preview CameraActor")

    # The CameraActor remains useful as an editor composition reference, but
    # Player 0 must view the possessed free-flight spectator pawn at runtime.
    preview_camera.set_editor_property(
        "auto_activate_for_player", unreal.AutoReceiveInput.DISABLED
    )
    camera_components = preview_camera.get_components_by_class(unreal.CameraComponent)
    if not camera_components:
        raise RuntimeError("SkySim preview CameraActor has no CameraComponent")
    camera_components[0].set_field_of_view(95.0)

    game_mode_class = unreal.load_class(
        None, "/Script/uskysim.uskysimGameModeBase"
    )
    if game_mode_class is None:
        raise RuntimeError("Could not load the native SkySim game mode")
    world.get_world_settings().set_editor_property(
        "default_game_mode", game_mode_class
    )

    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP_PATH):
        raise RuntimeError(f"Could not save map: {MAP_PATH}")

    unreal.log(
        "SkySim: preview PlayerStart and CameraActor ready at "
        f"({PREVIEW_LOCATION.x:.0f}, {PREVIEW_LOCATION.y:.0f}, {PREVIEW_LOCATION.z:.0f})"
    )


main()
