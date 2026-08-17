/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_world.c: world model rendering

#include "quakedef.h"
#include "atomics.h"

extern cvar_t gl_fullbrights;
extern cvar_t r_drawflat;
extern cvar_t r_oldskyleaf;
extern cvar_t r_showtris;
extern cvar_t r_simd;
extern cvar_t gl_zfix;
extern cvar_t r_gpulightmapupdate;
extern cvar_t vid_filter;
extern cvar_t vid_palettize;

cvar_t r_parallelmark = {"r_parallelmark", "1", CVAR_NONE};

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

extern VkBuffer bmodel_vertex_buffer;
static int		world_texstart[NUM_WORLD_CBX];
static int		world_texend[NUM_WORLD_CBX];

#define MARK_SURFACE_CALLS_PER_WORKER (4)

/*
===============
mark_surfaces_state_t
===============
*/
typedef struct
{
#if defined(USE_SIMD)
#if defined(USE_SSE2)
	__m128 frustum_px[4];
	__m128 frustum_py[4];
	__m128 frustum_pz[4];
	__m128 frustum_pd[4];
	__m128 vieworg_px;
	__m128 vieworg_py;
	__m128 vieworg_pz;
#elif defined(USE_NEON)
	float32x4_t frustum_px[4];
	float32x4_t frustum_py[4];
	float32x4_t frustum_pz[4];
	float32x4_t frustum_pd[4];
	float32x4_t vieworg_px;
	float32x4_t vieworg_py;
	float32x4_t vieworg_pz;
#endif
	int frustum_ofsx[4];
	int frustum_ofsy[4];
	int frustum_ofsz[4];
#endif
	byte *vis;
} mark_surfaces_state_t;
mark_surfaces_state_t mark_surfaces_state;

//==============================================================================
//
// SETUP CHAINS
//
//==============================================================================

/*
================
R_ClearTextureChains -- ericw

clears texture chains for all textures used by the given model, and also
clears the lightmap chains
================
*/
void R_ClearTextureChains (qmodel_t *mod, texchain_t chain)
{
	int i;

	// set all chains to null
	for (i = 0; i < mod->numtextures; i++)
	{
		if (mod->textures[i])
		{
			mod->textures[i]->texturechains[chain] = NULL;
			mod->textures[i]->chain_size[chain] = 0;
		}
	}
}

/*
================
R_ChainSurface -- ericw -- adds the given surface to its texture chain
================
*/
void R_ChainSurface (msurface_t *surf, texchain_t chain)
{
	surf->texturechains[chain] = surf->texinfo->texture->texturechains[chain];
	surf->texinfo->texture->texturechains[chain] = surf;
	surf->texinfo->texture->chain_size[chain] += 1;
}

/*
================
R_BackFaceCull -- johnfitz -- returns true if the surface is facing away from vieworg
================
*/
static inline qboolean R_BackFaceCull (msurface_t *surf)
{
	double dot;

	if (surf->plane->type < 3)
		dot = r_refdef.vieworg[surf->plane->type] - surf->plane->dist;
	else
		dot = DotProduct (r_refdef.vieworg, surf->plane->normal) - surf->plane->dist;

	if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
		return true;

	return false;
}

/*
===============
R_SetupWorldCBXTexRanges
===============
*/
void R_SetupWorldCBXTexRanges (qboolean use_tasks)
{
	memset (world_texstart, 0, sizeof (world_texstart));
	memset (world_texend, 0, sizeof (world_texend));

	const int num_textures = cl.worldmodel->texofs[TEXTYPE_SKY];
	if (!use_tasks)
	{
		world_texstart[0] = 0;
		world_texend[0] = num_textures;
		return;
	}

	int total_world_surfs = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[cl.worldmodel->usedtextures[i]];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & SURF_DRAWTILED)
			continue;
		total_world_surfs += t->chain_size[chain_world];
	}

	const int num_surfs_per_cbx = (total_world_surfs + NUM_WORLD_CBX - 1) / NUM_WORLD_CBX;
	int		  current_cbx = 0;
	int		  num_assigned_to_cbx = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[cl.worldmodel->usedtextures[i]];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & SURF_DRAWTILED)
			continue;

		assert (current_cbx < NUM_WORLD_CBX);
		// TODO : quick hack to shutup MSYS2 error:
		// error: array subscript 6 is above array bounds of 'int[6]' [-Werror=array-bounds=]
		// world_texend[current_cbx] = i + 1;
		// ==> potential bug or zealous compiler triggered by assert (current_cbx < NUM_WORLD_CBX); ?
		if (current_cbx >= NUM_WORLD_CBX)
			break;

		world_texend[current_cbx] = i + 1;
		num_assigned_to_cbx += t->chain_size[chain_world];
		if (num_assigned_to_cbx >= num_surfs_per_cbx)
		{
			current_cbx += 1;
			if (current_cbx < NUM_WORLD_CBX)
			{
				world_texstart[current_cbx] = i + 1;
			}

			num_assigned_to_cbx = 0;
		}
	}
}

#if defined(USE_SSE2)
/*
===============
R_BackFaceCullSIMD

Performs backface culling for 32 planes
===============
*/
static FORCE_INLINE uint32_t R_BackFaceCullSIMD (soa_plane_t *planes)
{
	__m128 px = mark_surfaces_state.vieworg_px;
	__m128 py = mark_surfaces_state.vieworg_py;
	__m128 pz = mark_surfaces_state.vieworg_pz;

	uint32_t activelanes = 0;
	for (int plane_index = 0; plane_index < 4; ++plane_index)
	{
		soa_plane_t *plane = planes + plane_index;

		__m128 v0 = _mm_mul_ps (_mm_loadu_ps ((*plane) + 0), px);
		__m128 v1 = _mm_mul_ps (_mm_loadu_ps ((*plane) + 4), px);

		v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*plane) + 8), py));
		v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*plane) + 12), py));

		v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*plane) + 16), pz));
		v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*plane) + 20), pz));

		__m128 pd0 = _mm_loadu_ps ((*plane) + 24);
		__m128 pd1 = _mm_loadu_ps ((*plane) + 28);

		uint32_t plane_lanes = (uint32_t)(_mm_movemask_ps (_mm_cmplt_ps (pd0, v0)) | (_mm_movemask_ps (_mm_cmplt_ps (pd1, v1)) << 4));
		activelanes |= plane_lanes << (plane_index * 8);
	}
	return activelanes;
}

/*
===============
R_CullBoxSIMD

Performs frustum culling for 32 bounding boxes
===============
*/
static FORCE_INLINE uint32_t R_CullBoxSIMD (soa_aabb_t *boxes, uint32_t activelanes)
{
	for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
	{
		if (activelanes == 0)
			break;

		int	   ofsx = mark_surfaces_state.frustum_ofsx[frustum_index];
		int	   ofsy = mark_surfaces_state.frustum_ofsy[frustum_index];
		int	   ofsz = mark_surfaces_state.frustum_ofsz[frustum_index];
		__m128 px = mark_surfaces_state.frustum_px[frustum_index];
		__m128 py = mark_surfaces_state.frustum_py[frustum_index];
		__m128 pz = mark_surfaces_state.frustum_pz[frustum_index];
		__m128 pd = mark_surfaces_state.frustum_pd[frustum_index];

		uint32_t frustum_lanes = 0;
		for (int boxes_index = 0; boxes_index < 4; ++boxes_index)
		{
			soa_aabb_t *box = boxes + boxes_index;
			__m128		v0 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx), px);
			__m128		v1 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx + 4), px);
			v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsy), py));
			v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsy + 4), py));
			v0 = _mm_add_ps (v0, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsz), pz));
			v1 = _mm_add_ps (v1, _mm_mul_ps (_mm_loadu_ps ((*box) + ofsz + 4), pz));
			frustum_lanes |= (uint32_t)(_mm_movemask_ps (_mm_cmplt_ps (pd, v0)) | (_mm_movemask_ps (_mm_cmplt_ps (pd, v1)) << 4)) << (boxes_index * 8);
		}
		activelanes &= frustum_lanes;
	}

	return activelanes;
}
#elif defined(USE_NEON)
static FORCE_INLINE uint32_t NeonMoveMask (uint32x4_t input)
{
	static const int32x4_t shift = {0, 1, 2, 3};
	return vaddvq_u32 (vshlq_u32 (vshrq_n_u32 (input, 31), shift));
}

/*
===============
R_BackFaceCullSIMD

Performs backface culling for 32 planes
===============
*/
static FORCE_INLINE uint32_t R_BackFaceCullSIMD (soa_plane_t *planes)
{
	float32x4_t px = mark_surfaces_state.vieworg_px;
	float32x4_t py = mark_surfaces_state.vieworg_py;
	float32x4_t pz = mark_surfaces_state.vieworg_pz;

	uint32_t activelanes = 0;
	for (int plane_index = 0; plane_index < 4; ++plane_index)
	{
		soa_plane_t *plane = planes + plane_index;

		float32x4_t v0 = vmulq_f32 (vld1q_f32 ((*plane) + 0), px);
		float32x4_t v1 = vmulq_f32 (vld1q_f32 ((*plane) + 4), px);

		v0 = vmlaq_f32 (v0, vld1q_f32 ((*plane) + 8), py);
		v1 = vmlaq_f32 (v1, vld1q_f32 ((*plane) + 12), py);

		v0 = vmlaq_f32 (v0, vld1q_f32 ((*plane) + 16), pz);
		v1 = vmlaq_f32 (v1, vld1q_f32 ((*plane) + 20), pz);

		float32x4_t pd0 = vld1q_f32 ((*plane) + 24);
		float32x4_t pd1 = vld1q_f32 ((*plane) + 28);

		uint32_t plane_lanes = (uint32_t)(NeonMoveMask (vcltq_f32 (pd0, v0)) | (NeonMoveMask (vcltq_f32 (pd1, v1)) << 4));
		activelanes |= plane_lanes << (plane_index * 8);
	}
	return activelanes;
}

/*
===============
R_CullBoxSIMD

Performs frustum culling for 32 bounding boxes
===============
*/
static FORCE_INLINE uint32_t R_CullBoxSIMD (soa_aabb_t *boxes, uint32_t activelanes)
{
	for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
	{
		if (activelanes == 0)
			break;

		int			ofsx = mark_surfaces_state.frustum_ofsx[frustum_index];
		int			ofsy = mark_surfaces_state.frustum_ofsy[frustum_index];
		int			ofsz = mark_surfaces_state.frustum_ofsz[frustum_index];
		float32x4_t px = mark_surfaces_state.frustum_px[frustum_index];
		float32x4_t py = mark_surfaces_state.frustum_py[frustum_index];
		float32x4_t pz = mark_surfaces_state.frustum_pz[frustum_index];
		float32x4_t pd = mark_surfaces_state.frustum_pd[frustum_index];

		uint32_t frustum_lanes = 0;
		for (int boxes_index = 0; boxes_index < 4; ++boxes_index)
		{
			soa_aabb_t *box = boxes + boxes_index;
			float32x4_t v0 = vmulq_f32 (vld1q_f32 ((*box) + ofsx), px);
			float32x4_t v1 = vmulq_f32 (vld1q_f32 ((*box) + ofsx + 4), px);
			v0 = vmlaq_f32 (v0, vld1q_f32 ((*box) + ofsy), py);
			v1 = vmlaq_f32 (v1, vld1q_f32 ((*box) + ofsy + 4), py);
			v0 = vmlaq_f32 (v0, vld1q_f32 ((*box) + ofsz), pz);
			v1 = vmlaq_f32 (v1, vld1q_f32 ((*box) + ofsz + 4), pz);
			frustum_lanes |= (uint32_t)(NeonMoveMask (vcltq_f32 (pd, v0)) | (NeonMoveMask (vcltq_f32 (pd, v1)) << 4)) << (boxes_index * 8);
		}
		activelanes &= frustum_lanes;
	}

	return activelanes;
}
#endif

