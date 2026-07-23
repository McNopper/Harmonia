// common.slang — re-export of the shared GPU types for Harmonia's own (renderer-neutral)
// shaders and as a standalone-compilable fallback for the shared modules.
//
// Each renderer shadows this file with its OWN common.slang (the entry-shader compile uses
// `-I <renderer>/shaders -I Harmonia/shaders`, so `import common;` binds the renderer's copy,
// which carries its renderer-specific globals/bindings). This Harmonia copy therefore must NOT
// declare renderer-specific types/bindings (it historically held a stale, divergent 3rd copy of
// Hyperion's RayPayload/HitInfo/bindings — removed). It keeps only the gpu_types re-export, which
// is the sole thing the shared modules (path_integrator, medium_walk, bsdf_shared, env, …)
// consume from `common`.
__exported import gpu_types;
