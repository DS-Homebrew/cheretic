
// R_data.c

#include "doomdef.h"
#include "r_local.h"
#include "p_tick.h"
#include "p_mobj.h"
#include "map.h"
#include "ds_textures.h"

#ifdef ARM9
#include <alloca.h>
#else
#include <malloc.h>
#define	strcasecmp _strcmpi
#define	strncasecmp _strnicmp
#endif

extern void CheckAbortStartup(void);



int		firstflat, lastflat, numflats;
int		firstpatch, lastpatch, numpatches;
int		firstspritelump, lastspritelump, numspritelumps;

int			numtextures;
texture_t	**textures;
dstex_t		*textures_ds;
dstex_t		*flats_ds;
dstex_t		*sprites_ds;
int			*texturewidthmask;
fixed_t		*textureheight;		// needed for texture pegging
int			*texturecompositesize;
short		**texturecolumnlump;
unsigned short		**texturecolumnofs;
byte		**texturecomposite;

int			*flattranslation;		// for global animation
int			*texturetranslation;	// for global animation

fixed_t		*spritewidth;		// needed for pre rendering
fixed_t		*spriteheight;		// needed for pre rendering
fixed_t		*spriteoffset;
fixed_t		*spritetopoffset;

lighttable_t	*colormaps;


/*
==============================================================================

						MAPTEXTURE_T CACHING

when a texture is first needed, it counts the number of composite columns
required in the texture and allocates space for a column directory and any
new columns.  The directory will simply point inside other patches if there
is only one patch in a given column, but any columns with multiple patches
will have new column_ts generated.

==============================================================================
*/

/*
===================
=
= R_DrawColumnInCache
=
= Clip and draw a column from a patch into a cached post
=
===================
*/

void R_DrawColumnInCache (column_t *patch, byte *cache, int originy, int cacheheight)
{
	int		count, position;
	byte	*source;
		
	while (patch->topdelta != 0xff)
	{
		source = (byte *)patch + 3;
		count = patch->length;
		position = originy + patch->topdelta;
		if (position < 0)
		{
			count += position;
			position = 0;
		}
		if (position + count > cacheheight)
			count = cacheheight - position;
		if (count > 0)
			memcpy (cache + position, source, count);
		
		patch = (column_t *)(  (byte *)patch + patch->length
+ 4);
	}
}


/*
===================
=
= R_GenerateComposite
=
===================
*/

void R_GenerateComposite (int texnum)
{
	byte		*block;
	texture_t	*texture;
	texpatch_t	*patch;	
	patch_t		*realpatch;
	int			x, x1, x2;
	int			i;
	column_t	*patchcol;
	short		*collump;
	unsigned short *colofs;
	
	texture = textures[texnum];
	block = Z_Calloc (texturecompositesize[texnum], PU_STATIC, 
		&texturecomposite[texnum]);	
	collump = texturecolumnlump[texnum];
	colofs = texturecolumnofs[texnum];
		
//
// composite the columns together
//
	patch = texture->patches;
		
	for (i=0 , patch = texture->patches; i<texture->patchcount ; i++, patch++)
	{
		realpatch = W_CacheLumpNum (patch->patch, PU_CACHE);
		x1 = patch->originx;
		x2 = x1 + SHORT(realpatch->width);

		if (x1<0)
			x = 0;
		else
			x = x1;
		if (x2 > texture->width)
			x2 = texture->width;

		for ( ; x<x2 ; x++)
		{
			if (collump[x] >= 0)
				continue;		// column does not have multiple patches
			patchcol = (column_t *)((byte *)realpatch + 
				LONG(realpatch->columnofs[x-x1]));
			R_DrawColumnInCache (patchcol, block + colofs[x], patch->originy,
			texture->height);
		}
						
	}

// now that the texture has been built, it is purgable
	Z_ChangeTag (block, PU_CACHE);
}


/*
===================
=
= R_GenerateLookup
=
===================
*/

