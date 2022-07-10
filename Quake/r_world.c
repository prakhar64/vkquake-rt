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

extern cvar_t rt_brush_metal;
extern cvar_t rt_brush_rough;

cvar_t r_parallelmark = {"r_parallelmark", "1", CVAR_NONE};

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

static int world_texstart[NUM_WORLD_CBX];
static int world_texend[NUM_WORLD_CBX];

extern RgVertex *rtallbrushvertices;

/*
===============
mark_surfaces_state_t
===============
*/
typedef struct
{
#if defined(USE_SIMD)
	__m128 frustum_px[4];
	__m128 frustum_py[4];
	__m128 frustum_pz[4];
	__m128 frustum_pd[4];
	__m128 vieworg_px;
	__m128 vieworg_py;
	__m128 vieworg_pz;
	int    frustum_ofsx[4];
	int    frustum_ofsy[4];
	int    frustum_ofsz[4];
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

	const int num_textures = cl.worldmodel->numtextures;
	if (!use_tasks)
	{
		world_texstart[0] = 0;
		world_texend[0] = num_textures;
		return;
	}

	int total_world_surfs = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;
		total_world_surfs += t->chain_size[chain_world];
	}

	const int num_surfs_per_cbx = (total_world_surfs + NUM_WORLD_CBX - 1) / NUM_WORLD_CBX;
	int       current_cbx = 0;
	int       num_assigned_to_cbx = 0;
	for (int i = 0; i < num_textures; ++i)
	{
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->texturechains[chain_world] || t->texturechains[chain_world]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;
		assert (current_cbx < NUM_WORLD_CBX);
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

#ifdef USE_SSE2
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

		int    ofsx = mark_surfaces_state.frustum_ofsx[frustum_index];
		int    ofsy = mark_surfaces_state.frustum_ofsy[frustum_index];
		int    ofsz = mark_surfaces_state.frustum_ofsz[frustum_index];
		__m128 px = mark_surfaces_state.frustum_px[frustum_index];
		__m128 py = mark_surfaces_state.frustum_py[frustum_index];
		__m128 pz = mark_surfaces_state.frustum_pz[frustum_index];
		__m128 pd = mark_surfaces_state.frustum_pd[frustum_index];

		uint32_t frustum_lanes = 0;
		for (int boxes_index = 0; boxes_index < 4; ++boxes_index)
		{
			soa_aabb_t *box = boxes + boxes_index;
			__m128      v0 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx), px);
			__m128      v1 = _mm_mul_ps (_mm_loadu_ps ((*box) + ofsx + 4), px);
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
#endif // defined(USE_SSE2)

