/* Depth-based effects for presentation and captures. */
#define GLSL(...) #__VA_ARGS__
// clang-format off
static const char screen_space_shader[] = GLSL(
    uniform highp sampler2D sceneDepth;
    uniform int torchCount, capsuleCount;
    uniform float playerMinDepth;
    uniform vec4 capsuleStart[11], capsuleEnd[11];
    uniform vec4 torchCamera, torchCenters[8], torchColors[8];
    uniform bool showReflections, showOcclusion, showTorchShadows;

    // Zero marks background and overlays. Three byte lanes retain N64 depth
    // precision, including distant surfaces near the far plane.
    highp float viewDepth(vec2 uv) {
        vec3 packedZ = textureLod(sceneDepth, uv, 0.0).rgb;
        float code = dot(round(packedZ * 255.0), vec3(65536.0, 256.0, 1.0));
        if (code < 1.0) return 0.0;
        return 16777215.0 / max(16777216.0 - code, 1.0);
    }

    vec3 viewPosition(vec2 uv, float z, vec2 lens) {
        return vec3((2.0 * uv - 1.0) * lens, 1.0) * z;
    }

    // Use the closer neighbor in depth on each axis to avoid fitting a normal
    // across silhouettes. Both tangents point in increasing screen coordinates.
    vec3 tangent(vec2 stepUV, vec3 origin, vec2 lens) {
        float low = viewDepth(tc - stepUV), high = viewDepth(tc + stepUV);
        if (low > 0.0 && (high == 0.0 || abs(low - origin.z) < abs(high - origin.z)))
            return origin - viewPosition(tc - stepUV, low, lens);
        if (high > 0.0) return viewPosition(tc + stepUV, high, lens) - origin;
        return vec3(0.0);
    }

    // Window depth (and therefore 1/viewDepth) is affine on a projected
    // triangle. Extrapolate from each side before choosing a normal: the
    // smallest one-pixel depth change can point across a triangle seam.
    vec3 aoTangent(vec2 uv, vec2 stepUV, vec3 p, vec2 lens) {
        float lo = viewDepth(uv - stepUV), hi = viewDepth(uv + stepUV);
        float lo2 = viewDepth(uv - 2.0 * stepUV), hi2 = viewDepth(uv + 2.0 * stepUV);
        if (lo <= 0.0 && hi <= 0.0) return vec3(0);
        float center = 1.0 / p.z;
        float leftError = lo > 0.0 && lo2 > 0.0 ? abs(center - 2.0 / lo + 1.0 / lo2) : 2.0;
        float rightError = hi > 0.0 && hi2 > 0.0 ? abs(center - 2.0 / hi + 1.0 / hi2) : 2.0;
        bool left = leftError == rightError ? abs(lo - p.z) < abs(hi - p.z) : leftError < rightError;
        if (lo > 0.0 && (hi == 0.0 || left))
            return p - viewPosition(uv - stepUV, lo, lens);
        return viewPosition(uv + stepUV, hi, lens) - p;
    }

    // Approximate the occluded hemisphere by its highest nearby sample in six
    // directions. Bias rejects small coplanar depth errors. Range attenuation
    // prevents a distant surface across a silhouette from darkening the edge.
    float ambientVisibility(vec2 uv0, float z0, vec2 lens) {
        vec3 p = viewPosition(uv0, z0, lens);
        vec3 tx = aoTangent(uv0, vec2(pixel.x,0.0), p, lens);
        vec3 ty = aoTangent(uv0, vec2(0.0,pixel.y), p, lens);
        vec3 n = cross(tx,ty);
        if (dot(n,n) == 0.0 || max(abs(tx.z),abs(ty.z)) > 0.1 * z0) return 1.0;
        n = normalize(n);
        if (dot(n,p) > 0.0) n = -n;
        // Allow a native RDP Z unit between independently set-up triangles.
        // Convert window-Z uncertainty to distance above the receiver plane.
        float planeBias = abs(dot(n,p)) * p.z / 32768.0;
        float radius = min(3.0, p.z * 0.14);
        vec2 reach = 0.5 * radius / (p.z * lens);
        const vec2 directions[6] = vec2[6](vec2(1,0), vec2(0.5,0.866), vec2(-0.5,0.866),
                                            vec2(-1,0), vec2(-0.5,-0.866), vec2(0.5,-0.866));
        float occupied = 0.0;
        for (int i = 0; i < 6; ++i) {
            float horizon = 0.0;
            for (int j = 1; j <= 3; ++j) {
                vec2 uv = (floor((uv0 + directions[i] * reach * (float(j*j)/9.0)) / pixel) + 0.5) * pixel;
                if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1)))) continue;
                float z = viewDepth(uv);
                if (z == 0.0) continue;
                vec3 delta = viewPosition(uv, z, lens) - p;
                float dist = length(delta);
                if (dist < radius * 0.015 || dist >= radius) continue;
                horizon = max(horizon, ((dot(n, delta) - planeBias) / dist - 0.08) * (1.0 - dist/radius));
            }
            occupied += horizon;
        }
        return 1.0 - min(0.55, occupied * 0.23);
    }

    // Closest distance between the receiver-to-light ray and each body segment.
    // Both endpoints are in view space even when the camera cannot see them.
    float playerBlocker(vec3 start, vec3 end) {
        vec3 ray = end - start;
        float reach = length(ray);
        if (reach < 0.001) return 0.0;
        vec3 dir = ray / reach;
        float blocker = 0.0;
        for (int i = 0; i < capsuleCount; ++i) {
            vec3 a = capsuleStart[i].xyz - start;
            vec3 bone = capsuleEnd[i].xyz - capsuleStart[i].xyz;
            float radius = capsuleStart[i].w;
            float length2 = max(dot(bone,bone), 0.00001);
            // A surface on the player's own capsule should still receive light.
            vec3 nearest = a + bone * clamp(-dot(a,bone)/length2, 0.0, 1.0);
            if (dot(nearest,nearest) <= radius*radius) continue;
            vec3 side = bone - dir * dot(bone,dir);
            float t = clamp(-dot(a,side) / max(dot(side,side),0.00001), 0.0, 1.0);
            vec3 center = a + bone * t;
            float along = dot(center,dir);
            // For parallel segments or a closest point beyond the light,
            // minimize against the corresponding ray endpoint instead.
            if (along <= 0.0 || along >= reach) {
                vec3 endpoint = dir * clamp(along, 0.0, reach);
                center = a + bone * clamp(dot(endpoint-a,bone)/length2, 0.0, 1.0);
                along = clamp(dot(center,dir), 0.0, reach);
            }
            float dist = length(center - dir * along);
            blocker = max(blocker, 1.0 - smoothstep(0.80 * radius, radius, dist));
        }
        return blocker;
    }

    // Trace the receiver-to-flame segment against visible depth. A normal
    // offset avoids the receiver, and a small gap at the flame leaves the
    // emitting geometry clear. Limit blocker thickness to reject foreground
    // objects that merely overlap the ray in the camera view.
    float torchVisibility(vec3 surface, vec3 normal, vec4 emitter, float distanceToLight, bool playerInRange) {
        float footprint = surface.z * max(pixel.x * torchCamera.x, pixel.y * torchCamera.y) * 2.0;
        float bias = clamp(footprint, 0.007 * emitter.w, 0.035 * emitter.w);
        if (distanceToLight <= bias + 0.06 * emitter.w) return 1.0;
        vec3 start = surface + normal * bias;
        vec3 end = mix(emitter.xyz, surface, 0.06 * emitter.w / distanceToLight);
        float thickness = 0.22 * emitter.w;
        float blocker = playerInRange ? playerBlocker(start,end) : 0.0;
        if (blocker >= 1.0) return 0.0;
        for (int j = 1; j <= 24; ++j) {
            vec3 q = mix(start, end, float(j) / 24.0);
            if (q.z <= 0.0) break;
            vec2 uv = 0.5 + 0.5 * q.xy / (q.z * torchCamera.xy);
            if (any(lessThanEqual(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1)))) break;
            float z = viewDepth(uv);
            if (z == 0.0) continue;
            float denom = 1.0 - 1.0 / z - torchCamera.z;
            if (denom >= -0.000001) continue;
            float gap = q.z - torchCamera.w / denom;
            float hit = smoothstep(bias, 2.0 * bias, gap)
                      * (1.0 - smoothstep(thickness, 1.5 * thickness, gap));
            blocker = max(blocker, hit);
            if (blocker >= 1.0) break;
        }
        return 1.0 - blocker;
    }

    vec3 shadeScene(vec3 base) {
        if (!showOcclusion && !showReflections && torchCount == 0) return base;
        float z = viewDepth(tc);
        if (z == 0.0) return base;
        // RDP triangles arrive after projection. Approximate a 60-degree
        // vertical field of view and reconstruct positions in near-plane units.
        vec2 lens = 0.57735 * vec2(pixel.y / pixel.x, 1.0);
        if (showOcclusion) base *= ambientVisibility((floor(tc / pixel) + 0.5) * pixel, z, lens);
        if (!showReflections && torchCount == 0) return base;
        vec3 p = viewPosition(tc, z, lens);
        vec3 tx = tangent(vec2(pixel.x, 0.0), p, lens);
        vec3 ty = tangent(vec2(0.0, pixel.y), p, lens);
        if (max(abs(tx.z), abs(ty.z)) > 0.1 * z) return base;
        vec3 n = cross(tx, ty);
        if (dot(n, n) == 0.0) return base;
        n = normalize(n);
        vec3 incident = normalize(p);
        if (dot(n, incident) > 0.0) n = -n;
        if (torchCount > 0) {
            // Undo the game projection and N64 viewport Z mapping.
            float denom = 1.0 - 1.0/z - torchCamera.z;
            if (denom < -0.000001) {
                vec3 surface = viewPosition(tc, torchCamera.w / denom, torchCamera.xy);
                // Transform the depth-continuous normal into world units.
                vec3 normal = normalize(vec3(n.xy * lens / torchCamera.xy,
                                             n.z + (torchCamera.z-1.0) * dot(n,p)));
                vec3 light = vec3(0);
                for (int i = 0; i < torchCount; ++i) {
                    vec3 delta = torchCenters[i].xyz - surface;
                    float d = length(delta);
                    if (d >= torchCenters[i].w) continue;
                    float attenuation = 1.0 - smoothstep(0.0, torchCenters[i].w, d);
                    float diffuse = max(dot(normal, delta / max(d, 0.001)), 0.0);
                    if (showTorchShadows) {
                        bool playerInRange = torchColors[i].w > 0.0 && 1.0 - 1.0/z > playerMinDepth;
                        attenuation *= torchVisibility(surface, normal, torchCenters[i], d, playerInRange);
                    }
                    light += torchColors[i].rgb * attenuation * (0.22 + 0.78 * diffuse);
                }
                base += (0.12 + 0.72 * base) * light;
            }
        }
        if (!showReflections) return base;
        float floorMask = smoothstep(0.45, 0.80, n.y);
        if (floorMask == 0.0) return base;

        vec3 dir = reflect(incident, n);
        vec3 start = p + n * (0.004 * z);
        float lastT = 0.0;
        float previousGap = -1.0;
        for (int i = 1; i <= 32; ++i) {
            float t = float(i) * (0.012 + 0.0014 * float(i));
            vec3 q = start + dir * (z * t);
            if (q.z <= 0.0) break;
            vec2 uv = 0.5 + 0.5 * q.xy / (q.z * lens);
            vec2 edge = min(uv, 1.0 - uv);
            if (min(edge.x, edge.y) <= 0.0) break;
            float surfaceZ = viewDepth(uv);
            float gap = surfaceZ > 0.0 ? (q.z - surfaceZ) / q.z : -1.0;
            if (previousGap < 0.0 && gap >= 0.0) {
                // Refine the first crossing before testing thickness. A coarse
                // stride can step from open space onto a nearby silhouette.
                float lo = lastT, hi = t;
                for (int j = 0; j < 4; ++j) {
                    float mid = (lo + hi) * 0.5;
                    vec3 m = start + dir * (z * mid);
                    vec2 at = 0.5 + 0.5 * m.xy / (m.z * lens);
                    float d = viewDepth(at);
                    if (d > 0.0 && m.z >= d) {
                        hi = mid;
                        uv = at;
                        gap = (m.z - d) / m.z;
                    } else lo = mid;
                }
                if (gap < 0.12) {
                    float fade = smoothstep(0.0, 0.045, min(edge.x, edge.y));
                    float fresnel = pow(1.0 - clamp(-dot(incident, n), 0.0, 1.0), 3.0);
                    return mix(base, textureLod(image, uv, 0.0).rgb,
                               (0.25 + 0.45 * fresnel) * floorMask * fade * (1.0 - t / 2.0));
                }
            }
            lastT = t;
            previousGap = gap;
        }
        return base;
    }
);
// clang-format on
#undef GLSL
