// Checks ApplyRoll against a synthetic view-projection, both storage conventions.
// The question it answers: does a point projected through a rolled matrix land where a
// rotation of the NDC plane by the same angle would put it, INCLUDING when tanX != tanY?
#include <cstdio>
#include <cmath>

struct Reg4 { float x, y, z, w; };
const int CONV_ROW = 0, CONV_COL = 1;

inline void ProjTangents(const Reg4* r, int conv, float* tanX, float* tanY) {
    float xc[3], yc[3];
    if (conv == CONV_ROW) {
        xc[0] = r[0].x; xc[1] = r[1].x; xc[2] = r[2].x;
        yc[0] = r[0].y; yc[1] = r[1].y; yc[2] = r[2].y;
    } else {
        xc[0] = r[0].x; xc[1] = r[0].y; xc[2] = r[0].z;
        yc[0] = r[1].x; yc[1] = r[1].y; yc[2] = r[1].z;
    }
    float lx = sqrtf(xc[0]*xc[0] + xc[1]*xc[1] + xc[2]*xc[2]);
    float ly = sqrtf(yc[0]*yc[0] + yc[1]*yc[1] + yc[2]*yc[2]);
    *tanX = (lx > 1e-6f) ? 1.0f / lx : 0.0f;
    *tanY = (ly > 1e-6f) ? 1.0f / ly : 0.0f;
}

// ---- verbatim from d3d9.cpp ----
void ApplyRoll(Reg4* r, int conv, float rollRad, float tanX, float tanY) {
    if (rollRad == 0.0f) return;
    if (!(tanX > 1e-6f && tanY > 1e-6f)) return;
    const float cs = cosf(rollRad), sn = sinf(rollRad);
    const float xy = (tanY / tanX) * sn;
    const float yx = (tanX / tanY) * sn;
    if (conv == CONV_ROW) {
        for (int i = 0; i < 4; ++i) {
            const float x = r[i].x, y = r[i].y;
            r[i].x = cs * x - xy * y;
            r[i].y = yx * x + cs * y;
        }
    } else {
        for (int k = 0; k < 4; ++k) {
            const float x = (&r[0].x)[k], y = (&r[1].x)[k];
            (&r[0].x)[k] = cs * x - xy * y;
            (&r[1].x)[k] = yx * x + cs * y;
        }
    }
}

void ApplyEyeRemap(Reg4* r, int conv, float s) {
    if (conv == CONV_ROW) {
        for (int i = 0; i < 4; ++i) r[i].x = 0.5f * r[i].x + s * r[i].w;
    } else {
        r[0].x = 0.5f * r[0].x + s * r[3].x;
        r[0].y = 0.5f * r[0].y + s * r[3].y;
        r[0].z = 0.5f * r[0].z + s * r[3].z;
        r[0].w = 0.5f * r[0].w + s * r[3].w;
    }
}

// Camera at the origin of translated-world space looking down +X (the game is Z-up, X forward,
// Y right), with the projection scales the mod forces. Built in both conventions so the two
// code paths are exercised on the same geometry.
//   clip.x = viewRight * (1/tanX)   -> world  y
//   clip.y = viewUp    * (1/tanY)   -> world  z
//   clip.w = viewFwd                -> world  x
void BuildView(Reg4* r, int conv, float tanX, float tanY) {
    for (int i = 0; i < 4; ++i) r[i] = Reg4{0,0,0,0};
    const float sx = 1.0f / tanX, sy = 1.0f / tanY;
    if (conv == CONV_ROW) {
        // clip.c = sum_i v_i * r[i].c + r[3].c
        r[1].x = sx;   // world y -> clip x
        r[2].y = sy;   // world z -> clip y
        r[0].w = 1.0f; // world x -> clip w
    } else {
        // clip.x = dot(r[0].xyz, v) + r[0].w, etc.
        r[0].y = sx;
        r[1].z = sy;
        r[3].x = 1.0f;
    }
}

void Project(const Reg4* r, int conv, const float* v, float* ndcX, float* ndcY) {
    float cx, cy, cw;
    if (conv == CONV_ROW) {
        cx = v[0]*r[0].x + v[1]*r[1].x + v[2]*r[2].x + r[3].x;
        cy = v[0]*r[0].y + v[1]*r[1].y + v[2]*r[2].y + r[3].y;
        cw = v[0]*r[0].w + v[1]*r[1].w + v[2]*r[2].w + r[3].w;
    } else {
        cx = v[0]*r[0].x + v[1]*r[0].y + v[2]*r[0].z + r[0].w;
        cy = v[0]*r[1].x + v[1]*r[1].y + v[2]*r[1].z + r[1].w;
        cw = v[0]*r[3].x + v[1]*r[3].y + v[2]*r[3].z + r[3].w;
    }
    *ndcX = cx / cw; *ndcY = cy / cw;
}

int g_fail = 0;
void Check(const char* what, float got, float want, float tol = 1e-4f) {
    const bool ok = fabsf(got - want) <= tol;
    if (!ok) ++g_fail;
    printf("  %-58s got %+8.4f want %+8.4f  %s\n", what, got, want, ok ? "ok" : "**FAIL**");
}