#if defined(USE_SIMD)
/*
===============
R_MarkVisSurfacesSIMD
===============
*/
void R_MarkVisSurfacesSIMD (qboolean *use_tasks)
{
	msurface_t	*surf;
	unsigned int i, k;
	unsigned int numleafs = cl.worldmodel->numleafs;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t	*vis = (uint32_t *)mark_surfaces_state.vis;
	uint32_t	*surfvis = (uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t	*leafbounds = cl.worldmodel->soa_leafbounds;

	int current_combined_dep_index = INT_MAX;

	const bool cst_r_drawworld_cheatsafe = r_drawworld_cheatsafe;
	const bool cst_indirect = indirect;
	const bool cst_r_oldskyleaf = (r_oldskyleaf.value > 0.0f);
	const bool cst_r_gpulightmapupdate = (r_gpulightmapupdate.value > 0.0f);

	// iterate through leaves, marking surfaces
	for (i = 0; i < numleafs; i += 32)
	{
		uint32_t mask = vis[i / 32];
		if (mask == 0)
			continue;

		mask = R_CullBoxSIMD (&leafbounds[i / 8], mask);
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);

			mleaf_t *leaf = &cl.worldmodel->leafs[1 + i + j];
			if (cst_r_drawworld_cheatsafe && (leaf->contents != CONTENTS_SKY || cst_r_oldskyleaf))
			{
				unsigned int nummarksurfaces = leaf->nummarksurfaces;
				int			*marksurfaces = leaf->firstmarksurface;
				for (k = 0; k < nummarksurfaces; ++k)
				{
					unsigned int index = marksurfaces[k];
					surfvis[index / 32] |= 1u << (index % 32);
				}

				if (cst_indirect && current_combined_dep_index != leaf->combined_deps)
				{
					R_MarkDeps (leaf->combined_deps, 0);
					current_combined_dep_index = leaf->combined_deps;
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

	if (cst_indirect)
		return;

	uint32_t brushpolys = 0;
	for (i = 0; i < numsurfaces; i += 32)
	{
		uint32_t mask = surfvis[i / 32];
		if (mask == 0)
			continue;

		mask &= R_BackFaceCullSIMD (&cl.worldmodel->soa_surfplanes[i / 8]);
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);

			surf = &cl.worldmodel->surfaces[i + j];
			++brushpolys;
			R_ChainSurface (surf, chain_world);
			if (!cst_r_gpulightmapupdate)
				R_RenderDynamicLightmaps (surf);
			else if (surf->lightmaptexturenum >= 0)
				lightmaps[surf->lightmaptexturenum].modified[0] |= surf->styles_bitmap;
			if (surf->texinfo->texture->warpimage)
				Atomic_StoreUInt32_Relaxed (&surf->texinfo->texture->update_warp, true);
		}
	}

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}

/*
===============
R_MarkLeafsSIMD
===============
*/
void R_MarkLeafsSIMD (int index, void *unused)
{
	const int batch_index = index;

	// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER
	const int nb_batchs = MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers ();

	const int numleafs = cl.worldmodel->numleafs;

	const int nominal_nb_32leaf_in_batch = ((numleafs + 31) / 32) / nb_batchs;

	// the last batch gather the reminder
	const int nb_32leaf_in_batch =
		((batch_index != nb_batchs - 1) ? nominal_nb_32leaf_in_batch : ((numleafs + 31) / 32) - (nb_batchs - 1) * nominal_nb_32leaf_in_batch);

	atomic_uint32_t *surfvis = (atomic_uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t		*leafbounds = cl.worldmodel->soa_leafbounds;
	uint32_t		*vis = (uint32_t *)mark_surfaces_state.vis;

	unsigned int current_surfvis_index_written = 0;
	uint32_t	 current_surfvis_written = 0;
	int			 current_combined_dep_index = INT_MAX;

	const bool cst_r_drawworld_cheatsafe = r_drawworld_cheatsafe;
	const bool cst_indirect = indirect;
	const bool cst_r_oldskyleaf = (r_oldskyleaf.value > 0.0f);
	const int  worker_index = Tasks_GetWorkerIndex ();

	for (int k = 0; k < nb_32leaf_in_batch; k++)
	{
		const unsigned int index_32leaf = batch_index * nominal_nb_32leaf_in_batch + k;

		const unsigned int first_leaf = index_32leaf * 32 + 1;

		uint32_t *mask = &vis[index_32leaf];

		if (*mask == 0)
			continue;

		*mask = R_CullBoxSIMD (&leafbounds[index_32leaf * 4], *mask);

		uint32_t mask_iter = *mask;

		while (mask_iter != 0)
		{
			const int i = FindFirstBitNonZero (mask_iter);

			mleaf_t *leaf = &cl.worldmodel->leafs[first_leaf + i];

			if (cst_r_drawworld_cheatsafe && (leaf->contents != CONTENTS_SKY || cst_r_oldskyleaf))
			{
				unsigned int nummarksurfaces = leaf->nummarksurfaces;
				int			*marksurfaces = leaf->firstmarksurface;

				for (unsigned int j = 0; j < nummarksurfaces; ++j)
				{
					const unsigned int surf_index = marksurfaces[j];

					if (surf_index / 32 != current_surfvis_index_written)
					{
						Atomic_OrUInt32_Relaxed (&surfvis[current_surfvis_index_written], current_surfvis_written);
						current_surfvis_index_written = surf_index / 32;
						current_surfvis_written = 0;
					}
					current_surfvis_written |= 1u << (surf_index % 32);
				}

				if (cst_indirect && current_combined_dep_index != leaf->combined_deps)
				{
					R_MarkDeps (leaf->combined_deps, worker_index);
					current_combined_dep_index = leaf->combined_deps;
				}
			}
			const uint32_t bit_mask = ~(1u << i);
			if (!leaf->efrags)
			{
				*mask &= bit_mask;
			}
			mask_iter &= bit_mask;
		}
	}

	Atomic_OrUInt32 (&surfvis[current_surfvis_index_written], current_surfvis_written);
}

/*
===============
R_BackfaceCullSurfacesSIMD
===============
*/
void R_BackfaceCullSurfacesSIMD (int index, void *unused)
{
	uint32_t *surfvis = (uint32_t *)cl.worldmodel->surfvis;

	const int batch_index = index;

	// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER
	const int nb_batchs = MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers ();

	const unsigned int numsurfaces = cl.worldmodel->numsurfaces;

	const int nominal_nb_32surface_in_batch = ((numsurfaces + 31) / 32) / nb_batchs;

	// the last batch gather the reminder
	const int nb_32surface_in_batch =
		((batch_index != nb_batchs - 1) ? nominal_nb_32surface_in_batch : ((numsurfaces + 31) / 32) - (nb_batchs - 1) * nominal_nb_32surface_in_batch);

	const int worker_index = Tasks_GetWorkerIndex ();

	for (int k = 0; k < nb_32surface_in_batch; k++)
	{
		const unsigned int index_32surf = batch_index * nominal_nb_32surface_in_batch + k;

		uint32_t *mask = &surfvis[index_32surf];

		if (*mask == 0)
			continue;

		*mask &= R_BackFaceCullSIMD (&cl.worldmodel->soa_surfplanes[index_32surf * 4]);

		uint32_t mask_iter = *mask;

		while (mask_iter != 0)
		{
			const int i = FindFirstBitNonZero (mask_iter);

			msurface_t *surf = &cl.worldmodel->surfaces[(index_32surf * 32) + i];

			if (surf->lightmaptexturenum >= 0)
				lightmaps[surf->lightmaptexturenum].modified[worker_index] |= surf->styles_bitmap;
			if (surf->texinfo->texture->warpimage)
				Atomic_StoreUInt32_Relaxed (&surf->texinfo->texture->update_warp, true);

			const uint32_t bit_mask = ~(1u << i);
			mask_iter &= bit_mask;
		}
	}
}
#endif // defined(USE_SIMD)

/*
===============
R_StoreLeafEFrags
===============
*/
void R_StoreLeafEFrags (void *unused)
{
	unsigned int i;
	unsigned int numleafs = cl.worldmodel->numleafs;
	uint32_t	*vis = (uint32_t *)mark_surfaces_state.vis;
	for (i = 0; i < numleafs; i += 32)
	{
		uint32_t mask = vis[i / 32];
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);
			mleaf_t *leaf = &cl.worldmodel->leafs[1 + i + j];
			R_StoreEfrags (&leaf->efrags);
		}
	}
}

/*
===============
R_ChainVisSurfaces
===============
*/
void R_ChainVisSurfaces (qboolean *use_tasks)
{
	unsigned int i;
	msurface_t	*surf;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t	*surfvis = (uint32_t *)cl.worldmodel->surfvis;
	uint32_t	 brushpolys = 0;
	for (i = 0; i < numsurfaces; i += 32)
	{
		uint32_t mask = surfvis[i / 32];
		while (mask != 0)
		{
			const int j = FindFirstBitNonZero (mask);
			mask &= ~(1u << j);
			surf = &cl.worldmodel->surfaces[i + j];
			++brushpolys;
			R_ChainSurface (surf, chain_world);
		}
	}

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}

/*
===============
R_GetTransparentWaterTypes
===============
*/
static int R_GetTransparentWaterTypes ()
{
	int types = 0;
	if ((map_lavaalpha > 0 ? map_lavaalpha : map_fallbackalpha) != 1)
		types |= SURF_DRAWLAVA;
	if ((map_telealpha > 0 ? map_telealpha : map_fallbackalpha) != 1)
		types |= SURF_DRAWTELE;
	if ((map_slimealpha > 0 ? map_slimealpha : map_fallbackalpha) != 1)
		types |= SURF_DRAWSLIME;
	if (map_wateralpha != 1)
		types |= SURF_DRAWWATER;
	return types;
}

/*
===============
R_PrepareTransparentWaterSurfList
===============
*/
static void R_PrepareTransparentWaterSurfList ()
{
	int types = R_GetTransparentWaterTypes ();
	if (cl.worldmodel->water_surfs_specials != types)
	{
		if (!cl.worldmodel->water_surfs)
			cl.worldmodel->water_surfs = Mem_Realloc (cl.worldmodel->water_surfs, 8192 * sizeof (int));
		cl.worldmodel->used_water_surfs = 0;

		for (int i = 0; i < cl.worldmodel->numsurfaces; i++)
			if (cl.worldmodel->surfaces[i].flags & types)
			{
				if (cl.worldmodel->used_water_surfs >= 8192 && !(cl.worldmodel->used_water_surfs & (cl.worldmodel->used_water_surfs - 1)))
					cl.worldmodel->water_surfs = Mem_Realloc (cl.worldmodel->water_surfs, cl.worldmodel->used_water_surfs * 2 * sizeof (int));
				cl.worldmodel->water_surfs[cl.worldmodel->used_water_surfs] = i;
				++cl.worldmodel->used_water_surfs;
			}

		cl.worldmodel->water_surfs_specials = types;
	}
}

/*
===============
R_ChainVisSurfaces_TransparentWater
===============
*/
static void R_ChainVisSurfaces_TransparentWater ()
{
	R_PrepareTransparentWaterSurfList ();
	uint32_t *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	for (int i = 0; i < cl.worldmodel->used_water_surfs; i++)
	{
		int j = cl.worldmodel->water_surfs[i];
		if (surfvis[j / 32] & 1 << j % 32 && !R_BackFaceCull (&cl.worldmodel->surfaces[j]))
			R_ChainSurface (&cl.worldmodel->surfaces[j], chain_world);
	}
}

