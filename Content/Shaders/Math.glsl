const float PI = 3.14159265359;
const float TwoPI = 2 * PI;
const float HalfPI = 0.5 * PI;
const float E = 2.71828;

float rcp(float value)
{
  return 1.0 / value;
}

vec2 rcp(vec2 value)
{
  return 1.0 / value;
}

float saturate(float value)
{
  return clamp(value, 0.0, 1.0);
}

vec4 QuatAxisAngle(vec3 axis, float angleRad)
{
  const float halfAngle = angleRad / 2.0f;
  
  vec4 q = vec4(axis.x * sin(halfAngle),
                axis.y * sin(halfAngle),
                axis.z * sin(halfAngle),
                cos(halfAngle));
  
  return q;
}

vec4 QuatConj(vec4 q)
{ 
  return vec4(-q.x, -q.y, -q.z, q.w); 
}
  
vec4 QuatMult(vec4 q1, vec4 q2)
{ 
  vec4 qr;
  qr.x = (q1.w * q2.x) + (q1.x * q2.w) + (q1.y * q2.z) - (q1.z * q2.y);
  qr.y = (q1.w * q2.y) - (q1.x * q2.z) + (q1.y * q2.w) + (q1.z * q2.x);
  qr.z = (q1.w * q2.z) + (q1.x * q2.y) - (q1.y * q2.x) + (q1.z * q2.w);
  qr.w = (q1.w * q2.w) - (q1.x * q2.x) - (q1.y * q2.y) - (q1.z * q2.z);
  return qr;
}

vec3 Rotate(vec3 v, vec4 quat)
{ 
  vec4 qr = quat;
  vec4 qr_conj = QuatConj(qr);
  vec4 q_pos = vec4(v.x, v.y, v.z, 0);
  
  vec4 q_tmp = QuatMult(qr, q_pos);
  qr = QuatMult(q_tmp, qr_conj);
  
  return vec3(qr.x, qr.y, qr.z);
}

vec3 Rotate(vec3 v, vec3 axis, float angle)
{ 
  vec4 quat = QuatAxisAngle(axis, angle);
  
  return Rotate(v, quat);
}

vec2 slerp(vec2 start, vec2 end, float t)
{
     // Dot product - the cosine of the angle between 2 vectors.
     float dot = dot(start, end);     
     // Clamp it to be in the range of Acos()
     // This may be unnecessary, but floating point
     // precision can be a fickle mistress.
     dot = clamp(dot, -1.0, 1.0);
     // Acos(dot) returns the angle between start and end,
     // And multiplying that by t returns the angle between
     // start and the final result.
     float theta = acos(dot)*t;
     vec2 RelativeVec = normalize(end - start*dot); // Orthonormal basis
     // The final result.
     return ((start*cos(theta)) + (RelativeVec*sin(theta)));
}

vec3 slerp(vec3 start, vec3 end, float t)
{
     float dot = dot(start, end);     
     dot = clamp(dot, -1.0, 1.0);
     float theta = acos(dot)*t;
     vec3 RelativeVec = normalize(end - start*dot); // Orthonormal basis
     return ((start*cos(theta)) + (RelativeVec*sin(theta)));
}

vec4 slerp(vec4 start, vec4 end, float t)
{
     float dot = dot(start, end);     
     dot = clamp(dot, -1.0, 1.0);
     float theta = acos(dot)*t;
     vec4 RelativeVec = normalize(end - start*dot); // Orthonormal basis
     return ((start*cos(theta)) + (RelativeVec*sin(theta)));
}

// Vulkan NDC, Reverse Z: minDepth = 1.0, maxDepth = 0.0
const vec2 NdcUpperLeft = vec2(-1.0, -1.0);
const float NdcNearPlane = 0.0;
const float NdcFarPlane = 1.0;

struct ViewFrustum
{
    vec4 planes[4]; 
    vec2 center;
};

vec4 ComputePlane(vec3 p0, vec3 p1, vec3 p2)
{
    vec4 plane;
    vec3 v0 = p1 - p0;
    vec3 v2 = p2 - p0;

    plane.xyz = normalize( cross( v0, v2 ) );

    // Compute the distance to the origin using p0.
    plane.w = dot(plane.xyz, p0);

    return plane;
}

float LinearizeDepth(float depth, vec2 cameraZNearZFar)
{
    return cameraZNearZFar.x * cameraZNearZFar.y / 
    (cameraZNearZFar.y + depth * (cameraZNearZFar.x - cameraZNearZFar.y));
}

// Convert clip space coordinates to view space
vec4 ClipSpaceToViewSpace(vec4 clip, mat4 invProjection)
{
    // View space position
    vec4 view = invProjection * clip;
    // Perspective projection
    view = view/view.w;

    // Reverse Z
    view.z *= -1;
    
    return view;
}

// Convert view space coordinates to screen space
vec4 ViewSpaceToScreenSpace(vec4 view, mat4 projection)
{
    vec4 res = projection * view;
    return res / res.w;
}

// Convert screen space coordinates to view space.
vec4 ScreenSpaceToViewSpace(vec4 screen, vec2 viewportSize, mat4 invProjection)
{
    // Convert to normalized texture coordinates
    vec2 texCoord = screen.xy / viewportSize;

    // Convert to clip space
    vec4 clip = vec4(vec2(texCoord.x, texCoord.y) * 2.0f - 1.0f, screen.z, screen.w);

    return ClipSpaceToViewSpace(clip, invProjection);
}

