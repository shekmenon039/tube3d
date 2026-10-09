// See vmesh.h.
#include "vmesh.h"
#include "motion.h"

#include <3ds.h>
#include <citro2d.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"   // the generated header tests __cplusplus
#include "vmesh_shbin.h"
#pragma GCC diagnostic pop

#define MAX_VW     (ME_MAX_BW + 1)
#define MAX_VH     (ME_MAX_BH + 1)
#define RING_DRAWS 48   // vertex sets in flight; one frame uses up to 4 (two meshes per eye)

typedef struct {
	float x, y, z, u, v;
} Vtx;

static DVLB_s* dvlb = NULL;
static shaderProgram_s prog;
static s8 loc_proj = -1;   // s8 because that's what shaderInstanceGetUniformLocation returns
static C3D_Mtx proj;
static u16* indices = NULL;   // in linear memory, two triangles per mesh cell
static int idx_vw = 0, idx_vh = 0, idx_count = 0;
static Vtx* ring = NULL;   // in linear memory, RING_DRAWS vertex sets of MAX_VW x MAX_VH
static int ring_pos = 0;
static bool ready = false;

bool vmesh_init(void)
{
	if (ready) return true;
	dvlb = DVLB_ParseFile((u32*)vmesh_shbin, vmesh_shbin_size);
	if (!dvlb) return false;
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	loc_proj = shaderInstanceGetUniformLocation(prog.vertexShader, "projection");
	if (loc_proj < 0)
	{
		vmesh_exit();
		return false;
	}
	indices = (u16*)linearAlloc((MAX_VW - 1) * (MAX_VH - 1) * 6 * sizeof(u16));
	ring = (Vtx*)linearAlloc((size_t)RING_DRAWS * MAX_VW * MAX_VH * sizeof(Vtx));
	if (!indices || !ring)
	{
		vmesh_exit();
		return false;
	}
	// The top screen as citro2d sets it up: 400 x 240 on a rotated framebuffer
	Mtx_OrthoTilt(&proj, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
	ready = true;
	return true;
}

void vmesh_exit(void)
{
	if (indices) linearFree(indices);
	if (ring) linearFree(ring);
	indices = NULL;
	ring = NULL;
	if (dvlb)
	{
		shaderProgramFree(&prog);
		DVLB_Free(dvlb);
		dvlb = NULL;
	}
	ready = false;
}

bool vmesh_ready(void) { return ready; }

static void build_indices(int vw, int vh)
{
	int count = 0;
	for (int y = 0; y + 1 < vh; y++)
		for (int x = 0; x + 1 < vw; x++)
		{
			u16 a = (u16)(y * vw + x), b = (u16)(a + 1), c = (u16)(a + vw), d = (u16)(c + 1);
			indices[count++] = a;
			indices[count++] = b;
			indices[count++] = c;
			indices[count++] = b;
			indices[count++] = d;
			indices[count++] = c;
		}
	idx_vw = vw;
	idx_vh = vh;
	idx_count = count;
	GSPGPU_FlushDataCache(indices, (u32)count * sizeof(u16));
}

void vmesh_draw(const VMeshDraw* draw)
{
	if (!ready || draw->vw < 2 || draw->vh < 2 || draw->vw > MAX_VW || draw->vh > MAX_VH) return;
	if (draw->vw != idx_vw || draw->vh != idx_vh) build_indices(draw->vw, draw->vh);

	Vtx* verts = ring + (size_t)ring_pos * MAX_VW * MAX_VH;
	ring_pos = (ring_pos + 1) % RING_DRAWS;
	for (int gy = 0; gy < draw->vh; gy++)
		for (int gx = 0; gx < draw->vw; gx++)
		{
			float px = draw->org_x + (gx == draw->vw - 1 ? draw->pic_w : gx * draw->cell);
			float py = draw->org_y + (gy == draw->vh - 1 ? draw->pic_h : gy * draw->cell);
			int i = gy * draw->vw + gx;
			Vtx* vert = &verts[i];
			vert->x = (px + draw->vx[i] * draw->s - draw->src_x) * draw->scale_x + draw->dst_x;
			vert->y = (py + draw->vy[i] * draw->s - draw->src_y) * draw->scale_y + draw->dst_y;
			vert->z = 0.5f;
			vert->u = px / draw->tex_w;
			vert->v = 1.0f - py / draw->tex_h;
		}
	GSPGPU_FlushDataCache(verts, (u32)(draw->vw * draw->vh) * sizeof(Vtx));

	C2D_Flush();   // draw citro2d's queued shapes first so the order is kept
	C3D_BindProgram(&prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc_proj, &proj);
	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);   // v0 = position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   // v1 = texcoord
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, verts, sizeof(Vtx), 2, 0x10);
	C3D_TexBind(0, draw->tex);
	// colour from the texture, opacity from the draw
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	float alpha = draw->alpha < 0 ? 0 : draw->alpha > 1 ? 1 : draw->alpha;
	C3D_TexEnvColor(env, (u32)(alpha * 255.0f + 0.5f) << 24);
	for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));   // pass the result through
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	// citro3d's default blending. citro2d relies on it, and C2D_Prepare doesn't restore it.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
	               GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DrawElements(GPU_TRIANGLES, idx_count, C3D_UNSIGNED_SHORT, indices);
	C2D_Prepare();   // put back citro2d's shader, attributes, texenv and blending
}
