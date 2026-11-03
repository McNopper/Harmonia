# GI-SMS qualification fixtures

C1–C3 share triangulated sphere OBJs so Hyperion and Theia trace identical geometry;
Aether's analytic sphere primitive must not be used for cross-renderer comparison.

Triangle winding is outward, matching the supplied vertex normals. This is required
by the specular-event support rules: inward winding rejects every glass event even
when shading normals look correct. `Theia/tools/test_qualify_sms.py` checks every
nondegenerate caster triangle against the sphere center. Flip face index order only;
do not change geometry, normals, material parameters or measurement tolerances.

The C1 winding defect was the actual cause of zero local roots and therefore zero
spatial-reuse attempts. It was not evidence that curved SMS requires temporal reuse.