/*
===============
R_MarkLeafsParallel
===============
*/
void R_MarkLeafsParallel (int index, void *unused)
{
	const int batch_index = index;

	// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER
	const int nb_batchs = MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers ();

	const int numleafs = cl.worldmodel->numleafs;

	const int nominal_nb_32leaf_in_batch = ((numleafs + 31) / 32) / nb_batchs;

	// the last batch gather the reminder
	const int nb_32leaf_in_batch =
		((batch_index != nb_batchs - 1) ? nominal_nb_32leaf_in_batch : ((numleafs + 31) / 32) - (nb_batchs - 1) * nominal_nb_32leaf_in_batch);

	atomic_uint32_t *surfvis = (atomic_uint32_t *)cl.worldmodel->surfvis;
	uint32_t		*vis = (uint32_t *)mark_surfaces_state.vis;

	unsigned int current_surfvis_index_written = 0;
	uint32_t	 current_surfvis_written = 0;
	int			 current_combined_dep_index = INT_MAX;

	const bool cst_r_drawworld_cheatsafe = r_drawworld_cheatsafe;
	const bool cst_indirect = indirect;
	const bool cst_r_oldskyleaf = (r_oldskyleaf.value > 0.0f);
	const int  worker_index = Tasks_GetWorkerIndex ();

	for (int k = 0; k < nb_32leaf_in_batch; k++)
	{
		const unsigned int index_32leaf = batch_index * nominal_nb_32leaf_in_batch + k;

		const unsigned int first_leaf = index_32leaf * 32 + 1;

		uint32_t *mask = &vis[index_32leaf];

		if (*mask == 0)
			continue;

		uint32_t mask_iter = *mask;

		while (mask_iter != 0)
		{
			const int i = FindFirstBitNonZero (mask_iter);

			const uint32_t bit_mask = ~(1u << i);

			mask_iter &= bit_mask;

			mleaf_t *leaf = &cl.worldmodel->leafs[first_leaf + i];

			if (R_CullBox (leaf->minmaxs, leaf->minmaxs + 3))
			{
				*mask &= bit_mask;
				continue;
			}
			if (!leaf->efrags)
				*mask &= bit_mask;

			if (cst_r_drawworld_cheatsafe && (leaf->contents != CONTENTS_SKY || cst_r_oldskyleaf))
			{
				unsigned int nummarksurfaces = leaf->nummarksurfaces;
				int			*marksurfaces = leaf->firstmarksurface;

				for (unsigned int j = 0; j < nummarksurfaces; ++j)
				{
					unsigned int surf_index = marksurfaces[j];

					if (surf_index / 32 != current_surfvis_index_written)
					{
						Atomic_OrUInt32_Relaxed (&surfvis[current_surfvis_index_written], current_surfvis_written);
						current_surfvis_index_written = surf_index / 32;
						current_surfvis_written = 0;
					}
					current_surfvis_written |= 1u << (surf_index % 32);
				}

				if (cst_indirect && current_combined_dep_index != leaf->combined_deps)
				{
					R_MarkDeps (leaf->combined_deps, worker_index);
					current_combined_dep_index = leaf->combined_deps;
				}
			}
		}
	}

	Atomic_OrUInt32 (&surfvis[current_surfvis_index_written], current_surfvis_written);
}

/*
===============
R_BackfaceCullSurfacesParallel
===============
*/
void R_BackfaceCullSurfacesParallel (int index, void *unused)
{
	uint32_t *surfvis = (uint32_t *)cl.worldmodel->surfvis;

	const int batch_index = index;

	// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER
	const int nb_batchs = MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers ();

	const unsigned int numsurfaces = cl.worldmodel->numsurfaces;

	const int nominal_nb_32surface_in_batch = ((numsurfaces + 31) / 32) / nb_batchs;

	// the last batch gather the reminder
	const int nb_32surface_in_batch =
		((batch_index != nb_batchs - 1) ? nominal_nb_32surface_in_batch : ((numsurfaces + 31) / 32) - (nb_batchs - 1) * nominal_nb_32surface_in_batch);

	const int worker_index = Tasks_GetWorkerIndex ();

	for (int k = 0; k < nb_32surface_in_batch; k++)
	{
		const unsigned int index_32surf = batch_index * nominal_nb_32surface_in_batch + k;

		uint32_t *mask = &surfvis[index_32surf];

		if (*mask == 0)
			continue;

		uint32_t mask_iter = *mask;

		while (mask_iter != 0)
		{
			const int	   i = FindFirstBitNonZero (mask_iter);
			const uint32_t bit_mask = ~(1u << i);
			mask_iter &= bit_mask;

			msurface_t *surf = &cl.worldmodel->surfaces[(index_32surf * 32) + i];

			if (R_BackFaceCull (surf))
				*surfvis &= bit_mask;
			else
			{
				if (surf->lightmaptexturenum >= 0)
					lightmaps[surf->lightmaptexturenum].modified[worker_index] |= surf->styles_bitmap;
				if (surf->texinfo->texture->warpimage)
					Atomic_StoreUInt32_Relaxed (&surf->texinfo->texture->update_warp, true);
			}
		}
	}
}

/*
===============
R_MarkVisSurfaces
===============
*/
void R_MarkVisSurfaces (qboolean *use_tasks)
{
	int			i, j;
	msurface_t *surf;
	mleaf_t	   *leaf;
	uint32_t	brushpolys = 0;
	uint32_t   *vis = (uint32_t *)mark_surfaces_state.vis;
	uint32_t   *surfvis = (uint32_t *)cl.worldmodel->surfvis;

	int current_combined_dep_index = INT_MAX;

	const bool cst_r_drawworld_cheatsafe = r_drawworld_cheatsafe;
	const bool cst_indirect = indirect;
	const bool cst_r_oldskyleaf = (r_oldskyleaf.value > 0.0f);
	const bool cst_r_gpulightmapupdate = (r_gpulightmapupdate.value > 0.0f);

	leaf = &cl.worldmodel->leafs[1];
	for (i = 0; i < cl.worldmodel->numleafs; i++, leaf++)
	{
		if (vis[i / 32] & (1u << (i % 32)))
		{
			if (R_CullBox (leaf->minmaxs, leaf->minmaxs + 3))
				continue;

			if (cst_r_drawworld_cheatsafe && (leaf->contents != CONTENTS_SKY || cst_r_oldskyleaf))
			{
				if (cst_indirect && current_combined_dep_index != leaf->combined_deps)
				{
					R_MarkDeps (leaf->combined_deps, 0);
					current_combined_dep_index = leaf->combined_deps;
				}

				for (j = 0; j < leaf->nummarksurfaces; j++)
				{
					if (cst_indirect)
					{
						unsigned int surf_index = leaf->firstmarksurface[j];
						surfvis[surf_index / 32] |= 1u << (surf_index % 32);
						continue;
					}
					surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[j]];
					if (surf->visframe != r_visframecount)
					{
						surf->visframe = r_visframecount;
						if (!R_BackFaceCull (surf))
						{
							++brushpolys;
							R_ChainSurface (surf, chain_world);
							if (!cst_r_gpulightmapupdate)
								R_RenderDynamicLightmaps (surf);
							else if (surf->lightmaptexturenum >= 0)
								lightmaps[surf->lightmaptexturenum].modified[0] |= surf->styles_bitmap;
							if (surf->texinfo->texture->warpimage)
								Atomic_StoreUInt32_Relaxed (&surf->texinfo->texture->update_warp, true);
						}
					}
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

	if (cst_indirect)
		return;

	Atomic_AddUInt32 (&rs_brushpolys, brushpolys); // count wpolys here
	R_SetupWorldCBXTexRanges (*use_tasks);
}

/*
===============
R_MarkSurfacesPrepare
===============
*/
static void R_MarkSurfacesPrepare (void *unused)
{
	int		 i;
	qboolean nearwaterportal;
	int		 numleafs = cl.worldmodel->numleafs;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i = 0; i < r_viewleaf->nummarksurfaces; i++)
		if (cl.worldmodel->surfaces[r_viewleaf->firstmarksurface[i]].flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		mark_surfaces_state.vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		mark_surfaces_state.vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		mark_surfaces_state.vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	uint32_t *vis = (uint32_t *)mark_surfaces_state.vis;
	if ((numleafs % 32) != 0)
		vis[numleafs / 32] &= (1u << (numleafs % 32)) - 1;

	r_visframecount++;

	// set all chains to null
	for (i = 0; i < cl.worldmodel->numtextures; i++)
		if (cl.worldmodel->textures[i])
		{
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;
			cl.worldmodel->textures[i]->chain_size[chain_world] = 0;
		}

#if defined(USE_SIMD)
	if (use_simd)
	{
		memset (cl.worldmodel->surfvis, 0, (cl.worldmodel->numsurfaces + 31) / 8);
#if defined(USE_SSE2)
		for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
		{
			mplane_t *p = frustum + frustum_index;
			byte	  signbits = p->signbits;
			__m128	  vplane = _mm_loadu_ps (p->normal);
			mark_surfaces_state.frustum_ofsx[frustum_index] = signbits & 1 ? 0 : 8;	  // x min/max
			mark_surfaces_state.frustum_ofsy[frustum_index] = signbits & 2 ? 16 : 24; // y min/max
			mark_surfaces_state.frustum_ofsz[frustum_index] = signbits & 4 ? 32 : 40; // z min/max
			mark_surfaces_state.frustum_px[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (0, 0, 0, 0));
			mark_surfaces_state.frustum_py[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (1, 1, 1, 1));
			mark_surfaces_state.frustum_pz[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (2, 2, 2, 2));
			mark_surfaces_state.frustum_pd[frustum_index] = _mm_shuffle_ps (vplane, vplane, _MM_SHUFFLE (3, 3, 3, 3));
		}
		__m128 pos = _mm_loadu_ps (r_refdef.vieworg);
		mark_surfaces_state.vieworg_px = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (0, 0, 0, 0));
		mark_surfaces_state.vieworg_py = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (1, 1, 1, 1));
		mark_surfaces_state.vieworg_pz = _mm_shuffle_ps (pos, pos, _MM_SHUFFLE (2, 2, 2, 2));
#elif defined(USE_NEON)
		for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
		{
			mplane_t *p = frustum + frustum_index;
			byte	  signbits = p->signbits;
			mark_surfaces_state.frustum_ofsx[frustum_index] = signbits & 1 ? 0 : 8;	  // x min/max
			mark_surfaces_state.frustum_ofsy[frustum_index] = signbits & 2 ? 16 : 24; // y min/max
			mark_surfaces_state.frustum_ofsz[frustum_index] = signbits & 4 ? 32 : 40; // z min/max
			mark_surfaces_state.frustum_px[frustum_index] = vdupq_n_f32 (p->normal[0]);
			mark_surfaces_state.frustum_py[frustum_index] = vdupq_n_f32 (p->normal[1]);
			mark_surfaces_state.frustum_pz[frustum_index] = vdupq_n_f32 (p->normal[2]);
			mark_surfaces_state.frustum_pd[frustum_index] = vdupq_n_f32 (p->dist);
		}
		mark_surfaces_state.vieworg_px = vdupq_n_f32 (r_refdef.vieworg[0]);
		mark_surfaces_state.vieworg_py = vdupq_n_f32 (r_refdef.vieworg[1]);
		mark_surfaces_state.vieworg_pz = vdupq_n_f32 (r_refdef.vieworg[2]);
#endif
	}
	else
#endif
	{
		if (r_parallelmark.value || indirect)
			memset (cl.worldmodel->surfvis, 0, (cl.worldmodel->numsurfaces + 31) / 8);
	}
}

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (qboolean use_tasks, task_handle_t before_mark, task_handle_t *store_efrags, task_handle_t *cull_surfaces, task_handle_t *chain_surfaces)
{
	if (use_tasks)
	{
		task_handle_t prepare_mark = Task_AllocateAndAssignFunc (R_MarkSurfacesPrepare, NULL, 0);
		Task_AddDependency (before_mark, prepare_mark);
		Task_Submit (prepare_mark);
		if (r_parallelmark.value)
		{
			task_handle_t mark_surfaces;
#if defined(USE_SIMD)
			// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER:
			if (use_simd)
				mark_surfaces = Task_AllocateAndAssignIndexedFunc (R_MarkLeafsSIMD, MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers (), NULL, 0);
			else
#endif
				mark_surfaces = Task_AllocateAndAssignIndexedFunc (R_MarkLeafsParallel, MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers (), NULL, 0);
			Task_AddDependency (prepare_mark, mark_surfaces);
			Task_Submit (mark_surfaces);

			*store_efrags = Task_AllocateAndAssignFunc (R_StoreLeafEFrags, NULL, 0);
			Task_AddDependency (mark_surfaces, *store_efrags);

			if (!indirect && r_drawworld_cheatsafe)
			{
#if defined(USE_SIMD)
				// split processing in as many as there are workers and by MARK_SURFACE_CALLS_PER_WORKER:
				if (use_simd)
					*cull_surfaces =
						Task_AllocateAndAssignIndexedFunc (R_BackfaceCullSurfacesSIMD, MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers (), NULL, 0);
				else
#endif
					*cull_surfaces =
						Task_AllocateAndAssignIndexedFunc (R_BackfaceCullSurfacesParallel, MARK_SURFACE_CALLS_PER_WORKER * Tasks_NumWorkers (), NULL, 0);
				Task_AddDependency (mark_surfaces, *cull_surfaces);

				*chain_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_ChainVisSurfaces, &use_tasks, sizeof (qboolean));
				Task_AddDependency (*cull_surfaces, *chain_surfaces);
			}
			else // indirect
			{
				*cull_surfaces = mark_surfaces;
				*chain_surfaces = mark_surfaces;
			}
		}
		else
		{
			task_handle_t mark_surfaces;
#if defined(USE_SIMD)
			if (use_simd)
				mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfacesSIMD, &use_tasks, sizeof (qboolean));
			else
#endif
				mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfaces, &use_tasks, sizeof (qboolean));
			Task_AddDependency (prepare_mark, mark_surfaces);
			*store_efrags = mark_surfaces;
			*chain_surfaces = mark_surfaces;
			*cull_surfaces = mark_surfaces;
		}
	}
	else
	{
		R_MarkSurfacesPrepare (NULL);
		// iterate through leaves, marking surfaces
#if defined(USE_SIMD)
		if (use_simd)
		{
			R_MarkVisSurfacesSIMD (&use_tasks);
		}
		else
#endif
			R_MarkVisSurfaces (&use_tasks);
	}
}

