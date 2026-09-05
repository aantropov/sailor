// Main-pass instance layout is shared by MOTIONS and colour-only variants.
struct ObjectMotionData
{
  mat4 previousModel;
  uvec4 state;
};

#if defined(MOTIONS) && !defined(SHADOW) && !defined(SHADOW_CASTER)
#ifdef VERTEX
layout(location=12) out vec4 motionCurrentClip;
layout(location=13) out vec4 motionPreviousClip;
layout(location=14) flat out float motionAlphaBlend;

#ifdef SKINNING
layout(std430, set=0, binding=2) readonly buffer PreviousBoneMatrices
{
  mat4 instance[];
} previousBones;

vec4 PreviousMotionPosition(vec3 position, uvec4 ids, vec4 weights, uint offset, inout bool valid)
{
  uint lastBone = max(max(ids.x, ids.y), max(ids.z, ids.w));
  valid = valid && offset != 0xFFFFFFFFu && offset + lastBone < uint(previousBones.instance.length());
  if(!valid) return vec4(position, 1.0);
  mat4 skin = previousBones.instance[offset + ids.x] * weights.x +
    previousBones.instance[offset + ids.y] * weights.y +
    previousBones.instance[offset + ids.z] * weights.z +
    previousBones.instance[offset + ids.w] * weights.w;
  return skin * vec4(position, 1.0);
}
#endif

void WriteMotionVertex(vec4 currentClip, vec4 previousClip, bool valid, bool alphaBlend)
{
  motionCurrentClip = currentClip;
  motionPreviousClip = valid ? previousClip : currentClip;
  motionAlphaBlend = alphaBlend ? 1.0 : 0.0;
}
#endif

#ifdef FRAGMENT
layout(location=12) in vec4 motionCurrentClip;
layout(location=13) in vec4 motionPreviousClip;
layout(location=14) flat in float motionAlphaBlend;
layout(location=1) out vec4 outMotion;

void WriteMotionFragment(float alpha)
{
  vec2 velocity = vec2(0.0);
  if(motionCurrentClip.w > 0.00001 && motionPreviousClip.w > 0.00001)
  {
    // Main passes have a negative-height Vulkan viewport. Texture UVs have
    // a top-left origin, so their Y velocity has the opposite sign to NDC.
    velocity = (motionCurrentClip.xy / motionCurrentClip.w -
      motionPreviousClip.xy / motionPreviousClip.w) * vec2(0.5, -0.5);
    if(any(isnan(velocity)) || any(isinf(velocity))) velocity = vec2(0.0);
  }
  outMotion = vec4(clamp(velocity, -1.0, 1.0),
    clamp(motionCurrentClip.w, 0.0, 65000.0), motionAlphaBlend > 0.5 ? clamp(alpha, 0.0, 1.0) : 1.0);
}
#endif
#endif
