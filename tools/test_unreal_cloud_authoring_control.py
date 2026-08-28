"""Exercise SkySimSystem's Details-style cloud control without saving the map."""

import time

import unreal


MAP_PATH = "/Game/NewWorld"


world = unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
if not world:
    raise RuntimeError(f"Could not load {MAP_PATH}")

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
sky_system = next(
    (
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if actor.get_class().get_name() == "SkySimSystem"
    ),
    None,
)
if sky_system is None:
    raise RuntimeError("NewWorld has no SkySimSystem actor")

# Send a clearly different coverage and then restore the authored default. The
# map is intentionally not saved, so this validates the same UFUNCTION used by
# the Details button without modifying the user's asset.
for coverage in (0.52, 0.60):
    sky_system.set_editor_property("custom_cloud_coverage", coverage)
    sky_system.apply_custom_cumulus_settings()
    unreal.log(f"SkySimAuthoringTest: sent coverage={coverage:.2f}")
    time.sleep(1.0)

unreal.log("SkySimAuthoringTest: control sequence complete; map left unsaved")