//==============================================================================
//
// VBO SUPPORT
//
//==============================================================================

static unsigned int R_NumTriangleIndicesForSurf (msurface_t *s)
{
	return 3 * (s->numedges - 2);
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (msurface_t *s, uint32_t *dest)
{
	int i;
	for (i = 2; i < s->numedges; i++)
	{
		*dest++ = s->vbo_firstvert;
		*dest++ = s->vbo_firstvert + i - 1;
		*dest++ = s->vbo_firstvert + i;
	}
}

/*
================
R_ClearBatch
================
*/
static void R_ClearBatch (cb_context_t *cbx)
{
	cbx->num_vbo_indices = 0;
}

/*
================
R_FlushBatch

Draw the current batch if non-empty and clears it, ready for more R_BatchSurface calls.
================
*/
static void R_FlushBatch (
	cb_context_t *cbx, qboolean fullbright_enabled, qboolean alpha_test, qboolean alpha_blend, qboolean use_zbias, gltexture_t *lightmap_texture,
	uint32_t *brushpasses)
{
	if (cbx->num_vbo_indices > 0)
	{
		int pipeline_index =
			(fullbright_enabled ? 1 : 0) + (alpha_test ? 2 : 0) + (alpha_blend ? 4 : 0) + (vid_filter.value != 0 && vid_palettize.value != 0 ? 8 : 0);
		vulkan_pipeline_t pipeline = R_PipelineForRenderPass (
			cbx->render_pass_index, vulkan_globals.world_pipelines[R_MainPassPipelineVariant (cbx->render_pass_index)][pipeline_index],
			vulkan_globals.world_wboit_pipelines[pipeline_index], vulkan_globals.world_mboit_moment_pipelines[pipeline_index],
			vulkan_globals.world_mboit_composite_pipelines[pipeline_index]);
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		float constant_factor = 0.0f, slope_factor = 0.0f;
		if (use_zbias)
		{
			if (vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT)
			{
				constant_factor = -4.f;
				slope_factor = -0.125f;
			}
			else
			{
				constant_factor = -1.f;
				slope_factor = -0.25f;
			}
		}
		vkCmdSetDepthBias (cbx->cb, constant_factor, 0.0f, slope_factor);

		if (!r_fullbright_cheatsafe)
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 1, 1, &lightmap_texture->descriptor_set, 0, NULL);
		else
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 1, 1, &greylightmap->descriptor_set, 0, NULL);

		VkBuffer	 buffer;
		VkDeviceSize buffer_offset;
		byte		*indices = R_IndexAllocate (cbx->num_vbo_indices * sizeof (uint32_t), &buffer, &buffer_offset);
		memcpy (indices, cbx->vbo_indices, cbx->num_vbo_indices * sizeof (uint32_t));

		vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, buffer, buffer_offset, VK_INDEX_TYPE_UINT32);
		vulkan_globals.vk_cmd_draw_indexed (cbx->cb, cbx->num_vbo_indices, 1, 0, 0, 0);

		R_ClearBatch (cbx);
		++(*brushpasses);
	}
}

/*
================
R_BatchSurface

Add the surface to the current batch, or just draw it immediately if we're not
using VBOs.
================
*/
static void R_BatchSurface (
	cb_context_t *cbx, msurface_t *s, qboolean fullbright_enabled, qboolean alpha_test, qboolean alpha_blend, qboolean use_zbias, gltexture_t *lightmap_texture,
	uint32_t *brushpasses)
{
	int num_surf_indices;

	num_surf_indices = R_NumTriangleIndicesForSurf (s);

	if (cbx->num_vbo_indices + num_surf_indices > MAX_BATCH_SIZE)
		R_FlushBatch (cbx, fullbright_enabled, alpha_test, alpha_blend, use_zbias, lightmap_texture, brushpasses);

	R_TriangleIndicesForSurf (s, &cbx->vbo_indices[cbx->num_vbo_indices]);
	cbx->num_vbo_indices += num_surf_indices;
}

float GL_WaterAlphaForEntityTextureType (entity_t *ent, textype_t type)
{
	float entalpha;
	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForTextureType (type);
	else
		entalpha = ENTALPHA_DECODE (ent->alpha);
	return entalpha;
}

// ============================================================================
// q2rtx: RT renderer world drawing (ported from vkquake-rt)
// ============================================================================

extern RgVertex *rtallbrushvertices;

extern cvar_t rt_classic_render;
extern cvar_t rt_plight_intensity;
extern cvar_t rt_plight_radius;
extern cvar_t rt_wlight_intensity;
extern cvar_t rt_wlight_radius;
extern cvar_t rt_brush_rough;
extern cvar_t rt_brush_metal;
extern cvar_t rt_reflrefr_depth;

// quadrilateral area-light shapes on the floor/wall grid are converted to
// spherical lights
#define RT_USE_SPHERE_INSTEAD_OF_POLY 1

#define MAX_WORLDLIGHTS_COUNT 2048
static RgPolygonalLightUploadInfo rt_wldlights_tri[MAX_WORLDLIGHTS_COUNT];
static int                        rt_wldlights_tri_count = 0;
static RgSphericalLightUploadInfo rt_wldlights_sph[MAX_WORLDLIGHTS_COUNT];
static int                        rt_wldlights_sph_count = 0;

#if RT_USE_SPHERE_INSTEAD_OF_POLY
static RgPolygonalLightUploadInfo rt_tempbuffer[512];
#endif

#define RT_CUSTOMLIGHTS_PATH RT_OVERRIDEN_FOLDER "world_custom_lights.txt"
typedef struct rt_worldcustomlight_t
{
	char      mapname[64];
	// color is in [0,1]
	RgFloat3D color01;
	RgFloat3D position;
	qboolean  deleted;
} rt_worldcustomlight_t;
static rt_worldcustomlight_t *rt_customlights_all = NULL;
static int                    rt_customlights_all_count = 0;
static int                   *rt_customlights_curr = NULL;
static int                    rt_customlights_curr_count = 0;

#define RT_CUSTOMPORTALS_PATH RT_OVERRIDEN_FOLDER "world_custom_portals.txt"

typedef struct rt_uploadsurf_state_t
{
	int          entuniqueid;
	entity_t    *ent;
	qmodel_t    *model;
	msurface_t  *surf;
	gltexture_t *diffuse_tex;
	gltexture_t *lightmap_tex;
	qboolean     alpha_test;
	float        alpha;
	qboolean     use_zbias;
	qboolean     is_warp;
	qboolean     is_water;
	qboolean     is_acid;
	qboolean     is_teleport;
} rt_uploadsurf_state_t;

static qboolean  RT_FindNearestTeleport (const RgGeometryUploadInfo *info, uint8_t *result, qboolean *potentially_mirror);
static RgFloat3D ApplyTransform (const RgTransform *transform, const vec3_t v);
static void      PolyToSphericalLights (const RgPolygonalLightUploadInfo *polys, int count, qboolean upload);

static void RT_ClearBatch (rt_cb_context_t *cbx)
{
	cbx->batch_verts_count = 0;
	cbx->batch_indices_count = 0;
}

RgTransform RT_GetBrushModelMatrix (entity_t *e)
{
	if (e == NULL)
	{
		const static RgTransform identity = RT_TRANSFORM_IDENTITY;
		return identity;
	}

	vec3_t e_angles;
	VectorCopy (e->angles, e_angles);
	e_angles[0] = -e_angles[0]; // stupid quake bug

	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, e->origin, e_angles, ENTSCALE_DEFAULT);

	return RT_GetModelTransform (model_matrix);
}

static void AccumulateCenterAndNormal (const RgPolygonalLightUploadInfo *src, vec3_t inout_center, vec3_t inout_normal)
{
	vec3_t local_center = {0, 0, 0};

	const float *a = src->positions[0].data;
	const float *b = src->positions[1].data;
	const float *c = src->positions[2].data;

	VectorAdd (local_center, a, local_center);
	VectorAdd (local_center, b, local_center);
	VectorAdd (local_center, c, local_center);
	VectorScale (local_center, 1.0f / 3.0f, local_center);

	vec3_t e1, e2;
	VectorSubtract (b, a, e1);
	VectorSubtract (c, a, e2);
	VectorNormalize (e1);
	VectorNormalize (e2);

	vec3_t local_normal;
	CrossProduct (e1, e2, local_normal);

	VectorAdd (inout_center, local_center, inout_center);
	VectorAdd (inout_normal, local_normal, inout_normal);
}

