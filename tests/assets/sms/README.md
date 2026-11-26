# Specular manifold sampling test scenes

The scenes in this directory qualify specular manifold sampling (SMS). They are written
in Aether's scene format; the header comment of each file states what the scene
exercises and which path depth it needs.

| Scene or file | Content |
|---------------|---------|
| `c1_curved_glass.scene.toml` | A closed, perfectly smooth glass sphere over a diffuse floor with a finite area emitter. The camera frames the floor under the sphere, where the transmission caustic lands |
| `c2_absorption_prefix.scene.toml` | The same sphere with colored pure absorption (exact Beer–Lambert) and a camera that looks through a glass pane before the floor: a specular camera prefix with a frozen throughput |
| `c3_boundary_mixed.scene.toml` | The first scene plus an opaque blocker (a visibility boundary through the caustic), a rough metal sphere (left to ordinary path tracing) and a back wall with diffuse indirect light |
| `complex_case_dragon_area_light.scene.toml` | The dragon and teapot content with a finite area emitter instead of the environment map, so that SMS has a finite target |
| `hybrid_k3.scene.toml`, `hybrid_k4.scene.toml` | Scenes in which Theia's hybrid reconnection shift has to advance to the third and to the fourth path vertex before a pair qualifies |
| `sms_qualification.materials.toml`, `hybrid_k3.materials.toml` | The material libraries (OpenPBR, colors in linear Rec.709) |
| `sms_sphere_r05.obj`, `sms_sphere_r035.obj` | The shared triangulated spheres |

All scenes use a black environment (the preview preset, no image-based lighting).

## Rules for these scenes

- The first three scenes share triangulated spheres so that Hyperion and Theia trace
  identical geometry. Aether's analytic sphere primitive is analytic in Hyperion and
  tessellated in Theia, so it must not be used for a comparison between the renderers.
- Triangle winding is outward and matches the vertex normals. The support rules for
  specular events require this: inward winding rejects every glass event, even when the
  shading normals look correct. `Theia/tools/test_qualify_sms.py` checks every
  non-degenerate caster triangle against the sphere center.
- To correct the winding, flip the order of the face indices only. Do not change the
  geometry, the normals, the material parameters or the measurement tolerances.
