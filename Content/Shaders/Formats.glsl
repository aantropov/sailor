vec3 convertRGB2XYZ(vec3 _rgb)
{
    // Reference(s):
    // - RGB/XYZ Matrices
    //   https://web.archive.org/web/20191027010220/http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
    vec3 xyz;
    xyz.x = dot(vec3(0.4124564, 0.3575761, 0.1804375), _rgb);
    xyz.y = dot(vec3(0.2126729, 0.7151522, 0.0721750), _rgb);
    xyz.z = dot(vec3(0.0193339, 0.1191920, 0.9503041), _rgb);
    return xyz;
}

vec3 convertXYZ2Yxy(vec3 _xyz)
{
    // Reference(s):
    // - XYZ to xyY
    //   https://web.archive.org/web/20191027010144/http://www.brucelindbloom.com/index.html?Eqn_XYZ_to_xyY.html
    float inv = 1.0/dot(_xyz, vec3(1.0, 1.0, 1.0) );
    return vec3(_xyz.y, _xyz.x*inv, _xyz.y*inv);
}

vec3 convertRGB2Yxy(vec3 _rgb)
{
    return convertXYZ2Yxy(convertRGB2XYZ(_rgb) );
}

vec3 convertXYZ2RGB(vec3 _xyz)
{
    vec3 rgb;
    rgb.x = dot(vec3( 3.2404542, -1.5371385, -0.4985314), _xyz);
    rgb.y = dot(vec3(-0.9692660,  1.8760108,  0.0415560), _xyz);
    rgb.z = dot(vec3( 0.0556434, -0.2040259,  1.0572252), _xyz);
    return rgb;
}

vec3 convertYxy2XYZ(vec3 _Yxy)
{
    // Reference(s):
    // - xyY to XYZ
    //   https://web.archive.org/web/20191027010036/http://www.brucelindbloom.com/index.html?Eqn_xyY_to_XYZ.html
    vec3 xyz;
    xyz.x = _Yxy.x*_Yxy.y/_Yxy.z;
    xyz.y = _Yxy.x;
    xyz.z = _Yxy.x*(1.0 - _Yxy.y - _Yxy.z)/_Yxy.z;
    return xyz;
}

vec3 convertYxy2RGB(vec3 _Yxy)
{
    return convertXYZ2RGB(convertYxy2XYZ(_Yxy));
}

const vec2 invAtan = vec2(0.1591f, 0.3183f);

vec2 EquirectangularCoords(vec3 cubemapDirection)
{
    vec2 uv = vec2(atan(cubemapDirection.z, cubemapDirection.x), asin(cubemapDirection.y));
    uv *= invAtan;
    uv += 0.5f;
    return uv;
}