static qboolean HaveSharedEdge (const RgPolygonalLightUploadInfo *poly_a, const RgPolygonalLightUploadInfo *poly_b)
{
	for (int e = 0; e < 3; e++)
	{
		const RgFloat3D *edge_cur[2] = {
			&poly_a->positions[(e + 0) % 3],
			&poly_a->positions[(e + 1) % 3],
		};

		for (int ek = 0; ek < 3; ek++)
		{
			const RgFloat3D *edge_prev[2] = {
				&poly_b->positions[(ek + 0) % 3],
				&poly_b->positions[(ek + 1) % 3],
			};

			const float threshold = 0.1f;

			float l0 = VectorLengthSquared (edge_cur[0]->data, edge_prev[0]->data);
			float l1 = VectorLengthSquared (edge_cur[1]->data, edge_prev[1]->data);

			float r0 = VectorLengthSquared (edge_cur[0]->data, edge_prev[1]->data);
			float r1 = VectorLengthSquared (edge_cur[1]->data, edge_prev[0]->data);

			if ((l0 < threshold && l1 < threshold) || (r0 < threshold && r1 < threshold))
			{
				return true;
			}
		}
	}

	return false;
}

static void RT_FlushBatch (rt_cb_context_t *cbx, const rt_uploadsurf_state_t *s, uint32_t *brushpasses)
{
	if (cbx->batch_verts_count == 0 || cbx->batch_indices_count == 0)
	{
		return;
	}

	const RgVertex *vertices = cbx->batch_verts;
	const uint32_t *indices = cbx->batch_indices;
	const int       num_surf_verts = cbx->batch_verts_count;
	const int       num_surf_indices = cbx->batch_indices_count;

	// i.e. uploaded once at the level load
	const qboolean is_static_geom = (s->model == cl.worldmodel) && !s->is_warp;

	gltexture_t *diffuse_tex = r_lightmap_cheatsafe ? NULL : s->diffuse_tex;
	gltexture_t *lightmap_tex = r_fullbright_cheatsafe ? NULL : s->lightmap_tex;

	// The classic lightmap (static baked light + dynamic dlight patches) is
	// applied as a SHADE layer on top of the RT albedo. In the RT renderer the
	// ray tracer produces ALL the lighting (Q2RTX model), so the classic
	// lightmap must not be part of the RT material.
	if (!CVAR_TO_BOOL (rt_classic_render))
	{
		lightmap_tex = NULL;
	}

	// Curated poly light textures (@POLY_LIGHT, e.g. *light*) become light
	// sources; with RT_USE_SPHERE_INSTEAD_OF_POLY they are converted to sphere
	// lights.
	const qboolean is_poly_light = diffuse_tex && diffuse_tex->rtcustomtextype == RT_CUSTOMTEXTUREINFO_TYPE_POLY_LIGHT;

	if (is_poly_light)
	{
		const RgTransform transf = RT_GetBrushModelMatrix (s->ent);

		vec3_t color;
		VectorCopy (diffuse_tex->rtlightcolor, color);
		VectorScale (color, CVAR_TO_FLOAT (rt_plight_intensity), color);
		RT_FIXUP_LIGHT_INTENSITY (color, true);

		for (int tri = 0; tri < num_surf_indices / 3; tri++)
		{
			const vec_t *a0 = vertices[indices[tri * 3 + 0]].position;
			const vec_t *a1 = vertices[indices[tri * 3 + 1]].position;
			const vec_t *a2 = vertices[indices[tri * 3 + 2]].position;

			RgPolygonalLightUploadInfo light_info = {
				.uniqueID = RT_GetBrushSurfUniqueId (s->entuniqueid, s->model, s->surf, tri),
				.color = RT_VEC3 (color),
				.positions =
					{
						ApplyTransform (&transf, a0),
						ApplyTransform (&transf, a1),
						ApplyTransform (&transf, a2),
					},
			};

			if (!is_static_geom)
			{
#if RT_USE_SPHERE_INSTEAD_OF_POLY
				if (tri < (int)countof (rt_tempbuffer))
				{
					rt_tempbuffer[tri] = light_info;
				}
				else
				{
					assert (false);
				}
#else
				RgResult r = rgUploadPolygonalLight (vulkan_globals_rt.instance, &light_info);
				RG_CHECK (r);
#endif
			}
			else
			{
				// if it's a static geometry, then save light data
				// to upload it each frame
				if (rt_wldlights_tri_count < MAX_WORLDLIGHTS_COUNT)
				{
					rt_wldlights_tri[rt_wldlights_tri_count++] = light_info;
				}
				else
				{
					// overflow: skip (don't assert - large maps may exceed the cap)
				}
			}
		}

#if RT_USE_SPHERE_INSTEAD_OF_POLY
		if (!is_static_geom)
		{
			PolyToSphericalLights (rt_tempbuffer, num_surf_indices / 3, true);
		}
#endif
	}

	if (s->is_teleport && !CVAR_TO_BOOL (rt_classic_render) && CVAR_TO_INT32 (rt_reflrefr_depth) > 0)
	{
		diffuse_tex = NULL;
	}

	float alpha = CLAMP (0.0f, s->alpha, 1.0f);
	uint8_t portalindex = 0;

	qboolean is_mirror = diffuse_tex && diffuse_tex->rtcustomtextype == RT_CUSTOMTEXTUREINFO_TYPE_MIRROR;
	qboolean rasterize = (alpha < 1.0f) && !s->is_warp;

	if (rasterize)
	{
		// worldmodel must be uploaded only once
		assert (!is_static_geom);

		RgRasterizedGeometryUploadInfo info = {
			.renderType = RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
			.vertexCount = num_surf_verts,
			.pVertices = vertices,
			.indexCount = num_surf_indices,
			.pIndices = indices,
			.transform = RT_GetBrushModelMatrix (s->ent),
			.color = RT_COLOR_WHITE,
			.material = diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
			.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		if (s->alpha_test)
		{
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
		}

		if (alpha < 1.0f)
		{
			info.color.data[3] = alpha;
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
			info.blendFuncSrc = RG_BLEND_FACTOR_SRC_ALPHA;
			info.blendFuncDst = RG_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}
		else
		{
			info.pipelineState |= RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE;
		}

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals_rt.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = RT_GetBrushSurfUniqueId (s->entuniqueid, s->model, s->surf, 0),
			.flags =
				(is_mirror ? RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT : 0) |
				(s->is_teleport && !CVAR_TO_BOOL (rt_classic_render) ? RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT : 0) |
				RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = is_static_geom ? RG_GEOMETRY_TYPE_STATIC : RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType =
				(s->is_teleport && CVAR_TO_BOOL (rt_classic_render)) ? RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE :
				is_mirror ? RG_GEOMETRY_PASS_THROUGH_TYPE_MIRROR :
				s->is_water ? RG_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT :
				s->is_acid ? RG_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT :
				s->is_teleport ? RG_GEOMETRY_PASS_THROUGH_TYPE_PORTAL :
				RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType = RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = num_surf_verts,
			.pVertices = vertices,
			.indexCount = num_surf_indices,
			.pIndices = indices,
			.layerColors =
				{
					RT_COLOR_WHITE,
					RT_COLOR_WHITE,
				},
			.layerBlendingTypes =
				{
					RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE,
					lightmap_tex ? RG_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE : 0,
				},
			.geomMaterial =
				{
					diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
					lightmap_tex ? lightmap_tex->rtmaterial : RG_NO_MATERIAL,
				},
			.defaultRoughness = CVAR_TO_FLOAT (rt_brush_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_brush_metal),
			.defaultEmission = 0,
			.transform = RT_GetBrushModelMatrix (s->ent),
		};

		if (s->is_teleport && !CVAR_TO_BOOL (rt_classic_render))
		{
			qboolean portal_is_mirror = false;

			if (RT_FindNearestTeleport (&info, &portalindex, &portal_is_mirror))
			{
				if (portal_is_mirror)
				{
					info.passThroughType = RG_GEOMETRY_PASS_THROUGH_TYPE_MIRROR;
				}
				else
				{
					info.pPortalIndex = &portalindex;
				}
			}
		}

		RgResult r = rgUploadGeometry (vulkan_globals_rt.instance, &info);
		RG_CHECK (r);
	}

	RT_ClearBatch (cbx);
	++(*brushpasses);
}

static void RT_BatchSurface (rt_cb_context_t *cbx, const rt_uploadsurf_state_t *s, uint32_t *brushpasses)
{
	int num_surf_verts = s->surf->numedges;
	int num_surf_indices = q_max (0, 3 * (num_surf_verts - 2));

	if (cbx->batch_indices_count + num_surf_indices > MAX_BATCH_INDICES ||
		cbx->batch_verts_count + num_surf_verts > MAX_BATCH_VERTS)
	{
		RT_FlushBatch (cbx, s, brushpasses);
	}

	// fan triangulation, like R_TriangleIndicesForSurf
	uint32_t *dest = &cbx->batch_indices[cbx->batch_indices_count];
	for (int i = 2; i < num_surf_verts; i++)
	{
		*dest++ = cbx->batch_verts_count + i;
		*dest++ = cbx->batch_verts_count + i - 1;
		*dest++ = cbx->batch_verts_count;
	}

	memcpy (&cbx->batch_verts[cbx->batch_verts_count], rtallbrushvertices + s->surf->vbo_firstvert, sizeof (RgVertex) * num_surf_verts);

	cbx->batch_indices_count += num_surf_indices;
	cbx->batch_verts_count += num_surf_verts;
}

/*
================
GL_WaterAlphaForEntitySurface -- ericw

Returns the water alpha to use for the entity and surface combination.
================
*/
float GL_WaterAlphaForEntitySurface (entity_t *ent, msurface_t *s)
{
	float entalpha;
	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForSurface (s);
	else
		entalpha = ENTALPHA_DECODE (ent->alpha);
	return entalpha;
}

void RT_DrawTextureChains_ShowTris (rt_cb_context_t *cbx, qmodel_t *model, texchain_t chain)
{
	int         i;
	msurface_t *s;
	texture_t  *t;
	float       color[] = {1.0f, 1.0f, 1.0f};
	const float alpha = 1.0f;

	const static RgTransform tr = RT_TRANSFORM_IDENTITY;

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
			DrawGLPoly_RT (
				cbx, RT_UNIQUEID_DONTCARE,
				s->polys, color, alpha,
				&tr, NULL,
				CVAR_TO_BOOL (r_showtris) ? DRAW_GL_POLY_TYPE_SHOWTRI : DRAW_GL_POLY_TYPE_SHOWTRI_NODEPTH);
	}
}