int main() {
    // The real forced frustum: 98 deg vertical, 4096x2160 backbuffer, duplication on, so
    // tanX is the HALF-width per-eye value ApplyVrFov computes.
    const float tanY = tanf(49.0f * 3.14159265f / 180.0f);
    const float aspect = 4096.0f / 2160.0f;
    const float tanX = tanY * aspect * 0.5f;
    const float theta = 20.0f * 3.14159265f / 180.0f;

    for (int conv = 0; conv < 2; ++conv) {
        printf("\n=== %s ===\n", conv == CONV_ROW ? "CONV_ROW" : "CONV_COL");
        Reg4 r[4];
        BuildView(r, conv, tanX, tanY);

        float mx, my; ProjTangents(r, conv, &mx, &my);
        printf("  built frustum: tanX %.4f (want %.4f), tanY %.4f (want %.4f)\n", mx, tanX, my, tanY);

        // A point straight up the view's own up axis, at the top edge of the frustum.
        const float depth = 100.0f;
        float top[3] = { depth, 0.0f, depth * tanY };   // world x fwd, z up
        float nx0, ny0; Project(r, conv, top, &nx0, &ny0);
        Check("unrolled: top-edge point sits at NDC y = +1", ny0, 1.0f);
        Check("unrolled: top-edge point sits at NDC x =  0", nx0, 0.0f);

        // Roll the matrix, then project the SAME world point.
        Reg4 rr[4]; for (int i = 0; i < 4; ++i) rr[i] = r[i];
        ApplyRoll(rr, conv, theta, mx, my);
        float nx1, ny1; Project(rr, conv, top, &nx1, &ny1);

        // What it must equal: the view-space direction rotated by theta, then projected.
        // view (right, up) = (0, tanY*depth)/depth -> after roll by theta about forward:
        //   right' = -sin(theta)*tanY, up' = cos(theta)*tanY
        // NDC = (right'/tanX, up'/tanY)
        const float wantX = (-sinf(theta) * tanY) / tanX;
        const float wantY = ( cosf(theta) * tanY) / tanY;
        Check("rolled: top-edge point NDC x (aspect-corrected)", nx1, wantX);
        Check("rolled: top-edge point NDC y", ny1, wantY);

        // The decisive property: the point's direction in VIEW space must have rotated by
        // exactly theta. Undo the aspect to get back to view-space direction and measure it.
        const float vx0 = nx0 * tanX, vy0 = ny0 * tanY;
        const float vx1 = nx1 * tanX, vy1 = ny1 * tanY;
        const float a0 = atan2f(vy0, vx0), a1 = atan2f(vy1, vx1);
        // ApplyRoll(+theta) rotates the image CONTENT by +theta in view space (counter-clockwise
        // as the viewer sees it). Whether that is the correct direction for a given head tilt is
        // a handedness question between OpenXR's frame and the game's, which is what g_rollSign
        // and NUMPAD2 exist to settle in the headset - the same escape hatch F4/F5 give pitch and
        // yaw, both of which were guessed wrong first time. What this asserts is the part that
        // must hold regardless: the magnitude is exactly the angle asked for.
        float d = (a1 - a0) * 180.0f / 3.14159265f;
        while (d < -180.0f) d += 360.0f; while (d > 180.0f) d -= 360.0f;
        Check("view-space rotation equals the requested angle (deg)", d, 20.0f, 1e-3f);

        // Length must be preserved - a roll rotates, it must not scale.
        const float l0 = sqrtf(vx0*vx0 + vy0*vy0), l1 = sqrtf(vx1*vx1 + vy1*vy1);
        Check("view-space magnitude unchanged (no shear/scale)", l1, l0, 1e-4f);

        // Roll then eye-remap must equal the per-eye image of the rolled full frame:
        // the left half maps NDC x in [-1,0], so x' = 0.5x - 0.5.
        Reg4 re[4]; for (int i = 0; i < 4; ++i) re[i] = rr[i];
        ApplyEyeRemap(re, conv, -0.5f);
        float ex, ey; Project(re, conv, top, &ex, &ey);
        Check("roll composes with the left-eye remap (x)", ex, 0.5f * nx1 - 0.5f);
        Check("roll composes with the left-eye remap (y)", ey, ny1);

        // Zero roll must be exactly a no-op.
        Reg4 rz[4]; for (int i = 0; i < 4; ++i) rz[i] = r[i];
        ApplyRoll(rz, conv, 0.0f, mx, my);
        float zx, zy; Project(rz, conv, top, &zx, &zy);
        Check("zero roll is a no-op (x)", zx, nx0, 0.0f);
        Check("zero roll is a no-op (y)", zy, ny0, 0.0f);

        // A square frustum must give a pure NDC rotation - the cross-check on the aspect terms.
        Reg4 sq[4]; BuildView(sq, conv, tanY, tanY);
        ApplyRoll(sq, conv, theta, tanY, tanY);
        float sx_, sy_; Project(sq, conv, top, &sx_, &sy_);
        Check("square frustum: NDC x = -sin(theta)", sx_, -sinf(theta));
        Check("square frustum: NDC y =  cos(theta)", sy_,  cosf(theta));
    }

    printf("\n%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