// Convert screen space coordinates to view space.
vec4 ScreenSpaceToViewSpace(vec2 texCoord, float z, mat4 invProjection)
{
    // Convert to clip space
    vec4 clip = vec4(vec2(texCoord.x, texCoord.y) * 2.0f - 1.0f, z, 1.0f);

    return ClipSpaceToViewSpace(clip, invProjection);
}

// Construct view frustum
ViewFrustum CreateViewFrustum(ivec2 viewportSize, mat4 invProjection)
{
  vec3 eyePos = vec3(0,0,0);
  vec4 screenSpace[5];

  // Top left point
  screenSpace[0] = vec4(0.0f, 0.0f, -1.0f, 1.0f );
  // Top right point
  screenSpace[1] = vec4(viewportSize.x, 0.0f, -1.0f, 1.0f );
  // Bottom left point
  screenSpace[2] = vec4(0.0f, viewportSize.y, -1.0f, 1.0f );
  // Bottom right point
  screenSpace[3] = vec4(viewportSize.xy, -1.0f, 1.0f );
  // Center point
  screenSpace[4] = (screenSpace[0] + screenSpace[3]) * 0.5f;
  
  vec3 viewSpace[5];
  // Now convert the screen space points to view space
  for ( int i = 0; i < 5; i++ ) 
  {
      viewSpace[i] = ScreenSpaceToViewSpace(screenSpace[i], viewportSize, invProjection).xyz;
  }

  ViewFrustum frustum;

  // Left plane
  frustum.planes[0] = ComputePlane(eyePos, viewSpace[2], viewSpace[0]);
  // Right plane
  frustum.planes[1] = ComputePlane(eyePos, viewSpace[1], viewSpace[3]);
  // Top plane
  frustum.planes[2] = ComputePlane(eyePos, viewSpace[0], viewSpace[1]);
  // Bottom plane
  frustum.planes[3] = ComputePlane(eyePos, viewSpace[3], viewSpace[2]);
  
  frustum.center = viewSpace[4].xy;
  
  return frustum;
}

bool SphereFrustumOverlaps(vec3 lightPos, float radius, ViewFrustum frustum, float zNear, float zFar)
{
  if (lightPos.z - radius > zNear || lightPos.z + radius < zFar) 
  {
    return false;
  }
  
  for(int i = 0; i < 4; i++)
  {
    if(dot(frustum.planes[i].xyz, lightPos) - frustum.planes[i].w < -radius)
    {
      return false;
    }
  }
  return true;
}

// https://gist.github.com/wwwtyro/beecc31d65d1004f5a9d
vec2 RaySphereIntersect(vec3 r0, vec3 rd, vec3 s0, float sr) 
{
  // - r0: ray origin
  // - rd: normalized ray direction
  // - s0: sphere center
  // - sr: sphere radius
  // - Returns distance from r0 to first intersecion with sphere,
  //   or -1.0 if no intersection.
  float a = 1.0f;
  vec3 s0_r0 = r0 - s0;
  float b = 2.0 * dot(rd, s0_r0);
  float c = dot(s0_r0, s0_r0) - (sr * sr);
  if (b*b - 4.0*a*c < 0.0) 
  {
      return vec2(-1.0);
  }
  
  float tmp = sqrt((b*b) - 4.0*a*c);
  float x1 = (-b + tmp)/(2.0*a);
  float x2 = (-b - tmp)/(2.0*a);
  
  return x1 < x2 ? vec2(x1, x2) : vec2(x2, x1);
}

float NormAngle180(float angle)
{
  // Reduce the angle  
  angle =  mod(angle, 360); 
  
  // Force it to be the positive remainder, so that 0 <= angle < 360  
  angle = mod((angle + 360), 360);
  
  // force into the minimum absolute value residue class, so that -180 < angle <= 180  
  if (angle > 180)
  {
      angle -= 360;
  }
  
  return angle;
}

// Compute Van der Corput radical inverse
// See: http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
bool ProjectSphere(vec3 C, float r, float znear, float P00, float P11, out vec4 aabb)
{
  if (C.z < r + znear)
      return false;

  vec2 cx = -C.xz;
  vec2 vx = vec2(sqrt(dot(cx, cx) - r * r), r);
  vec2 minx = mat2(vx.x, vx.y, -vx.y, vx.x) * cx;
  vec2 maxx = mat2(vx.x, -vx.y, vx.y, vx.x) * cx;

  vec2 cy = -C.yz;
  vec2 vy = vec2(sqrt(dot(cy, cy) - r * r), r);
  vec2 miny = mat2(vy.x, vy.y, -vy.y, vy.x) * cy;
  vec2 maxy = mat2(vy.x, -vy.y, vy.y, vy.x) * cy;

  aabb = vec4(minx.x / minx.y * P00, miny.x / miny.y * P11, maxx.x / maxx.y * P00, maxy.x / maxy.y * P11);
  aabb = aabb.xwzy * vec4(0.5f, -0.5f, 0.5f, -0.5f) + vec4(0.5f); // clip space -> uv space

  return true;
}