void RT_DrawTextureChains_Water (rt_cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	int                   i;
	msurface_t           *s;
	texture_t            *t;
	rt_uploadsurf_state_t last_state = {0};

	uint32_t brushpasses = 0;
	for (i = 0; i < model->numtextures; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;

		RT_ClearBatch (cbx);

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			if (model != cl.worldmodel)
			{
				// ericw -- this is copied from R_DrawSequentialPoly.
				// If the poly is not part of the world we have to
				// set this flag
				Atomic_StoreUInt32 (&t->update_warp, true); // FIXME: one frame too late!
			}

			rt_uploadsurf_state_t cur_state = {
				.entuniqueid = entuniqueid,
				.ent = ent,
				.model = model,
				.surf = s,
				.diffuse_tex = t->gltexture,
				.lightmap_tex = (s->lightmaptexturenum >= 0) ? lightmaps[s->lightmaptexturenum].texture : greytexture,
				.alpha_test = false,
				.alpha = GL_WaterAlphaForEntitySurface (ent, s),
				.use_zbias = false,
				.is_warp = true,
				.is_water = s->flags & SURF_DRAWWATER,
				.is_acid = s->flags & SURF_DRAWSLIME,
				.is_teleport = (s->flags & SURF_DRAWTELE),
			};

			if (cur_state.lightmap_tex != last_state.lightmap_tex ||
				fabsf (cur_state.alpha - last_state.alpha) < 0.001f)
			{
				RT_FlushBatch (cbx, &last_state, &brushpasses);
			}

			RT_BatchSurface (cbx, &cur_state, &brushpasses);
			last_state = cur_state;
		}

		RT_FlushBatch (cbx, &last_state, &brushpasses);
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

void RT_DrawTextureChains_Multitexture (
	rt_cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, const float alpha, int texstart, int texend, int entuniqueid)
{
	int                   i;
	msurface_t           *s;
	texture_t            *t;
	qboolean              use_zbias = (gl_zfix.value && model != cl.worldmodel);
	int                   ent_frame = ent != NULL ? ent->frame : 0;
	rt_uploadsurf_state_t last_state = {0};

	uint32_t brushpasses = 0;
	for (i = texstart; i < texend; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		RT_ClearBatch (cbx);

		qboolean alpha_test = (t->texturechains[chain]->flags & SURF_DRAWFENCE) != 0;
		gltexture_t *diffuse_tex = R_TextureAnimation (t, ent_frame)->gltexture;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			// Sky surfaces are not ray-traced geometry: the sky is drawn to the
			// sky cubemap by Sky_ProcessTextureChains / Sky_DrawSkySurface (which
			// read chain_world directly). Skip them here.
			if (s->flags & SURF_DRAWSKY)
				continue;

			rt_uploadsurf_state_t cur_state = {
				.entuniqueid = entuniqueid,
				.ent = ent,
				.model = model,
				.surf = s,
				.diffuse_tex = diffuse_tex,
				.lightmap_tex = (s->lightmaptexturenum >= 0) ? lightmaps[s->lightmaptexturenum].texture : greytexture,
				.alpha_test = alpha_test,
				.alpha = alpha,
				.use_zbias = use_zbias,
				.is_warp = false,
				.is_water = false,
				.is_acid = false,
				.is_teleport = false,
			};

			if (cur_state.lightmap_tex != last_state.lightmap_tex)
			{
				RT_FlushBatch (cbx, &last_state, &brushpasses);
			}

			RT_BatchSurface (cbx, &cur_state, &brushpasses);
			last_state = cur_state;
		}

		RT_FlushBatch (cbx, &last_state, &brushpasses);
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
=================
RT_DrawTextureChains

RT renderer version of R_DrawTextureChains (with entity unique id).
=================
*/
void RT_DrawTextureChains (rt_cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	float entalpha;

	if (ent != NULL)
		entalpha = ENTALPHA_DECODE (ent->alpha);
	else
		entalpha = 1;

	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	RT_DrawTextureChains_Multitexture (cbx, model, ent, chain, entalpha, 0, model->numtextures, entuniqueid);
}

#if RT_USE_SPHERE_INSTEAD_OF_POLY
static void AddSphericalLight (qboolean upload, const RgPolygonalLightUploadInfo *src, vec3_t accum_center, vec3_t accum_normal, int sharing)
{
	VectorScale (accum_center, 1.0f / (float)sharing, accum_center);

	float radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_plight_radius));

	// The emission normal of the light surface. Degenerate (e.g. a box-like
	// flame where opposite faces cancel) -> zero normal = full sphere.
	RgFloat3D normal = {{0, 0, 0}};
	if (VectorLength (accum_normal) > 0.001f)
	{
		VectorNormalize (accum_normal);
		normal.data[0] = accum_normal[0];
		normal.data[1] = accum_normal[1];
		normal.data[2] = accum_normal[2];

		VectorMA (accum_center, radius, accum_normal, accum_center);
	}

	RgSphericalLightUploadInfo light_info = {
		.uniqueID = src->uniqueID,
		.color = src->color,
		.position = RT_VEC3 (accum_center),
		.radius = radius,
		.normal = normal,
	};

	if (upload)
	{
		RgResult r = rgUploadSphericalLight (vulkan_globals_rt.instance, &light_info);
		RG_CHECK (r);
	}
	else
	{
		rt_wldlights_sph[rt_wldlights_sph_count++] = light_info;
	}
}

static void PolyToSphericalLights (const RgPolygonalLightUploadInfo *polys, int count, qboolean upload)
{
	vec3_t accum_center = {0, 0, 0};
	vec3_t accum_normal = {0, 0, 0};
	int    sharing = 0;

	for (int i = 1; i < count; i++)
	{
		const RgPolygonalLightUploadInfo *poly_prev = &polys[i - 1];
		const RgPolygonalLightUploadInfo *poly_cur = &polys[i];

		if (HaveSharedEdge (poly_prev, poly_cur))
		{
			if (sharing == 0)
			{
				AccumulateCenterAndNormal (poly_prev, accum_center, accum_normal);
				sharing++;
			}

			AccumulateCenterAndNormal (poly_cur, accum_center, accum_normal);
			sharing++;
		}
		else
		{
			if (sharing > 0)
			{
				AddSphericalLight (upload, poly_cur, accum_center, accum_normal, sharing);

				RT_VEC3_SET (accum_center, 0, 0, 0);
				RT_VEC3_SET (accum_normal, 0, 0, 0);
			}

			sharing = 0;
		}
	}

	if (sharing > 0)
	{
		AddSphericalLight (upload, &polys[count - 1], accum_center, accum_normal, sharing);
	}
}
#endif

void RT_DrawWorld (rt_cb_context_t *cbx, int index)
{
	rt_wldlights_sph_count = 0;
	rt_wldlights_tri_count = 0;

	if (!r_drawworld_cheatsafe)
		return;

	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	RT_DrawTextureChains_Multitexture (cbx, cl.worldmodel, NULL, chain_world, 1, world_texstart[index], world_texend[index], ENT_UNIQUEID_WORLD);

#if RT_USE_SPHERE_INSTEAD_OF_POLY
	PolyToSphericalLights (rt_wldlights_tri, rt_wldlights_tri_count, false);
#endif
}

void RT_DrawWorld_Water (rt_cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	RT_DrawTextureChains_Water (cbx, cl.worldmodel, NULL, chain_world, ENT_UNIQUEID_WORLD);
}

void RT_DrawWorld_ShowTris (rt_cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	RT_DrawTextureChains_ShowTris (cbx, cl.worldmodel, chain_world);
}

void RT_CustomLights_Parse (void)
{
	rt_customlights_all_count = 0;
	rt_customlights_curr_count = 0;

	FILE *f = fopen (RT_CUSTOMLIGHTS_PATH, "r");
	if (f == NULL)
	{
		Con_Printf ("Couldn't open %s\n", RT_CUSTOMLIGHTS_PATH);
		return;
	}

	int alloccount = 1;
	{
		int ch = 0;
		do
		{
			ch = fgetc (f);
			if (ch == '\n')
			{
				alloccount++;
			}
		} while (ch != EOF);
	}
	rt_customlights_all = Mem_Realloc (rt_customlights_all, sizeof (rt_customlights_all[0]) * alloccount);
	rt_customlights_curr = Mem_Realloc (rt_customlights_curr, sizeof (rt_customlights_curr[0]) * alloccount);
	rewind (f);

	qboolean foundend = false;
	char     curline[256] = "";

	while (!foundend)
	{
		{
			int i = 0;

			while (true)
			{
				int ch = fgetc (f);

				if (ch == '\n' || ch == '\r' || ch == '\0' || ch == EOF)
				{
					foundend = (ch == '\0' || ch == EOF);
					break;
				}

				if (i >= (int)sizeof (curline))
				{
					Sys_Error (RT_CUSTOMLIGHTS_PATH ": line must be < 256 characters");
				}

				curline[i] = (char)ch;
				i++;
			}

			curline[i] = '\0';
		}

		if (curline[0] == '\0')
		{
			continue;
		}

		char      mapname[countof (rt_customlights_all[0].mapname)];
		RgFloat3D position;
		char      str_hexcolor[8];

		int c = sscanf (curline, "%s %f %f %f %6s", mapname, &position.data[0], &position.data[1], &position.data[2], str_hexcolor);
		if (c >= 5)
		{
			const RgFloat3D color01 = RT_HexStringToColor (str_hexcolor);

			mapname[countof (mapname) - 1] = '\0';

			rt_worldcustomlight_t *dst = &rt_customlights_all[rt_customlights_all_count++];
			{
				strncpy (dst->mapname, mapname, sizeof (dst->mapname));
				dst->color01 = color01;
				dst->position = position;
				dst->deleted = false;
			}
		}
	}

	fclose (f);

	// make list for current map
	const char *cur_mapname = cl.worldmodel->name;

	for (int i = 0; i < rt_customlights_all_count; i++)
	{
		const rt_worldcustomlight_t *src = &rt_customlights_all[i];

		if (strncmp (src->mapname, cur_mapname, countof (src->mapname)) == 0)
		{
			rt_customlights_curr[rt_customlights_curr_count++] = i;
		}
	}
}

void RT_CustomLights_SaveCmd (void)
{
#ifdef _WIN32
	// backup file
	{
		static int backupId = 0;
		backupId = (backupId + 1) % 15;
#define BACKUP_FOLDER RT_OVERRIDEN_FOLDER "backup"
		char name[128];
		sprintf (name, BACKUP_FOLDER "/world_custom_lights - %d.txt", backupId);
		CreateDirectory (BACKUP_FOLDER, 0);
		CopyFile (RT_CUSTOMLIGHTS_PATH, name, FALSE);
	}
#endif

	FILE *f = fopen (RT_CUSTOMLIGHTS_PATH, "w+");
	if (f == NULL)
	{
		Con_Printf ("Couldn't open %s\n", RT_CUSTOMLIGHTS_PATH);
		return;
	}

	for (int i = 0; i < rt_customlights_all_count; i++)
	{
		const rt_worldcustomlight_t *lt = &rt_customlights_all[i];

		if (lt->deleted)
		{
			continue;
		}

		const float *rawcolor = lt->color01.data;
		const float *position = lt->position.data;

		char hexstr[7];
		assert (rawcolor[0] >= 0.0f && rawcolor[0] < 1.01f);
		assert (rawcolor[1] >= 0.0f && rawcolor[1] < 1.01f);
		assert (rawcolor[2] >= 0.0f && rawcolor[2] < 1.01f);
		RT_ColorToHexString (rawcolor, hexstr);

		fprintf (f, "%s %.2f %.2f %.2f %6s\n", lt->mapname, position[0], position[1], position[2], hexstr);
	}

	fclose (f);
}