void R_GenerateLookup (int texnum)
{
	texture_t	*texture;
	byte		*patchcount;		// [texture->width]
	texpatch_t	*patch;	
	patch_t		*realpatch;
	int			x, x1, x2;
	int			i;
	short		*collump;
	unsigned short	*colofs;
	
	texture = textures[texnum];

	texturecomposite[texnum] = 0;	// composited not created yet
	texturecompositesize[texnum] = 0;
	collump = texturecolumnlump[texnum];
	colofs = texturecolumnofs[texnum];
	
//
// count the number of columns that are covered by more than one patch
// fill in the lump / offset, so columns with only a single patch are
// all done
//
	patchcount = (byte *)malloc (texture->width);
	memset (patchcount, 0, texture->width);
	patch = texture->patches;
		
	for (i=0 , patch = texture->patches; i<texture->patchcount ; i++, patch++)
	{
		realpatch = W_CacheLumpNum (patch->patch, PU_CACHE);
		x1 = patch->originx;
		x2 = x1 + SHORT(realpatch->width);
		if (x1 < 0)
			x = 0;
		else
			x = x1;
		if (x2 > texture->width)
			x2 = texture->width;
		for ( ; x<x2 ; x++)
		{
			patchcount[x]++;
			collump[x] = patch->patch;
			colofs[x] = LONG(realpatch->columnofs[x-x1])+3;
		}
	}
	
	for (x=0 ; x<texture->width ; x++)
	{
		if (!patchcount[x])
		{
			printf ("R_GenerateLookup: column without a patch (%s)\n", texture->name);
			return;
		}
//			I_Error ("R_GenerateLookup: column without a patch");
		if (patchcount[x] > 1)
		{
			collump[x] = -1;	// use the cached block
			colofs[x] = texturecompositesize[texnum];
			if (texturecompositesize[texnum] > 0x10000-texture->height)
				I_Error ("R_GenerateLookup: texture %i is >64k",texnum);
			texturecompositesize[texnum] += texture->height;
		}
	}
	free(patchcount);
}


/*
================
=
= R_GetColumn
=
================
*/

byte *R_GetColumn (int tex, int col)
{
	int	lump, ofs;
	
	col &= texturewidthmask[tex];
	lump = texturecolumnlump[tex][col];
	ofs = texturecolumnofs[tex][col];
	if (lump > 0)
		return (byte *)W_CacheLumpNum(lump,PU_CACHE)+ofs;
	if (!texturecomposite[tex])
		R_GenerateComposite (tex);
	return texturecomposite[tex] + ofs;
}


/*
==================
=
= R_InitTextures
=
= Initializes the texture list with the textures from the world map
=
==================
*/

