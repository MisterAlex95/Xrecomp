/* The POSIX D3D8 back end (src/d3d/d3d8_gl.c) on a real GL context: what
 * its draw calls put on a 64x64 back buffer, read back with glReadPixels.
 * Pre-transformed vertices (XYZRHW + DIFFUSE, which d3d8_gl.c passes as
 * clip coordinates), no culling, no depth test.
 *
 *   1. DrawPrimitiveUP of a quad list (no GL primitive): two quads, red
 *      and green, each drawn whole (before: as prim_count * 3 vertices of
 *      triangles, half of each quad and a triangle across both);
 *   2. DrawIndexedPrimitive after SetIndices(ib, 4): the indices count from
 *      vertex 4 (before: the base was ignored and vertices 0..3 drawn);
 *   3. DrawIndexedPrimitiveUP with MinIndex 4: indices 4..7 reach vertices
 *      past the first NumVertices (before: only those were uploaded);
 *   4. DrawIndexedPrimitiveUP of a quad list through indices.
 * Exit 77 without a GL 3.3 core context. */
#include <stdio.h>
#include <string.h>
#include "d3d8_xbox.h"
#include "gl_host.h"

#define D3D_OK ((HRESULT)0)

typedef struct { float x, y, z, w; DWORD c; } V;
#define RED 0xFFFF0000u
#define GRN 0xFF00FF00u
#define BLU 0xFF0000FFu
#define YEL 0xFFFFFF00u

static int s_fail;

/* The back buffer's pixel (x, y), GL's origin bottom-left, as 0xRRGGBB. */
static unsigned px(int x, int y)
{
    unsigned char p[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}

static void expect(const char *what, const int (*at)[2], int n, unsigned want)
{
    int i;
    printf("%s:", what);
    for (i = 0; i < n; i++) {
        unsigned got = px(at[i][0], at[i][1]);
        printf(" %06X", got);
        if (got != want)
            s_fail = 1;
    }
    printf(" (want %06X)\n", want);
}

static void quad(V *v, float x0, float y0, float x1, float y1, DWORD c)
{
    V q[4] = { { x0, y0, 0.5f, 1, c }, { x1, y0, 0.5f, 1, c }, { x1, y1, 0.5f, 1, c },
               { x0, y1, 0.5f, 1, c } };
    memcpy(v, q, sizeof q);
}

int main(void)
{
    IDirect3D8 *d3d = xbox_Direct3DCreate8(0);
    IDirect3DDevice8 *dev;
    D3DPRESENT_PARAMETERS pp;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    if (d3d->lpVtbl->CreateDevice(d3d, 0, 0, 0, 0, &pp, &dev) != D3D_OK) {
        puts("no GL 3.3 core context: d3d8_gl checks skipped");
        return 77;
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, 0);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000u, 1.0f, 0);

    /* 1. Quad list: top half, red left, green right. */
    {
        static const int l[][2] = { { 2, 62 }, { 30, 34 }, { 2, 34 }, { 30, 62 } };
        static const int r[][2] = { { 34, 62 }, { 62, 34 }, { 34, 34 }, { 62, 62 } };
        V v[8];
        quad(v, -1, 0, 0, 1, RED);
        quad(v + 4, 0, 0, 1, 1, GRN);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_QUADLIST, 2, v, sizeof(V));
        expect("1. quad list, first quad", l, 4, 0xFF0000);
        expect("1. quad list, second quad", r, 4, 0x00FF00);
    }
    /* 2. Base vertex 4: blue, bottom left; vertices 0..3 are a yellow
     * corner that must not be drawn. */
    {
        static const int at[][2] = { { 1, 1 }, { 30, 30 }, { 1, 30 }, { 30, 1 } };
        static const WORD idx[6] = { 0, 1, 2, 0, 2, 3 };
        IDirect3DVertexBuffer8 *vb;
        IDirect3DIndexBuffer8 *ib;
        BYTE *p;
        V v[8];
        quad(v, -1, -1, -0.9f, -0.9f, YEL);
        quad(v + 4, -1, -1, 0, 0, BLU);
        dev->lpVtbl->CreateVertexBuffer(dev, sizeof v, 0, 0, 0, &vb);
        vb->lpVtbl->Lock(vb, 0, 0, &p, 0);
        memcpy(p, v, sizeof v);
        vb->lpVtbl->Unlock(vb);
        dev->lpVtbl->CreateIndexBuffer(dev, sizeof idx, 0, D3DFMT_INDEX16, 0, &ib);
        ib->lpVtbl->Lock(ib, 0, 0, &p, 0);
        memcpy(p, idx, sizeof idx);
        ib->lpVtbl->Unlock(ib);
        dev->lpVtbl->SetStreamSource(dev, 0, vb, sizeof(V));
        dev->lpVtbl->SetIndices(dev, ib, 4);
        dev->lpVtbl->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 4, 0, 2);
        expect("2. DrawIndexedPrimitive, base vertex 4", at, 4, 0x0000FF);
    }
    /* 3. MinIndex 4: yellow, bottom right. */
    {
        static const int at[][2] = { { 34, 1 }, { 62, 30 }, { 62, 1 }, { 34, 30 } };
        static const WORD idx[6] = { 4, 5, 6, 4, 6, 7 };
        V v[8];
        quad(v, 0.9f, -1, 1, -0.9f, RED);
        quad(v + 4, 0, -1, 1, 0, YEL);
        dev->lpVtbl->DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST, 4, 4, 2, idx,
                                            D3DFMT_INDEX16, v, sizeof(V));
        expect("3. DrawIndexedPrimitiveUP, MinIndex 4", at, 4, 0xFFFF00);
    }
    /* 4. One quad through indices, over the whole top half. */
    {
        static const int at[][2] = { { 2, 62 }, { 62, 34 }, { 2, 34 }, { 62, 62 } };
        static const WORD idx[4] = { 3, 2, 1, 0 };
        V v[4];
        quad(v, -1, 0, 1, 1, BLU);
        dev->lpVtbl->DrawIndexedPrimitiveUP(dev, D3DPT_QUADLIST, 0, 4, 1, idx,
                                            D3DFMT_INDEX16, v, sizeof(V));
        expect("4. indexed quad list", at, 4, 0x0000FF);
    }
    if (glGetError() != GL_NO_ERROR)
        s_fail = 1;
    if (!s_fail)
        puts("d3d8_gl: quad lists, base vertex and MinIndex: ok");
    return s_fail;
}