void RT_CustomLights_AddCmd (void)
{
	if (Cmd_Argc () != 5)
	{
		Con_Printf ("adds a custom light at a given position (no persistence between saves)\n");
		Con_Printf ("usage: <r> <g> <b> <intensity (0..1]>\n");
		return;
	}

	RgFloat3D color01 = {
		strtof (Cmd_Argv (1), NULL) / 255.0f,
		strtof (Cmd_Argv (2), NULL) / 255.0f,
		strtof (Cmd_Argv (3), NULL) / 255.0f,
	};
	float intensity = strtof (Cmd_Argv (4), NULL);
	VectorScale (color01.data, intensity, color01.data);

	RgFloat3D pos = RT_VEC3 (r_refdef.vieworg);

	// slight offset so the light is not right at the camera position
	vec3_t forward, right, up;
	AngleVectors (r_refdef.viewangles, forward, right, up);
	pos.data[0] += forward[0] * METRIC_TO_QUAKEUNIT (0.3f);
	pos.data[1] += forward[1] * METRIC_TO_QUAKEUNIT (0.3f);
	pos.data[2] += forward[2] * METRIC_TO_QUAKEUNIT (0.3f);

	rt_customlights_all = Mem_Realloc (rt_customlights_all, sizeof (rt_customlights_all[0]) * (rt_customlights_all_count + 1));
	rt_customlights_curr = Mem_Realloc (rt_customlights_curr, sizeof (rt_customlights_curr[0]) * (rt_customlights_all_count + 1));

	rt_worldcustomlight_t *dst = &rt_customlights_all[rt_customlights_all_count];
	{
		const char *cur_mapname = cl.worldmodel->name;
		strncpy (dst->mapname, cur_mapname, sizeof (dst->mapname));
		dst->color01 = color01;
		dst->position = pos;
		dst->deleted = false;
	}
	rt_customlights_curr[rt_customlights_curr_count++] = rt_customlights_all_count;

	rt_customlights_all_count++;

	RT_CustomLights_SaveCmd ();
}

void RT_CustomLights_RemoveCmd (void)
{
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("removes custom lights around the camera\n");
		Con_Printf ("usage: <radius (meters)>\n");

		return;
	}

	const vec3_t around = RT_VEC3 (r_refdef.vieworg);
	float radius = strtof (Cmd_Argv (1), NULL);
	radius = METRIC_TO_QUAKEUNIT (radius);

	int count = 0;

	// scan current world lights
	for (int i = 0; i < rt_customlights_curr_count; i++)
	{
		rt_worldcustomlight_t *lt = &rt_customlights_all[rt_customlights_curr[i]];

		if (lt->deleted)
		{
			continue;
		}

		if (VectorLengthSquared (lt->position.data, around) < radius * radius)
		{
			lt->deleted = true;
			count++;
		}
	}

	Con_Printf ("removed %d lights\n", count);

	{
		RT_CustomLights_SaveCmd ();
	}
}

void RT_UploadAllWorldModelLights (void)
{
#if RT_USE_SPHERE_INSTEAD_OF_POLY
	for (int i = 0; i < rt_wldlights_sph_count; i++)
	{
		RgResult r = rgUploadSphericalLight (vulkan_globals_rt.instance, &rt_wldlights_sph[i]);
		RG_CHECK (r);

		RT_ClusterLightAdd (rt_wldlights_sph[i].uniqueID, rt_wldlights_sph[i].position.data);
	}
#else
	for (int i = 0; i < rt_wldlights_tri_count; i++)
	{
		RgResult r = rgUploadPolygonalLight (vulkan_globals_rt.instance, &rt_wldlights_tri[i]);
		RG_CHECK (r);
	}
#endif

	for (int i = 0; i < rt_customlights_curr_count; i++)
	{
		const rt_worldcustomlight_t *src = &rt_customlights_all[rt_customlights_curr[i]];

		if (src->deleted)
		{
			continue;
		}

		RgFloat3D color = src->color01;
		VectorScale (color.data, CVAR_TO_FLOAT (rt_wlight_intensity), color.data);
		RT_FIXUP_LIGHT_INTENSITY (color.data, true);

		RgSphericalLightUploadInfo lt = {
			.uniqueID = RT_GetCustomObjectUniqueId (i),
			.color = color,
			.position = src->position,
			.radius = METRIC_TO_QUAKEUNIT (CVAR_TO_FLOAT (rt_wlight_radius)),
		};

		RgResult r = rgUploadSphericalLight (vulkan_globals_rt.instance, &lt);
		RG_CHECK (r);

		RT_ClusterLightAdd (lt.uniqueID, lt.position.data);
	}
}

typedef struct rt_teleport_s
{
	vec3_t   a;
	vec3_t   b;
	float    b_angle;
	qboolean potentially_mirror;
} rt_teleport_t;

rt_teleport_t *rt_teleports = NULL;
int            rt_teleports_count = 0;

struct rt_triggerteleport_t
{
	char target[128];
	char model[128];
};
struct rt_infoteleportdestination_t
{
	char   targetname[128];
	float  angle;
	vec3_t origin;
};
struct rt_parsetriggers_result_t
{
	struct rt_triggerteleport_t         *trigs;
	int                                  trigs_count;
	struct rt_infoteleportdestination_t *dsts;
	int                                  dsts_count;
};

static struct rt_parsetriggers_result_t ParseTeleportTriggers (void)
{
	struct rt_parsetriggers_result_t result = {0};

	if (!cl.worldmodel)
	{
		return result;
	}

	const char *data = cl.worldmodel->entities;
	if (!data)
	{
		return result;
	}

	char key[128], value[4096];

#define STRUCT_STATE_STRUCT_STARTED	   1
#define STRUCT_STATE_CLASSNAME_TRIGGER	   2
#define STRUCT_STATE_CLASSNAME_DESTINATION 4
#define STRUCT_STATE_TARGET		   8
#define STRUCT_STATE_TARGETNAME		   16
#define STRUCT_STATE_MODEL		   32
#define STRUCT_STATE_ANGLE		   64
#define STRUCT_STATE_ORIGIN		   128
	int structstate = 0;

	struct
	{
		struct rt_triggerteleport_t         tr;
		struct rt_infoteleportdestination_t dst;
	} structvalues = {0};

	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return result; // error

		if (com_token[0] == '{')
		{
			memset (&structvalues, 0, sizeof (structvalues));
			structstate = STRUCT_STATE_STRUCT_STARTED;
			continue;
		}
		else if (com_token[0] == '}')
		{
			if (structstate & STRUCT_STATE_STRUCT_STARTED)
			{
				if (structstate & STRUCT_STATE_CLASSNAME_TRIGGER)
				{
					result.trigs = Mem_Realloc (result.trigs, sizeof (*result.trigs) * (result.trigs_count + 1));
					result.trigs[result.trigs_count] = structvalues.tr;
					result.trigs_count++;
				}
				else if (structstate & STRUCT_STATE_CLASSNAME_DESTINATION)
				{
					result.dsts = Mem_Realloc (result.dsts, sizeof (*result.dsts) * (result.dsts_count + 1));
					result.dsts[result.dsts_count] = structvalues.dst;
					result.dsts_count++;
				}
			}

			structstate = 0; // end of struct
			continue;
		}

		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_Parse (data);
		if (!data)
			return result; // error
		q_strlcpy (value, com_token, sizeof (value));

		if (strcmp (key, "classname") == 0)
		{
			if (strcmp (value, "trigger_teleport") == 0)
			{
				structstate |= STRUCT_STATE_CLASSNAME_TRIGGER;
			}
			else if (strcmp (value, "info_teleport_destination") == 0)
			{
				structstate |= STRUCT_STATE_CLASSNAME_DESTINATION;
			}
		}
		else if (strcmp (key, "origin") == 0)
		{
			vec3_t tmpvec;
			int    components = sscanf (value, "%f %f %f", &tmpvec[0], &tmpvec[1], &tmpvec[2]);

			if (components == 3)
			{
				structvalues.dst.origin[0] = tmpvec[0];
				structvalues.dst.origin[1] = tmpvec[1];
				structvalues.dst.origin[2] = tmpvec[2];
				structstate |= STRUCT_STATE_ORIGIN;
			}
		}
		else if (strcmp (key, "angle") == 0)
		{
			float tmp;
			int   components = sscanf (value, "%f", &tmp);

			if (components == 1)
			{
				structvalues.dst.angle = tmp;
				structstate |= STRUCT_STATE_ANGLE;
			}
		}
		else if (strcmp (key, "model") == 0)
		{
			q_strlcpy (structvalues.tr.model, value, sizeof (structvalues.tr.model));
			structstate |= STRUCT_STATE_MODEL;
		}
		else if (strcmp (key, "target") == 0)
		{
			q_strlcpy (structvalues.tr.target, value, sizeof (structvalues.tr.target));
			structstate |= STRUCT_STATE_TARGET;
		}
		else if (strcmp (key, "targetname") == 0)
		{
			q_strlcpy (structvalues.dst.targetname, value, sizeof (structvalues.dst.targetname));
			structstate |= STRUCT_STATE_TARGETNAME;
		}
	}
}

static float DistanceSqr (const vec3_t a, const vec3_t b)
{
	vec3_t delta;
	VectorSubtract (a, b, delta);

	return DotProduct (delta, delta);
}

#define CUSTOM_PORTAL_DISTANCE_THRESHOLD (METRIC_TO_QUAKEUNIT (3.0f))
static void LoadCustomTeleportInfoAndPatch (void)
{
	if (rt_teleports_count == 0)
	{
		return;
	}

	const char *cur_mapname = cl.worldmodel->name;
	if (cur_mapname == NULL)
	{
		Con_Printf ("Null world\n");
		return;
	}

	FILE *f = fopen (RT_CUSTOMPORTALS_PATH, "r");
	if (!f)
	{
		return;
	}

	char line[1024] = "";

	while (fgets (line, sizeof (line), f))
	{
		vec3_t entry_a = {0, 0, 0};
		vec3_t custom_output = {0, 0, 0};
		int    custom_ismirror = 0;

		char mapname[128] = "";

		int components = sscanf (
			line,
			"%s %f %f %f %f %f %f %d",
			mapname,
			&entry_a[0],
			&entry_a[1],
			&entry_a[2],
			&custom_output[0],
			&custom_output[1],
			&custom_output[2],
			&custom_ismirror);

		if (components == 7)
		{
			custom_ismirror = false;
			components = 8;
		}

		if (components == 8 && strncmp (mapname, cur_mapname, sizeof (mapname)) == 0)
		{
			for (int i = 0; i < rt_teleports_count; i++)
			{
				if (DistanceSqr (rt_teleports[i].a, entry_a) < CUSTOM_PORTAL_DISTANCE_THRESHOLD * CUSTOM_PORTAL_DISTANCE_THRESHOLD)
				{
					VectorCopy (custom_output, rt_teleports[i].b);
					rt_teleports[i].potentially_mirror = !!custom_ismirror;
				}
			}
		}
	}

	fclose (f);
}

#define RG_MAX_PORTALS 62

void RT_ParseTeleports (void)
{
	rt_teleports_count = 0;

	struct rt_parsetriggers_result_t r = ParseTeleportTriggers ();
	if (r.trigs_count == 0 || r.dsts_count == 0)
	{
		return;
	}

	for (int i = 0; i < r.trigs_count; i++)
	{
		for (int o = 0; o < r.dsts_count; o++)
		{
			const struct rt_triggerteleport_t         *in = &r.trigs[i];
			const struct rt_infoteleportdestination_t *out = &r.dsts[o];

			// if found a match between trigger and destination
			if (strncmp (in->target, out->targetname, sizeof (in->target)) == 0)
			{
				qmodel_t *mod = Mod_ForName (in->model, false);

				if (mod)
				{
					rt_teleport_t *entry;
					{
						rt_teleports = Mem_Realloc (rt_teleports, sizeof (*rt_teleports) * (rt_teleports_count + 1));
						entry = &rt_teleports[rt_teleports_count];
						memset (entry, 0, sizeof (*entry));
						rt_teleports_count++;
					}

					// trigger position
					VectorAdd (mod->mins, mod->maxs, entry->a);
					VectorScale (entry->a, 0.5f, entry->a);

					// destination position
					VectorCopy (out->origin, entry->b);
					entry->b_angle = out->angle;

					entry->potentially_mirror = false;
				}

				break;
			}
		}
	}

	// vkpt's portal limit
	if (rt_teleports_count > RG_MAX_PORTALS)
	{
		rt_teleports_count = RG_MAX_PORTALS;
		Con_Warning ("Too many teleports to render, limit is 62");
	}

	Mem_Free (r.trigs);
	Mem_Free (r.dsts);

	LoadCustomTeleportInfoAndPatch ();
}

