#include "catalyst/common.hpp"
namespace catalyst {
namespace {
// Option lists come from Catalyst's property tooltips ("Options: None, Front, Back"). Their order is the
// stored value: each template default matches the tooltip's "Default:" at that index, e.g. cullMode 2 = Back.
Json options(std::initializer_list<const char *> names, const char *note = nullptr) {
  Json values = Json::object();
  int index = 0;
  for (auto name : names)
    values[std::to_string(index++)] = name;
  Json hint = {{"values", values}};
  if (note)
    hint["note"] = note;
  return hint;
}
Json note(const char *text) {
  return {{"note", text}};
}
const Json &table() {
  static const Json hints = [] {
    const auto space = options({"World", "Effect", "Emitter"}, "Axes the values are expressed in. Emitter = "
                                                               "orientation of the Creation Domain.");
    const auto clock = options({"Point", "Effect", "Emitter"},
                               "Clock that drives the 0..1 curves. Point: the particle's age; Effect: the "
                               "effect's timeline; Emitter: the emitter's timeline.");
    const auto disturbance = options(
        {"None", "Field", "Field And Global"},
        "None: base wind, gusts and variation only; Field: also the wind disturbance field; Field And "
        "Global: also global disturbances such as helicopter downwash and explosions.");
    const auto rotation = options({"Keyframe", "Effect", "Emitter"},
                                  "Models only. Effect or Emitter: lock to that orientation.");
    const auto loop = options({"Same", "Zero", "Min", "Max", "Rand Min-Max"},
                              "Frame playback restarts from after the last frame.");
    const Json technique = {
        {"known",
         {"RGBA_lit_soft", "RGBA_lit", "RGBA_soft", "RGBA_lit_normalspec", "RGBA_lit_soft_normalspec",
          "RGBA_lit_screen", "RGBA_screen", "RGBA_proj", "RGB_lit_soft", "RGB_lit", "RGB_soft",
          "RGB_lit_screen", "RGB_screen", "RGB_refract", "RGB_soft_refract",
          "RGB_lit_screen_refract_normalspec"}},
        {"note", "Technique names present in Catalyst.exe. Default RGBA_lit_soft: lit by the scene and fades "
                 "where it meets geometry; RGBA_lit: the same without the fade; *_normalspec: uses "
                 "NormalSpecMap. Only RGBA_lit_soft, RGBA_lit and RGBA_lit_soft_normalspec have been tried "
                 "through this server."}};
    Json h = Json::object();
    h["ParticleEffectRule"] = {
        {"numLoops", note("Repeats after the first play. 0 = play once, N = play N more times, -1 = loop "
                          "forever.")},
        {"drawListId", options({"Quality Low", "Quality Medium", "Quality High"},
                               "Draw bucket. Buckets are drawn in the order High, Medium, Low.")},
        {"dataVolumeType", options({"Fire (Contained)", "Fire", "Water", "Blaze", "Soak Slow", "Soak Medium",
                                    "Soak Fast", "Fire (Flame Thrower)"},
                                   "Gameplay category of the data volume.")},
        {"colnType", options({"None", "Ground Plane", "Ground Bound", "Nearby Bound", "Vehicle Plane",
                              "Underwater Plane", "Ground Bounds"},
                             "Where collision surfaces come from for particles with a Collision behaviour. "
                             "Also needs colnRange above 0.")},
        {"distanceCullingMode",
         note("When true, the effect fades from distanceCullingFadeDist and is culled "
              "beyond distanceCullingCullDist (meters from the camera). The "
              "*WhenDistanceCulled flags keep rendering, updating or emitting anyway.")},
        {"viewportCullingMode", note("When true, the effect stops while its culling sphere "
                                     "(viewportCullingSphereOffset, viewportCullingSphereRadius) is off "
                                     "screen.")}};
    h["ParticleRule"] = {
        {"sortType", options({"Closest First", "Farthest First", "Newest First", "Oldest First", "Random"},
                             "Draw order of one emitter's particles. Farthest First (back to front) suits "
                             "translucent particles.")},
        {"blendSet", options({"Normal", "Add", "Subtract", "Composite Alpha", "Composite Alpha Subtract"},
                             "How the particle colour combines with the screen. Catalyst notes that other "
                             "values render as Normal.")},
        {"cullMode", options({"None", "Front", "Back"},
                             "Which faces are not drawn. Back hides a sprite seen from behind; use None for "
                             "velocity-aligned sprites.")},
        {"depthWrite", note("Write particle pixels to the depth buffer. Usually off for translucent "
                            "particles.")}};
    h["ParticleDomain"] = {
        {"shape",
         options({"Box", "Sphere", "Cylinder", "Attractor"},
                 "Attractor is only for the Attractor domain. sizeOuter holds half-extents for a box "
                 "and radii otherwise; sizeInner hollows the shape.")},
        {"isWorldSpace", note("Keep the domain axes aligned to the world, ignoring effect rotation. Position "
                              "stays relative to the effect.")},
        {"isPointRelative",
         note("Target domain only. Off: launch velocity = target point - spawn position. "
              "On: launch velocity = target point - effect origin, so every particle gets a "
              "direction from the target alone, wherever it spawns.")},
        {"isCreationRelative", note("Target and Attractor domains only. Place the domain relative to the "
                                    "Creation Domain, including its position and rotation. Without "
                                    "isPointRelative, particles from a wide creation domain converge on the "
                                    "target.")},
        {"isTargetRelative", note("Attractor domain only. Place the domain relative to the Target Domain. "
                                  "Ignored if isCreationRelative is on.")}};
    h["ParticleDrawSprite"] = {
        {"alignmentMode",
         options(
             {"Camera", "Velocity", "World", "Effect", "Emitter"},
             "Camera: always faces the viewer. Velocity: the height axis follows the direction of travel. "
             "World, Effect, Emitter: the height axis follows alignAxis in that space. All modes but "
             "Camera are directional and shrink to zero when seen end-on.")},
        {"keyframeMode", options({"Point", "Effect", "Emitter"}, "Clock that drives the animRate curve.")},
        {"loopMode", loop}};
    h["ParticleDrawModel"] = {{"keyframeMode", options({"Point", "Effect", "Emitter"})}, {"loopMode", loop}};
    h["ParticleDrawTrail"] = {
        {"alignmentMode", options({"Camera", "World", "Effect", "Emitter"},
                                  "How the ribbon is oriented. There is no Velocity option.")}};
    h["SpriteShader"] = {{"technique", technique}};
    h["TrailShader"] = {{"technique", technique}};
    h["ParticleBehaviorColor"] = {{"keyframeMode", clock}};
    h["ParticleBehaviorSize"] = {{"keyframeMode", clock}};
    h["ParticleBehaviorAcceleration"] = {{"referenceSpace", space}};
    h["ParticleBehaviorDampening"] = {{"referenceSpace", space}};
    h["ParticleBehaviorNoise"] = {{"referenceSpace", space}};
    h["ParticleBehaviorMatrixWeight"] = {
        {"referenceSpace", options({"Effect", "Emitter"},
                                   "Which movement the particle follows. Emitter = movement of the Creation "
                                   "Domain.")}};
    h["ParticleBehaviorWind"] = {{"highLodDisturbanceMode", disturbance},
                                 {"lowLodDisturbanceMode", disturbance}};
    h["ParticleBehaviorRotation"] = {{"initRotationMode", rotation}, {"updateRotationMode", rotation}};
    h["ParticleBehaviorZCull"] = {
        {"mode", options({"Cull Under", "Cull Over", "Clamp Under", "Clamp Over"},
                         "What happens to particles past the height limit. Cull destroys the particle; "
                         "Clamp puts it back on the limit.")},
        {"referenceSpace", options({"World", "Effect", "Emitter", "Water Z (Camera)", "Water Z (Point)"},
                                   "What height is measured from.")}};
    h["ParticleBehaviorFogVolume"] = {
        {"lightningType", options({"Fog HDR", "Directional", "None"},
                                  "How the fog colour is lit. Fog HDR: time-of-day fog HDR value; "
                                  "Directional: the sun's colour, intensity and height; None: not lit.")}};
    // Unnumbered: the tooltip names Add as default, but blend_mode 0 was observed to average.
    h["ParticleEvolution"] = {
        {"blend_mode",
         {{"known", {"Add", "Average", "Max", "Full Average"}},
          {"note", "Modes named by Catalyst's tooltip for catalyst_set_evolution_properties. Their numbering "
                   "is unverified: blend_mode 0 was observed to average active slots."}}}};
    return h;
  }();
  return hints;
}
} // namespace
Json property_hints(const std::string &type) {
  const auto &hints = table();
  auto it = hints.find(type);
  return it == hints.end() ? Json::object() : *it;
}
} // namespace catalyst