void R_InitTextures (void)
{
	maptexture_t	*mtexture;
	texture_t		*texture;
	dstex_t			*texture_ds;
	mappatch_t	*mpatch;
	texpatch_t	*patch;
	int			i,j;
	int			*maptex, *maptex2, *maptex1;
	char		name[9], *names, *name_p;
	int			*patchlookup;
	int			totalwidth;
	int			nummappatches;
	int			offset, maxoff, maxoff2;
	int			numtextures1, numtextures2;
	int			*directory;

//
// load the patch names from pnames.lmp
//
	name[8] = 0;
	names = W_CacheLumpName ("PNAMES", PU_STATIC);
	nummappatches = LONG ( *((int *)names) );
	name_p = names+4;
	patchlookup = malloc (nummappatches*sizeof(*patchlookup));
	if(patchlookup == 0) {
		I_Error("patchlookup == 0\n");
	}
	for (i=0 ; i<nummappatches ; i++)
	{
		strncpy (name,name_p+i*8, 8);
		patchlookup[i] = W_CheckNumForName (name);
	}
	Z_Free (names);

//
// load the map texture definitions from textures.lmp
//
	maptex = maptex1 = W_CacheLumpName ("TEXTURE1", PU_STATIC);
	numtextures1 = LONG(*maptex);
	maxoff = W_LumpLength (W_GetNumForName ("TEXTURE1"));
	directory = maptex+1;

	if (W_CheckNumForName ("TEXTURE2") != -1)
	{
		maptex2 = W_CacheLumpName ("TEXTURE2", PU_STATIC);
		numtextures2 = LONG(*maptex2);
		maxoff2 = W_LumpLength (W_GetNumForName ("TEXTURE2"));
	}
	else
	{
		maptex2 = NULL;
		numtextures2 = 0;
		maxoff2 = 0;
	}
	numtextures = numtextures1 + numtextures2;

	printf("num textures %d\n",numtextures);

	//
	//	Init the startup thermometer at this point...
	//
	{
		int	spramount;
		spramount = W_GetNumForName("S_END") - W_GetNumForName("S_START") + 1;
		InitThermo(spramount + numtextures + 6);
	}

	textures = Z_Calloc (numtextures*4, PU_STATIC, 0);
	textures_ds = Z_Calloc(numtextures*sizeof(*textures_ds), PU_STATIC, 0);
	texturecolumnlump = Z_Calloc(numtextures*4, PU_STATIC, 0);
	texturecolumnofs = Z_Calloc(numtextures*4, PU_STATIC, 0);
	texturecomposite = Z_Calloc(numtextures*4, PU_STATIC, 0);
	texturecompositesize = Z_Calloc(numtextures*4, PU_STATIC, 0);
	texturewidthmask = Z_Calloc(numtextures*4, PU_STATIC, 0);
	textureheight = Z_Calloc(numtextures*4, PU_STATIC, 0);

	totalwidth = 0;

	for (i=0 ; i<numtextures ; i++, directory++)
	{
		#ifdef __NEXT__
		if(!(i&63))
			printf (".");
		#else
		IncThermo();
		#endif
		if (i == numtextures1)
		{	// start looking in second texture file
			maptex = maptex2;
			maxoff = maxoff2;
			directory = maptex+1;
		}

		offset = LONG(*directory);
		if (offset > maxoff)
			I_Error ("R_InitTextures: bad texture directory");
		mtexture = (maptexture_t *) ( (byte *)maptex + offset);
		texture = textures[i] = Z_Calloc (sizeof(texture_t) 
			+ sizeof(texpatch_t)*(SHORT(mtexture->patchcount)-1), PU_STATIC,
			0);
		texture->width = SHORT(mtexture->width);
		texture->height = SHORT(mtexture->height);
		texture->patchcount = SHORT(mtexture->patchcount);
		texture_ds = &textures_ds[i];
		ds_init_texture(i,texture,texture_ds);
		memcpy (texture->name, mtexture->name, sizeof(texture->name));
		//printf("%d: %p %8s %p %8s\n",i,texture->name,texture->name,mtexture->name,mtexture->name);
		if(*texture->name == 0)
		{
			while(1);
		}
		mpatch = &mtexture->patches[0];
		patch = &texture->patches[0];
		for (j=0 ; j<texture->patchcount ; j++, mpatch++, patch++)
		{
			patch->originx = SHORT(mpatch->originx);
			patch->originy = SHORT(mpatch->originy);
			patch->patch = patchlookup[SHORT(mpatch->patch)];
			if (patch->patch == -1)
				I_Error (
				"R_InitTextures: Missing patch in texture %s",texture->name);
		}		
		texturecolumnlump[i] = Z_Calloc (texture->width*2, PU_STATIC,0);
		texturecolumnofs[i] = Z_Calloc (texture->width*2, PU_STATIC,0);
		j = 1;
		while (j*2 <= texture->width)
			j<<=1;
		texturewidthmask[i] = j-1;
		textureheight[i] = texture->height<<FRACBITS;
		
		totalwidth += texture->width;
	}

	free(patchlookup);
	Z_Free (maptex1);
	if (maptex2)
		Z_Free (maptex2);

//
// precalculate whatever possible
//		
	for(i = 0; i < numtextures; i++)
	{
		R_GenerateLookup(i);
		CheckAbortStartup();
	}

//
// translation table for global animation
//
	texturetranslation = Z_Calloc ((numtextures+1)*4, PU_STATIC, 0);
	for (i=0 ; i<numtextures ; i++)
		texturetranslation[i] = i;

	ds_init_textures();
}


