# Reflection Probes

Add `ReflectionProbeComponent` to a GameObject. Select that object to edit the component in the Inspector and display its influence sphere in Scene View.

| Property | Behavior |
| --- | --- |
| `reflectionMap` | Texture asset reference. Assign a texture from Content by dragging it into this Inspector field. Used by `Static` mode. |
| `updateMode` | `Static`, `OnLoad`, `EveryFrame`, or `Interval`. |
| `updateInterval` | Seconds between dynamic captures in `Interval` mode; default 20. |
| `resolution` | Cubemap face size, from 16 to 512 pixels. Values round up to a supported power of two. |
| `radius` | Local influence radius; the largest world transform scale also scales the sphere. |

`Static` converts an assigned HDR equirectangular texture into a cubemap on the GPU and filters it for specular and sheen reflections. Use linear float texture import settings, such as `R32G32B32A32_SFLOAT`. This input is a two-dimensional environment texture; six-face cubemap asset import is not provided.

`OnLoad` requests an initial scene capture. `Interval` requests an initial capture and then repeats at the configured interval. `EveryFrame` requests a capture for each prepared scene frame. Changing capture configuration requests a replacement; moving an `OnLoad` or `Interval` probe updates its influence without overriding its update schedule. Each dynamic capture renders six scene views, so `EveryFrame` has a substantial rendering cost.

The system supports 32 registered probes. Mesh preparation queries a dedicated probe Octree using the center of each mesh renderer's world bounds. It selects the nearest probe whose sphere contains that point; equal distances use a stable owner ID tie-break. Objects outside all probe spheres use the distant environment. The selected index belongs to the instance data, so objects using different probes can share a material and an instanced batch.

Specular and sheen cubemaps share one scene-wide bindless descriptor array, including probes with different resolutions. A probe keeps its last completed maps while a replacement is captured and filtered. Removing a probe cancels its pending publication, and retained scene submissions keep their immutable resource snapshots.