#if defined(USE_SIMD)
/*
===============
R_MarkVisSurfacesSIMD
===============
*/
void R_MarkVisSurfacesSIMD (qboolean *use_tasks)
{
	msurface_t  *surf;
	unsigned int i, k;
	unsigned int numleafs = cl.worldmodel->numleafs;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t    *vis = (uint32_t *)mark_surfaces_state.vis;
	uint32_t    *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t  *leafbounds = cl.worldmodel->soa_leafbounds;

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
			if (leaf->contents != CONTENTS_SKY || r_oldskyleaf.value)
			{
				unsigned int nummarksurfaces = leaf->nummarksurfaces;
				int         *marksurfaces = leaf->firstmarksurface;
				for (k = 0; k < nummarksurfaces; ++k)
				{
					unsigned int index = marksurfaces[k];
					surfvis[index / 32] |= 1u << (index % 32);
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

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
			if (!r_gpulightmapupdate.value)
				R_RenderDynamicLightmaps (surf);
			else if (surf->lightmaptexturenum >= 0)
				Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
			if (surf->texinfo->texture->warpimage)
				Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);
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
	unsigned int     j;
	unsigned int     first_leaf = index * 32;
	atomic_uint32_t *surfvis = (atomic_uint32_t *)cl.worldmodel->surfvis;
	soa_aabb_t      *leafbounds = cl.worldmodel->soa_leafbounds;
	uint32_t        *vis = (uint32_t *)mark_surfaces_state.vis;

	uint32_t *mask = &vis[index];
	if (*mask == 0)
		return;

	*mask = R_CullBoxSIMD (&leafbounds[index * 4], *mask);

	uint32_t mask_iter = *mask;
	while (mask_iter != 0)
	{
		const int i = FindFirstBitNonZero (mask_iter);

		mleaf_t *leaf = &cl.worldmodel->leafs[1 + first_leaf + i];
		if (leaf->contents != CONTENTS_SKY || r_oldskyleaf.value)
		{
			unsigned int nummarksurfaces = leaf->nummarksurfaces;
			int         *marksurfaces = leaf->firstmarksurface;
			for (j = 0; j < nummarksurfaces; ++j)
			{
				unsigned int surf_index = marksurfaces[j];
				Atomic_OrUInt32 (&surfvis[surf_index / 32], 1u << (surf_index % 32));
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

/*
===============
R_BackfaceCullSurfacesSIMD
===============
*/
void R_BackfaceCullSurfacesSIMD (int index, void *unused)
{
	uint32_t   *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	msurface_t *surf;

	uint32_t *mask = &surfvis[index];
	if (*mask == 0)
		return;

	*mask &= R_BackFaceCullSIMD (&cl.worldmodel->soa_surfplanes[index * 4]);

	uint32_t mask_iter = *mask;
	while (mask_iter != 0)
	{
		const int i = FindFirstBitNonZero (mask_iter);

		surf = &cl.worldmodel->surfaces[(index * 32) + i];
		if (surf->lightmaptexturenum >= 0)
			Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
		if (surf->texinfo->texture->warpimage)
			Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);

		const uint32_t bit_mask = ~(1u << i);
		mask_iter &= bit_mask;
	}
}

/*
===============
R_StoreLeafEFrags
===============
*/
void R_StoreLeafEFrags (void *unused)
{
	unsigned int i;
	unsigned int numleafs = cl.worldmodel->numleafs;
	uint32_t    *vis = (uint32_t *)mark_surfaces_state.vis;
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
	msurface_t  *surf;
	unsigned int numsurfaces = cl.worldmodel->numsurfaces;
	uint32_t    *surfvis = (uint32_t *)cl.worldmodel->surfvis;
	uint32_t     brushpolys = 0;
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
#endif // defined(USE_SIMD)

/*
===============
R_MarkVisSurfaces
===============
*/
void R_MarkVisSurfaces (qboolean *use_tasks)
{
	int         i, j;
	msurface_t *surf;
	mleaf_t    *leaf;
	uint32_t    brushpolys = 0;
	uint32_t   *vis = (uint32_t *)mark_surfaces_state.vis;

	leaf = &cl.worldmodel->leafs[1];
	for (i = 0; i < cl.worldmodel->numleafs; i++, leaf++)
	{
		if (vis[i / 32] & (1u << (i % 32)))
		{
			if (R_CullBox (leaf->minmaxs, leaf->minmaxs + 3))
				continue;

			if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
			{
				for (j = 0; j < leaf->nummarksurfaces; j++)
				{
					surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[j]];
					if (surf->visframe != r_visframecount)
					{
						surf->visframe = r_visframecount;
						if (!R_BackFaceCull (surf))
						{
							++brushpolys;
							R_ChainSurface (surf, chain_world);
							if (!r_gpulightmapupdate.value)
								R_RenderDynamicLightmaps (surf);
							else if (surf->lightmaptexturenum >= 0)
								Atomic_StoreUInt32 (&lightmaps[surf->lightmaptexturenum].modified, true);
							if (surf->texinfo->texture->warpimage)
								Atomic_StoreUInt32 (&surf->texinfo->texture->update_warp, true);
						}
					}
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

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
	int      i;
	qboolean nearwaterportal;
	int      numleafs = cl.worldmodel->numleafs;

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
		for (int frustum_index = 0; frustum_index < 4; ++frustum_index)
		{
			mplane_t *p = frustum + frustum_index;
			byte      signbits = p->signbits;
			__m128    vplane = _mm_loadu_ps (p->normal);
			mark_surfaces_state.frustum_ofsx[frustum_index] = signbits & 1 ? 0 : 8;   // x min/max
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
	}
#endif
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
#if defined(USE_SIMD)
		if (use_simd)
		{
			if (r_parallelmark.value)
			{
				unsigned int  numleafs = cl.worldmodel->numleafs;
				task_handle_t mark_surfaces = Task_AllocateAndAssignIndexedFunc (R_MarkLeafsSIMD, (numleafs + 31) / 32, NULL, 0);
				Task_AddDependency (prepare_mark, mark_surfaces);
				Task_Submit (mark_surfaces);

				*store_efrags = Task_AllocateAndAssignFunc (R_StoreLeafEFrags, NULL, 0);
				Task_AddDependency (mark_surfaces, *store_efrags);

				unsigned int numsurfaces = cl.worldmodel->numsurfaces;
				*cull_surfaces = Task_AllocateAndAssignIndexedFunc (R_BackfaceCullSurfacesSIMD, (numsurfaces + 31) / 32, NULL, 0);
				Task_AddDependency (mark_surfaces, *cull_surfaces);

				*chain_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_ChainVisSurfaces, &use_tasks, sizeof (qboolean));
				Task_AddDependency (*cull_surfaces, *chain_surfaces);
			}
			else
			{
				task_handle_t mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfacesSIMD, &use_tasks, sizeof (qboolean));
				Task_AddDependency (prepare_mark, mark_surfaces);
				*store_efrags = mark_surfaces;
				*chain_surfaces = mark_surfaces;
				*cull_surfaces = mark_surfaces;
			}
		}
		else
#endif
		{
			task_handle_t mark_surfaces = Task_AllocateAndAssignFunc ((task_func_t)R_MarkVisSurfaces, &use_tasks, sizeof (qboolean));
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

static int R_NumTriangleIndicesForSurf (int vertcount)
{
	return q_max (0, 3 * (vertcount - 2));
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (int basevert, int vertcount, uint32_t *dest)
{
	int i;
	for (i = 2; i < vertcount; i++)
	{
		*dest++ = basevert;
		*dest++ = basevert + i - 1;
		*dest++ = basevert + i;
	}
}

/*
================
R_ClearBatch
================
*/
static void R_ClearBatch (cb_context_t *cbx)
{
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
	R_RotateForEntity (model_matrix, e->origin, e_angles);

	return RT_GetModelTransform (model_matrix);
}

static void RT_UploadSurface (
	cb_context_t *cbx,
	int entuniqueid, entity_t *ent, qmodel_t *model, msurface_t *surf, 
	gltexture_t *diffuse_tex, gltexture_t *lightmap_tex, gltexture_t *fullbright_tex, 
	qboolean alpha_test, float alpha ,qboolean use_zbias, qboolean is_water,
	uint32_t *brushpasses)
{
	alpha = CLAMP (0.0f, alpha, 1.0f);

	int num_surf_verts = surf->numedges;
	int num_surf_indices = R_NumTriangleIndicesForSurf (num_surf_verts);

	if (num_surf_verts == 0 || num_surf_indices == 0)
	{
		return;
	}

    const RgVertex *vertices = rtallbrushvertices + surf->vbo_firstvert;

	R_TriangleIndicesForSurf (0, num_surf_verts, &cbx->vbo_indices[0]);
	const uint32_t *indices = cbx->vbo_indices;

#if 0
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
#endif

	if (r_lightmap_cheatsafe)
	{
		diffuse_tex = NULL;   
	}

	if (r_fullbright_cheatsafe)
	{
		lightmap_tex = NULL;
	}

	qboolean is_static_geom = (model == cl.worldmodel) && !is_water;
	qboolean rasterize = (alpha < 1.0f) && !is_water;

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
			.transform = RT_GetBrushModelMatrix (ent),
			.color = RT_COLOR_WHITE,
			.material = diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
			.pipelineState = RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST,
			.blendFuncSrc = 0,
			.blendFuncDst = 0,
		};

		if (alpha_test)
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

		RgResult r = rgUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
		RG_CHECK (r);
	}
	else
	{
		RgGeometryUploadInfo info = {
			.uniqueID = RT_GetBrushSurfUniqueId (entuniqueid, model, surf),
			.flags = RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT,
			.geomType = is_static_geom ? RG_GEOMETRY_TYPE_STATIC : RG_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType = is_water ? RG_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT : RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType = RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = num_surf_verts,
			.pVertices = vertices,
			.indexCount = num_surf_indices,
			.pIndices = indices,
			.layerColors = {RT_COLOR_WHITE, RT_COLOR_WHITE, RT_COLOR_WHITE},
			.layerBlendingTypes =
				{
					RG_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE,
					RG_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE,
					RG_GEOMETRY_MATERIAL_BLEND_TYPE_ADD,
				},
			.geomMaterial =
				{
					diffuse_tex ? diffuse_tex->rtmaterial : greytexture->rtmaterial,
					lightmap_tex ? lightmap_tex->rtmaterial : greytexture->rtmaterial,
					fullbright_tex ? fullbright_tex->rtmaterial : RG_NO_MATERIAL,
				},
			.defaultRoughness = CVAR_TO_FLOAT (rt_brush_rough),
			.defaultMetallicity = CVAR_TO_FLOAT (rt_brush_metal),
			.defaultEmission = 0,
			.transform = RT_GetBrushModelMatrix (ent),
		};

		RgResult r = rgUploadGeometry (vulkan_globals.instance, &info);
		RG_CHECK (r);
	}

	R_ClearBatch (cbx);
	++(*brushpasses);
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

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (cb_context_t *cbx, qmodel_t *model, texchain_t chain)
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
			DrawGLPoly (
				cbx, RT_UNIQUEID_DONTCARE,
				s->polys, color, alpha, 
				&tr, NULL, 
				CVAR_TO_BOOL (r_showtris) ? DRAW_GL_POLY_TYPE_SHOWTRI : DRAW_GL_POLY_TYPE_SHOWTRI_NODEPTH);
	}
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	int         i;
	msurface_t *s;
	texture_t  *t;

	uint32_t brushpasses = 0;
	for (i = 0; i < model->numtextures; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;

		gltexture_t *lightmap_tex = NULL;
		R_ClearBatch (cbx);

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			if (model != cl.worldmodel)
			{
				// ericw -- this is copied from R_DrawSequentialPoly.
				// If the poly is not part of the world we have to
				// set this flag
				Atomic_StoreUInt32 (&t->update_warp, true); // FIXME: one frame too late!
			}
			
		    float alpha = GL_WaterAlphaForEntitySurface (ent, s);
			
			RT_UploadSurface (cbx, entuniqueid, ent, model, s, t->warpimage, lightmap_tex, NULL, false, alpha, false, true, &brushpasses);
		}
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
================
R_DrawTextureChains_Multitexture
================
*/
void R_DrawTextureChains_Multitexture (
	cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, const float alpha, int texstart, int texend, int entuniqueid)
{
	int          i;
	msurface_t  *s;
	texture_t   *t;
    qboolean     use_zbias = (gl_zfix.value && model != cl.worldmodel);
	int          ent_frame = ent != NULL ? ent->frame : 0;
	gltexture_t *fullbright_tex = NULL;
	
	uint32_t brushpasses = 0;
	for (i = texstart; i < texend; ++i)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		if (gl_fullbrights.value && (fullbright_tex = R_TextureAnimation (t, ent_frame)->fullbright) && !r_lightmap_cheatsafe)
		{
			assert (fullbright_tex);
		}
		else
		{
			fullbright_tex = NULL;
		}

		gltexture_t *lightmap_tex = NULL;
		R_ClearBatch (cbx);

		qboolean alpha_test = (t->texturechains[chain]->flags & SURF_DRAWFENCE) != 0;

		gltexture_t *diffuse_tex = R_TextureAnimation (t, ent_frame)->gltexture;

		for (s = t->texturechains[chain]; s; s = s->texturechains[chain])
		{
			RT_UploadSurface (cbx, entuniqueid, ent, model, s, diffuse_tex, lightmap_tex, fullbright_tex, alpha_test, alpha, use_zbias, false, &brushpasses);
		}
	}

	Atomic_AddUInt32 (&rs_brushpasses, brushpasses);
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (cb_context_t *cbx, qmodel_t *model, entity_t *ent, texchain_t chain, int entuniqueid)
{
	float entalpha;

	if (ent != NULL)
		entalpha = ENTALPHA_DECODE (ent->alpha);
	else
		entalpha = 1;

	if (!r_gpulightmapupdate.value)
		R_UploadLightmaps ();
	R_DrawTextureChains_Multitexture (cbx, model, ent, chain, entalpha, 0, model->numtextures, entuniqueid);
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
	R_DrawTextureChains_Multitexture (cbx, cl.worldmodel, NULL, chain_world, 1, world_texstart[index], world_texend[index], ENT_UNIQUEID_WORLD);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_BeginDebugUtilsLabel (cbx, "Water");
	R_DrawTextureChains_Water (cbx, cl.worldmodel, NULL, chain_world, ENT_UNIQUEID_WORLD);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (cb_context_t *cbx)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_ShowTris (cbx, cl.worldmodel, chain_world);
}