/*
================
=
= R_InitFlats
=
=================
*/

void R_InitFlats (void)
{
	dstex_t			*flat_ds;
	int		i;
	
	firstflat = W_GetNumForName ("F_START") + 1;
	lastflat = W_GetNumForName ("F_END") - 1;
	numflats = lastflat - firstflat + 1;
	
	flats_ds = Z_Calloc (numflats*sizeof(*textures_ds), PU_STATIC, 0);
// translation table for global animation
	flattranslation = Z_Calloc ((numflats+1)*4, PU_STATIC, 0);
	for (i=0 ; i<numflats ; i++)
	{
		flattranslation[i] = i;
		flat_ds = &flats_ds[i];
		ds_init_flat(i,flat_ds);
	}
}


/*
================
=
= R_InitSpriteLumps
=
= Finds the width and hoffset of all sprites in the wad, so the sprite doesn't
= need to be cached just for the header during rendering
=================
*/

void R_InitSpriteLumps (void)
{
	int		i;
	patch_t	*patch;
	dstex_t			*sprite_ds;

	firstspritelump = W_GetNumForName ("S_START") + 1;
	lastspritelump = W_GetNumForName ("S_END") - 1;
	numspritelumps = lastspritelump - firstspritelump + 1;
	spritewidth = (fixed_t *)Z_Calloc(numspritelumps*4, PU_STATIC, 0);
	spriteheight = (fixed_t *)Z_Calloc(numspritelumps*4, PU_STATIC, 0);
	spriteoffset = (fixed_t *)Z_Calloc(numspritelumps*4, PU_STATIC, 0);
	spritetopoffset = (fixed_t *)Z_Calloc(numspritelumps*4, PU_STATIC, 0);
	sprites_ds = (dstex_t *)Z_Calloc(numspritelumps*sizeof(dstex_t), PU_STATIC, 0);

	for (i=0 ; i< numspritelumps ; i++)
	{
		#ifdef __NEXT__
		if (!(i&63))
			printf (".");
		#else
		IncThermo();
		#endif
		patch = (patch_t *)W_CacheLumpNum (firstspritelump+i, PU_CACHE);
		spritewidth[i] = SHORT(patch->width)<<FRACBITS;
		spriteheight[i] = SHORT(patch->height)<<FRACBITS;
		spriteoffset[i] = SHORT(patch->leftoffset)<<FRACBITS;
		spritetopoffset[i] = SHORT(patch->topoffset)<<FRACBITS;
		sprite_ds = &sprites_ds[i];
		ds_init_sprite(i,patch,sprite_ds);
	}
}


/*
================
=
= R_InitColormaps
=
=================
*/

void R_InitColormaps (void)
{
	int	lump, length;
//
// load in the light tables
// 256 byte align tables
//
	lump = W_GetNumForName("COLORMAP");
	length = W_LumpLength (lump) + 255;
	colormaps = Z_Calloc(length, PU_STATIC, 0);
	colormaps = (byte *)( ((int)colormaps + 255)&~0xff);
	W_ReadLump (lump,colormaps);
}


/*
================
=
= R_InitData
=
= Locates all the lumps that will be used by all views
= Must be called after W_Init
=================
*/

void R_InitData (void)
{
	tprintf("\nR_InitTextures ",0);
	R_InitTextures ();
//printf (".");
	tprintf("R_InitFlats\n",0);
	R_InitFlats ();
	IncThermo();
//printf (".");
	tprintf("R_InitSpriteLumps ",0);
	R_InitSpriteLumps ();
	IncThermo();
//printf (".");
	R_InitColormaps ();
}