static RgFloat3D ApplyTransform (const RgTransform *transform, const vec3_t v)
{
	RgFloat3D r = {0};
	for (int i = 0; i < 3; i++)
	{
		r.data[i] =
			transform->matrix[i][0] * v[0] +
			transform->matrix[i][1] * v[1] +
			transform->matrix[i][2] * v[2] +
			transform->matrix[i][3];
	}
	return r;
}

static qboolean RT_FindNearestTeleport (const RgGeometryUploadInfo *info, uint8_t *result, qboolean *potentially_mirror)
{
	vec3_t emin = {FLT_MAX, FLT_MAX, FLT_MAX};
	vec3_t emax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	for (uint32_t i = 0; i < info->vertexCount; i++)
	{
		RgFloat3D v = ApplyTransform (&info->transform, info->pVertices[i].position);

		for (int k = 0; k < 3; k++)
		{
			emin[k] = q_min (v.data[k], emin[k]);
			emax[k] = q_max (v.data[k], emax[k]);
		}
	}

	vec3_t center;
	VectorAdd (emin, emax, center);
	VectorScale (center, 0.5f, center);

	int   nearest = -1;
	float nearest_dist = FLT_MAX;

	for (int i = 0; i < rt_teleports_count; i++)
	{
		float d = DistanceSqr (rt_teleports[i].a, center);

		if (d < nearest_dist)
		{
			nearest = i;
			nearest_dist = d;
		}
	}

	if (nearest < 0)
	{
		return false;
	}

	assert (nearest <= RG_MAX_PORTALS);

	*result = (uint8_t)nearest;
	*potentially_mirror = rt_teleports[nearest].potentially_mirror;
	return true;
}

void RT_UploadAllTeleports (void)
{
	assert (rt_teleports_count >= 0 && rt_teleports_count <= RG_MAX_PORTALS);

	const vec3_t outoffset = {0, 0, 64};

	for (int i = 0; i < rt_teleports_count; i++)
	{
		const rt_teleport_t *tele = &rt_teleports[i];

		vec3_t forward, right, up;
		{
			vec3_t out_angles = {0, tele->b_angle, 0};
			AngleVectors (out_angles, forward, right, up);
		}

		RgPortalUploadInfo info =
			{
				.portalIndex = (uint8_t)i,
				.inPosition = RT_VEC3 (tele->a),
				.outPosition = RT_VEC3 (tele->b),
				.outDirection = RT_VEC3 (forward),
				.outUp = RT_VEC3 (up),
			};

		VectorAdd (info.outPosition.data, outoffset, info.outPosition.data);

		RgResult r = rgUploadPortal (vulkan_globals_rt.instance, &info);
		RG_CHECK (r);
	}
}

void RT_PrintNearestPortal (void)
{
	assert (rt_teleports_count >= 0 && rt_teleports_count <= RG_MAX_PORTALS);

	Con_Printf ("Camera: %.1f %.1f %.1f\n", r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);

	for (int i = 0; i < rt_teleports_count; i++)
	{
		qboolean isnear = DistanceSqr (rt_teleports[i].a, r_refdef.vieworg) < CUSTOM_PORTAL_DISTANCE_THRESHOLD * CUSTOM_PORTAL_DISTANCE_THRESHOLD;

		if (isnear)
		{
			Con_Printf ("[Near] Portal %d: %.1f %.1f %.1f\n", i, rt_teleports[i].a[0], rt_teleports[i].a[1], rt_teleports[i].a[2]);
		}
		else
		{
			Con_Printf ("       Portal %d: %.1f %.1f %.1f\n", i, rt_teleports[i].a[0], rt_teleports[i].a[1], rt_teleports[i].a[2]);
		}
	}

	int   nearest = -1;
	float nearest_dist = FLT_MAX;

	for (int i = 0; i < rt_teleports_count; i++)
	{
		float d = DistanceSqr (rt_teleports[i].a, r_refdef.vieworg);

		if (d < nearest_dist)
		{
			nearest = i;
			nearest_dist = d;
		}
	}

	if (nearest >= 0)
	{
		Con_Printf ("Nearest portal: %d\n", nearest);
	}
}

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (cb_context_t *cbx, qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t *s;
	texture_t  *t;
	float		color[] = {1.0f, 1.0f, 1.0f};
	const float alpha = 1.0f;

	for (i = 0; i < model->numtextures; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
			DrawGLPoly (cbx, s->polys, color, alpha);
	}
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, qboolean opaque_only, qboolean transparent_only)
{
	int			i, type;
	msurface_t *s;
	texture_t  *t;

	VkDeviceSize offset = 0;
	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, &bmodel_vertex_buffer, &offset);

	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 2, 1, &nulltexture->descriptor_set, 0, NULL);
	if (r_lightmap_cheatsafe)
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &greytexture->descriptor_set, 0, NULL);
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 4, 1, &vulkan_globals.bmodel_instances_desc_set, 0, NULL);
	const uint32_t instance_base = 0; // texture chain draws bake the entity transform into the mvp push constant
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 21 * sizeof (float), sizeof (uint32_t), &instance_base);

	uint32_t brushpasses = 0;
	for (type = TEXTYPE_FIRSTLIQUID; type <= TEXTYPE_LASTLIQUID; ++type)
	{
		const float	   alpha = GL_WaterAlphaForEntityTextureType (ent, (textype_t)type);
		const qboolean alpha_blend = alpha < 1.0f;

		if (opaque_only && alpha_blend)
			continue;
		if (transparent_only && !alpha_blend)
			continue;

		for (i = model->texofs[type]; i < model->texofs[type + 1]; ++i)
		{
			t = model->textures[model->usedtextures[i]];

			if (!t || !t->texturechains[chain])
				continue;

			gltexture_t *lightmap_texture = NULL;
			R_ClearBatch (cbx);

			int lastlightmap = -2;

			gltexture_t *gl_texture = t->warpimage;
			if (!r_lightmap_cheatsafe)
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &gl_texture->descriptor_set, 0, NULL);

			if (model != cl.worldmodel)
				Atomic_StoreUInt32 (
					&t->update_warp, true); // FIXME: races against UpdateWarpTextures task, bmodel-only warps may end up updating at half frequency

			for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
			{
				if (s->lightmaptexturenum != lastlightmap)
				{
					if (alpha_blend)
						R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 20 * sizeof (float), 1 * sizeof (float), &alpha);
					R_FlushBatch (cbx, false, false, alpha_blend, false, lightmap_texture, &brushpasses);
					lightmap_texture = (s->lightmaptexturenum >= 0) ? lightmaps[s->lightmaptexturenum].texture : greylightmap;
					lastlightmap = s->lightmaptexturenum;
				}
				R_BatchSurface (cbx, s, false, false, alpha_blend, false, lightmap_texture, &brushpasses);
			}

			if (alpha_blend)
				R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 20 * sizeof (float), 1 * sizeof (float), &alpha);
			R_FlushBatch (cbx, false, false, alpha_blend, false, lightmap_texture, &brushpasses);
		}
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
================
R_DrawTextureChains_Multitexture
================
*/
void R_DrawTextureChains_Multitexture (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, const float alpha, int texstart, int texend)
{
	int			 i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	 fullbright_enabled = false;
	qboolean	 alpha_test = false;
	qboolean	 alpha_blend = alpha < 1.0f;
	qboolean	 use_zbias = (gl_zfix.value && model != cl.worldmodel);
	int			 lastlightmap;
	int			 ent_frame = ent != NULL ? ent->frame : 0;
	gltexture_t *fullbright = NULL;

	VkDeviceSize offset = 0;
	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, &bmodel_vertex_buffer, &offset);

	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 2, 1, &nulltexture->descriptor_set, 0, NULL);
	if (r_lightmap_cheatsafe)
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &greytexture->descriptor_set, 0, NULL);
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 4, 1, &vulkan_globals.bmodel_instances_desc_set, 0, NULL);
	const uint32_t instance_base = 0; // texture chain draws bake the entity transform into the mvp push constant
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 21 * sizeof (float), sizeof (uint32_t), &instance_base);

	if (alpha_blend)
	{
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 20 * sizeof (float), 1 * sizeof (float), &alpha);
	}

	uint32_t brushpasses = 0;
	for (i = texstart; i < texend; ++i)
	{
		t = model->textures[model->usedtextures[i]];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & SURF_DRAWTILED)
			continue;

		if (gl_fullbrights.value && (fullbright = R_TextureAnimation (t, ent_frame)->fullbright) && !r_lightmap_cheatsafe)
		{
			fullbright_enabled = true;
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 2, 1, &fullbright->descriptor_set, 0, NULL);
		}
		else
			fullbright_enabled = false;

		gltexture_t *lightmap_texture = NULL;
		R_ClearBatch (cbx);

		lastlightmap = -1; // avoid compiler warning
		alpha_test = t->type == TEXTYPE_CUTOUT;

		texture_t	*texture = R_TextureAnimation (t, ent_frame);
		gltexture_t *gl_texture = texture->gltexture;
		if (!r_lightmap_cheatsafe)
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &gl_texture->descriptor_set, 0, NULL);

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			if (s->lightmaptexturenum != lastlightmap)
			{
				R_FlushBatch (cbx, fullbright_enabled, alpha_test, alpha_blend, use_zbias, lightmap_texture, &brushpasses);
				lightmap_texture = lightmaps[s->lightmaptexturenum].texture;
			}

			lastlightmap = s->lightmaptexturenum;
			R_BatchSurface (cbx, s, fullbright_enabled, alpha_test, alpha_blend, use_zbias, lightmap_texture, &brushpasses);
		}

		R_FlushBatch (cbx, fullbright_enabled, alpha_test, alpha_blend, use_zbias, lightmap_texture, &brushpasses);
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain)
{
	float entalpha;

	if (ent != NULL)
		entalpha = ENTALPHA_DECODE (ent->alpha);
	else
		entalpha = 1;

	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	R_DrawTextureChains_Multitexture (cbx, model, ent, chain, entalpha, 0, model->texofs[TEXTYPE_SKY]);
}

/*
=============
R_DrawWorld -- ericw -- moved from R_DrawTextureChains, which is no longer specific to the world.
=============
*/
void R_DrawWorld (cb_context_t *cbx, int index)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_BeginDebugUtilsLabel (cbx, "World");
	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	R_DrawTextureChains_Multitexture (cbx, cl.worldmodel, NULL, chain_world, 1, world_texstart[index], world_texend[index]);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (cb_context_t *cbx, qboolean transparent)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_BeginDebugUtilsLabel (cbx, transparent ? "Transparent World Water" : "Opaque World Water");
	if (indirect)
	{
		if (!transparent || R_UseIndirectTransparentWater ())
			R_DrawIndirectBrushes (cbx, true, transparent, false, -1);
		else
		{
			R_ChainVisSurfaces_TransparentWater ();
			R_DrawTextureChains_Water (cbx, cl.worldmodel, NULL, chain_world, false, true);
		}
	}
	else
		R_DrawTextureChains_Water (cbx, cl.worldmodel, NULL, chain_world, !transparent, transparent);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (cb_context_t *cbx)
{
	if (r_showtris.value == 1)
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.showtris_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]);
	else
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.showtris_depth_test_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]);

	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_ShowTris (cbx, cl.worldmodel, chain_world);
}