//=============================================================================

/*
================
=
= R_FlatNumForName
=
================
*/

int	R_FlatNumForName (char *name)
{
	int		i;
	char	namet[9];

	i = W_CheckNumForName (name);
	if (i == -1)
	{
		namet[8] = 0;
		memcpy (namet, name,8);
		I_Error ("R_FlatNumForName: %s not found",namet);
	}
	return i - firstflat;
}


/*
================
=
= R_CheckTextureNumForName
=
================
*/

int	R_CheckTextureNumForName (char *name)
{
	int		i;

	if (name[0] == '-')		// no texture marker
		return 0;
		
	for (i=0 ; i<numtextures ; i++)
	{
		//printf("t: %8s\n",textures[i]->name);
		if (!strncasecmp (textures[i]->name, name, 8) )
			return i;
	}
		
	return -1;
}


/*
================
=
= R_TextureNumForName
=
================
*/

int	R_TextureNumForName (char *name)
{
	int		i;
	//char	namet[9];
	
	i = R_CheckTextureNumForName (name);
	if (i==-1)
		I_Error ("R_TextureNumForName: %s not found",name);
	
	return i;
}


/*
=================
=
= R_PrecacheLevel
=
= Preloads all relevent graphics for the level
=================
*/

int		flatmemory, texturememory, spritememory;

void R_PrecacheLevel (void)
{
	char			*flatpresent;
	char			*texturepresent;
	char			*spritepresent;
	int				i,j,k, lump;
	texture_t		*texture;
	thinker_t		*th;
	spriteframe_t	*sf;

	if (demoplayback)
		return;
			
//
// precache flats
//	
	flatpresent = malloc(numflats);
	memset (flatpresent,0,numflats);	
	for (i=0 ; i<numsectors ; i++)
	{
		flatpresent[sectors[i].floorpic] = 1;
		flatpresent[sectors[i].ceilingpic] = 1;
	}
	
	flatmemory = 0;
	for (i=0 ; i<numflats ; i++)
		if (flatpresent[i])
		{
			lump = firstflat + i;
			flatmemory += lumpinfo[lump].size;
			W_CacheLumpNum(lump, PU_CACHE);
		}
		
	free(flatpresent);
//
// precache textures
//
	texturepresent = malloc(numtextures);
	memset (texturepresent,0, numtextures);
	
	for (i=0 ; i<numsides ; i++)
	{
		texturepresent[sides[i].toptexture] = 1;
		texturepresent[sides[i].midtexture] = 1;
		texturepresent[sides[i].bottomtexture] = 1;
	}
	
	texturepresent[skytexture] = 1;
	
	texturememory = 0;
	for (i=0 ; i<numtextures ; i++)
	{
		if (!texturepresent[i])
			continue;
		texture = textures[i];
		for (j=0 ; j<texture->patchcount ; j++)
		{
			lump = texture->patches[j].patch;
			texturememory += lumpinfo[lump].size;
			W_CacheLumpNum(lump , PU_CACHE);
		}
	}

	free(texturepresent);
	
//
// precache sprites
//
	spritepresent = malloc(numsprites);
	memset (spritepresent,0, numsprites);
	
	for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
	{
		if (th->function == P_MobjThinker)
			spritepresent[((mobj_t *)th)->sprite] = 1;
	}
	
	spritememory = 0;
	for (i=0 ; i<numsprites ; i++)
	{
		if (!spritepresent[i])
			continue;
		for (j=0 ; j<sprites[i].numframes ; j++)
		{
			sf = &sprites[i].spriteframes[j];
			for (k=0 ; k<8 ; k++)
			{
				lump = firstspritelump + sf->lump[k];
				spritememory += lumpinfo[lump].size;
				W_CacheLumpNum(lump , PU_CACHE);
			}
		}
	}

	free(spritepresent);
}




