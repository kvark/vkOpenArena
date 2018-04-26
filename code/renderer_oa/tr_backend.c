/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "tr_local.h"
#include "../sdl/sdl_glimp.h"

// AVI files have the start of pixel lines 4 byte-aligned
#define AVI_LINE_PADDING 4

extern shaderCommands_t tess;
extern cvar_t* r_fastsky;
extern cvar_t* r_debugSurface; //tr_init

extern trGlobals_t tr;
extern glconfig_t glConfig;
extern refimport_t ri;
// outside of TR since it shouldn't be cleared during ref re-init
extern glstate_t glState;

cvar_t* r_measureOverdraw;		// enables stencil buffer overdraw measurement
cvar_t* r_speeds; // various levels of information display
backEndState_t	backEnd;


static cvar_t* r_clear;	// force screen clear every frame
static cvar_t* r_showImages;
static cvar_t* r_finish;
static cvar_t* r_screenshotJpegQuality;
static cvar_t* r_aviMotionJpegQuality;
static cvar_t* r_drawSun;
// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=516
//extern const void* RB_TakeScreenshotCmd( const void *data );
//extern const void* RB_TakeVideoFrameCmd( const void *data );


const static float s_flipMatrix[16] =
{
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};


//void Matrix4Copy(const float in[16], float out[16]);

static ID_INLINE void Matrix4Copy(const float* in, float* out)
{
    /*
    out[ 0] = in[ 0];       out[ 4] = in[ 4];       out[ 8] = in[ 8];       out[12] = in[12];
    out[ 1] = in[ 1];       out[ 5] = in[ 5];       out[ 9] = in[ 9];       out[13] = in[13];
    out[ 2] = in[ 2];       out[ 6] = in[ 6];       out[10] = in[10];       out[14] = in[14];
    out[ 3] = in[ 3];       out[ 7] = in[ 7];       out[11] = in[11];       out[15] = in[15];
    */

    memcpy(out, in, 16*sizeof(float));
}


/*
static void GL_BindMultitexture( image_t *image0, GLuint env0, image_t *image1, GLuint env1 )
{
	int	texnum0 = image0->texnum;
    int texnum1 = image1->texnum;

	if( r_nobind->integer && tr.dlightImage )
    {		// performance evaluation option
		texnum0 = texnum1 = tr.dlightImage->texnum;
	}

	if( glState.currenttextures[1] != texnum1 )
    {
		GL_SelectTexture( 1 );
		image1->frameUsed = tr.frameCount;
		glState.currenttextures[1] = texnum1;
		qglBindTexture( GL_TEXTURE_2D, texnum1 );
	}
	if( glState.currenttextures[0] != texnum0 )
    {
		GL_SelectTexture( 0 );
		image0->frameUsed = tr.frameCount;
		glState.currenttextures[0] = texnum0;
		qglBindTexture( GL_TEXTURE_2D, texnum0 );
	}
}
*/

/*
================
RB_Hyperspace

A player has predicted a teleport, but hasn't arrived yet
================
*/
static void RB_Hyperspace( void )
{
	if ( !backEnd.isHyperspace ) {
		// do initialization shit
	}

	float c = ( backEnd.refdef.time & 255 ) / 255.0f;
	qglClearColor( c, c, c, 1 );
	qglClear( GL_COLOR_BUFFER_BIT );

	backEnd.isHyperspace = qtrue;
}


static void SetViewportAndScissor( void )
{
	qglMatrixMode(GL_PROJECTION);
	qglLoadMatrixf( backEnd.viewParms.projectionMatrix );
	qglMatrixMode(GL_MODELVIEW);

	// set the window clipping
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	Matrix4Copy(backEnd.viewParms.projectionMatrix, glState.currentProjectionMatrix);
    MatrixMultiply4x4(glState.currentModelViewMatrix, glState.currentProjectionMatrix, glState.currentModelViewProjectionMatrix);
}


/*
 * RB_BeginDrawingView: Any mirrored or portaled views have already been drawn, 
 * so prepare to actually render the visible surfaces for this view
 */
static void RB_BeginDrawingView(void)
{
	//int clearBits = 0;

	// sync with gl if needed
	if ( r_finish->integer == 0 )
    {
		glState.finishCalled = qtrue;
	}
    else if( !glState.finishCalled )
    {
        qglFinish();
		glState.finishCalled = qtrue;
    }

	// we will need to change the projection matrix before drawing 2D images again
	backEnd.projection2D = qfalse;

	//
	// set the modelview matrix for the viewer
	//
	SetViewportAndScissor();

	// ensures that depth writes are enabled for the depth clear
	GL_State( GLS_DEFAULT );
	// clear relevant buffers
	int clearBits = GL_DEPTH_BUFFER_BIT;

	if ( r_measureOverdraw->integer )
	{
		clearBits |= GL_STENCIL_BUFFER_BIT;
	}
	if ( r_fastsky->integer && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) )
	{
		clearBits |= GL_COLOR_BUFFER_BIT;	// FIXME: only if sky shaders have been used
#ifdef _DEBUG
		qglClearColor( 0.8f, 0.7f, 0.4f, 1.0f );	// FIXME: get color of sky
#else
		qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );	// FIXME: get color of sky
#endif
	}
	qglClear( clearBits );

	if( ( backEnd.refdef.rdflags & RDF_HYPERSPACE ) )
	{
		RB_Hyperspace();
		return;
	}
	else
	{
		backEnd.isHyperspace = qfalse;
	}

	glState.faceCulling = -1;		// force face culling to set next time

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;

	// clip to the plane of the portal
	if ( backEnd.viewParms.isPortal )
    {
		float plane[4];
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;
		
        double plane2[4];
		plane2[0] = DotProduct (backEnd.viewParms.or.axis[0], plane);
		plane2[1] = DotProduct (backEnd.viewParms.or.axis[1], plane);
		plane2[2] = DotProduct (backEnd.viewParms.or.axis[2], plane);
		plane2[3] = DotProduct (plane, backEnd.viewParms.or.origin) - plane[3];

		qglLoadMatrixf( s_flipMatrix );
		Matrix4Copy(s_flipMatrix, glState.currentModelViewMatrix);
		MatrixMultiply4x4(glState.currentModelViewMatrix, glState.currentProjectionMatrix, glState.currentModelViewProjectionMatrix);
		qglClipPlane(GL_CLIP_PLANE0, plane2);
		qglEnable(GL_CLIP_PLANE0);
	}
    else
    {
		qglDisable (GL_CLIP_PLANE0);
	}
}




static void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs )
{
	shader_t* shader;
	int				fogNum;
	int				entityNum;
	int				dlighted;
	qboolean		isCrosshair;
	int				i;
	drawSurf_t* drawSurf;

	// save original time for entity shader offsets
	float originalTime = backEnd.refdef.floatTime;

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView();

	// draw everything
	int oldEntityNum = -1;
    int oldFogNum = -1;
	int oldSort = -1;
    
    shader_t *oldShader = NULL;
	
    qboolean oldDepthRange = qfalse;
	qboolean wasCrosshair = qfalse;
	qboolean oldDlighted = qfalse;
    qboolean depthRange = qfalse;
	
    backEnd.currentEntity = &tr.worldEntity;	
	backEnd.pc.c_surfaces += numDrawSurfs;

	for(i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++)
    {
		if(drawSurf->sort == oldSort)
        {
			// fast path, same as previous sort
			rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );
			continue;
		}
		oldSort = drawSurf->sort;
		R_DecomposeSort( drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted );

		//
		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from seperate
		// entities merged into a single batch, like smoke and blood puff sprites
		if ( (shader != NULL) && ( shader != oldShader || fogNum != oldFogNum || dlighted != oldDlighted 
			|| ( entityNum != oldEntityNum && !shader->entityMergable ) ) )
        {
			if (oldShader != NULL) 
				RB_EndSurface();
			
			RB_BeginSurface( shader, fogNum );
			oldShader = shader;
			oldFogNum = fogNum;
			oldDlighted = dlighted;
		}

		//
		// change the modelview matrix if needed
		//
		if( entityNum != oldEntityNum )
        {
			depthRange = isCrosshair = qfalse;

			if ( entityNum != REFENTITYNUM_WORLD )
            {
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];
				backEnd.refdef.floatTime = originalTime - backEnd.currentEntity->e.shaderTime;
				// we have to reset the shaderTime as well otherwise image animations start
				// from the wrong frame
				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

				// set up the transformation matrix
				R_RotateForEntity( backEnd.currentEntity, &backEnd.viewParms, &backEnd.or );

				// set up the dynamic lighting if needed
				if ( backEnd.currentEntity->needDlights )
                {
					R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
				}

				if(backEnd.currentEntity->e.renderfx & RF_DEPTHHACK)
				{
					// hack the depth range to prevent view model from poking into walls
					depthRange = qtrue;
					
					if(backEnd.currentEntity->e.renderfx & RF_CROSSHAIR)
						isCrosshair = qtrue;
				}
			}
            else
            {
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd.or = backEnd.viewParms.world;
				// we have to reset the shaderTime as well otherwise image animations on
				// the world (like water) continue with the wrong frame
				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
				R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
			}

			qglLoadMatrixf( backEnd.or.modelMatrix );

			Matrix4Copy(backEnd.or.modelMatrix, glState.currentModelViewMatrix);
			MatrixMultiply4x4(glState.currentModelViewMatrix, glState.currentProjectionMatrix, glState.currentModelViewProjectionMatrix);

			//
			// change depthrange. Also change projection matrix so first person weapon does not look like coming
			// out of the screen.
			//
			if (oldDepthRange != depthRange || wasCrosshair != isCrosshair)
			{
				if (depthRange)
				{
					if(!oldDepthRange)
						qglDepthRange (0, 0.3);
				}
				else
				{
					qglDepthRange (0, 1);
				}

				oldDepthRange = depthRange;
				wasCrosshair = isCrosshair;
			}

			oldEntityNum = entityNum;
		}

		// add the triangles for this surface
		rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );
	}

	backEnd.refdef.floatTime = originalTime;

	// draw the contents of the last shader batch
	if (oldShader != NULL)
    {
		RB_EndSurface();
	}

	// go back to the world modelview matrix
	qglLoadMatrixf( backEnd.viewParms.world.modelMatrix );
	Matrix4Copy(backEnd.viewParms.world.modelMatrix, glState.currentModelViewMatrix);
	MatrixMultiply4x4(glState.currentModelViewMatrix, glState.currentProjectionMatrix, glState.currentModelViewProjectionMatrix);
	if ( depthRange )
    {
		qglDepthRange (0, 1);
	}

	if (r_drawSun->integer) {
		RB_DrawSun(0.1, tr.sunShader);
	}
	// add light flares on lights that aren't obscured
	RB_RenderFlares();
}



/*
============================================================================

RENDER BACK END FUNCTIONS

============================================================================
*/


static void RB_SetGL2D (void)
{
	backEnd.projection2D = qtrue;

	// set 2D virtual screen size

	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	
	qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();
	qglOrtho(0, glConfig.vidWidth, glConfig.vidHeight, 0, 0, 1);
	qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();


    qglGetFloatv(GL_PROJECTION_MATRIX, glState.currentProjectionMatrix);
    qglGetFloatv(GL_MODELVIEW_MATRIX, glState.currentModelViewMatrix);
    MatrixMultiply4x4(glState.currentModelViewMatrix, glState.currentProjectionMatrix,  glState.currentModelViewProjectionMatrix);
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );

	qglDisable( GL_CULL_FACE );
	qglDisable( GL_CLIP_PLANE0 );

	// set time for 2D shaders
	backEnd.refdef.time = ri.Milliseconds();
	backEnd.refdef.floatTime = backEnd.refdef.time * 0.001f;
}



static const void *RB_SetColor( const void *data )
{
	const setColorCommand_t	*cmd= (const setColorCommand_t *)data;

	backEnd.color2D[0] = cmd->color[0] * 255;
	backEnd.color2D[1] = cmd->color[1] * 255;
	backEnd.color2D[2] = cmd->color[2] * 255;
	backEnd.color2D[3] = cmd->color[3] * 255;

	return (const void *)(cmd + 1);
}


static const void *RB_StretchPic( const void *data )
{
	const stretchPicCommand_t *cmd = (const stretchPicCommand_t *)data;

	if ( !backEnd.projection2D )
    {
		RB_SetGL2D();
	}

	shader_t* shader = cmd->shader;
	if ( shader != tess.shader )
    {
		if ( tess.numIndexes )
        {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader, 0 );
	}

	//RB_CHECKOVERFLOW( 4, 6 );
    if ( (tess.numVertexes + 4 >= SHADER_MAX_VERTEXES) || (tess.numIndexes + 6 >= SHADER_MAX_INDEXES) )
    {
        RB_CheckOverflow(4,6);
    }
	int numVerts = tess.numVertexes;
	int numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes += 6;

	tess.indexes[ numIndexes ] = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	*(int *)tess.vertexColors[ numVerts ] =
		*(int *)tess.vertexColors[ numVerts + 1 ] =
		*(int *)tess.vertexColors[ numVerts + 2 ] =
		*(int *)tess.vertexColors[ numVerts + 3 ] = *(int *)backEnd.color2D;

	tess.xyz[ numVerts ][0] = cmd->x;
	tess.xyz[ numVerts ][1] = cmd->y;
	tess.xyz[ numVerts ][2] = 0;

	tess.texCoords[ numVerts ][0][0] = cmd->s1;
	tess.texCoords[ numVerts ][0][1] = cmd->t1;

	tess.xyz[ numVerts + 1 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 1 ][1] = cmd->y;
	tess.xyz[ numVerts + 1 ][2] = 0;

	tess.texCoords[ numVerts + 1 ][0][0] = cmd->s2;
	tess.texCoords[ numVerts + 1 ][0][1] = cmd->t1;

	tess.xyz[ numVerts + 2 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 2 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 2 ][2] = 0;

	tess.texCoords[ numVerts + 2 ][0][0] = cmd->s2;
	tess.texCoords[ numVerts + 2 ][0][1] = cmd->t2;

	tess.xyz[ numVerts + 3 ][0] = cmd->x;
	tess.xyz[ numVerts + 3 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 3 ][2] = 0;

	tess.texCoords[ numVerts + 3 ][0][0] = cmd->s1;
	tess.texCoords[ numVerts + 3 ][0][1] = cmd->t2;

	return (const void *)(cmd + 1);
}



static const void *RB_DrawSurfs(const void *data)
{
	// finish any 2D drawing if needed
	if ( tess.numIndexes )
    {
		RB_EndSurface();
	}

	const drawSurfsCommand_t* cmd = (const drawSurfsCommand_t *)data;

	backEnd.refdef = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	//TODO Maybe check for rdf_noworld stuff but q3mme has full 3d ui
	RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

	return (const void *)(cmd + 1);
}



static const void* RB_DrawBuffer(const void *data)
{
	const drawBufferCommand_t* cmd = (const drawBufferCommand_t *)data;

	qglDrawBuffer( cmd->buffer );

	// clear screen for debugging
	if ( r_clear->integer )
    {
		qglClearColor( 1, 0, 0.5, 1 );
		qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	}

	return (const void *)(cmd + 1);
}



static const void *RB_ColorMask(const void *data)
{
	const colorMaskCommand_t *cmd = data;
	
	qglColorMask(cmd->rgba[0], cmd->rgba[1], cmd->rgba[2], cmd->rgba[3]);
	
	return (const void *)(cmd + 1);
}


static const void *RB_ClearDepth(const void *data)
{
	const clearDepthCommand_t *cmd = data;
	
	if(tess.numIndexes)
		RB_EndSurface();

	// texture swapping test
	if (r_showImages->integer)
		RB_ShowImages();

	qglClear(GL_DEPTH_BUFFER_BIT);
	
	return (const void *)(cmd + 1);
}


static const void* RB_SwapBuffers(const void *data)
{
	// finish any 2D drawing if needed
	if( tess.numIndexes )
    {
		RB_EndSurface();
	}

	// texture swapping test
	if( r_showImages->integer )
    {
		RB_ShowImages();
	}


	const swapBuffersCommand_t *cmd = (const swapBuffersCommand_t *)data;

	// we measure overdraw by reading back the stencil buffer and
	// counting up the number of increments that have happened
	if ( r_measureOverdraw->integer )
    {
		long sum = 0;
		unsigned char *stencilReadback;

		stencilReadback = ri.Hunk_AllocateTempMemory( glConfig.vidWidth * glConfig.vidHeight );
		qglReadPixels( 0, 0, glConfig.vidWidth, glConfig.vidHeight, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencilReadback );
        
        int i;
		for( i = 0; i < glConfig.vidWidth * glConfig.vidHeight; i++ ) {
			sum += stencilReadback[i];
		}

		backEnd.pc.c_overDraw += sum;
		ri.Hunk_FreeTempMemory( stencilReadback );
	}


	if ( !glState.finishCalled ) {
		qglFinish();
	}

	GLimp_EndFrame();

	backEnd.projection2D = qfalse;


	

	return (const void *)(cmd + 1);
}


/*
==============================================================================
						SCREEN SHOTS

NOTE TTimo
some thoughts about the screenshots system:
screenshots get written in fs_homepath + fs_gamedir
vanilla q3 .. baseq3/screenshots/ *.tga
team arena .. missionpack/screenshots/ *.tga

two commands: "screenshot" and "screenshotJPEG"
we use statics to store a count and start writing the first screenshot/screenshot????.tga (.jpg) available
(with FS_FileExists / FS_FOpenFileWrite calls)
FIXME: the statics don't get a reinit between fs_game changes

==============================================================================
*/
static void RB_TakeScreenshot(int x, int y, int width, int height, char *fileName)
{
	unsigned char *destptr, *endline;
	
	int padlen;
	size_t offset = 18, memcount;

	unsigned char* allbuf = RB_ReadPixels(x, y, width, height, &offset, &padlen);
	unsigned char* buffer = allbuf + offset - 18;

	memset (buffer, 0, 18);
	buffer[2] = 2;		// uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;	// pixel size

	// swap rgb to bgr and remove padding from line endings
	int linelen = width * 3;

	unsigned char* srcptr = destptr = allbuf + offset;
	unsigned char* endmem = srcptr + (linelen + padlen) * height;

	while(srcptr < endmem)
    {
		endline = srcptr + linelen;

		while(srcptr < endline)
        {
			unsigned char temp = srcptr[0];
			*destptr++ = srcptr[2];
			*destptr++ = srcptr[1];
			*destptr++ = temp;

			srcptr += 3;
		}

		// Skip the pad
		srcptr += padlen;
	}

	memcount = linelen * height;

	// gamma correct
	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect(allbuf + offset, memcount);
	}

	ri.FS_WriteFile(fileName, buffer, memcount + 18);

	ri.Hunk_FreeTempMemory(allbuf);
}



static void RB_TakeScreenshotJPEG(int x, int y, int width, int height, char *fileName)
{
	size_t offset = 0;
	int padlen;
	unsigned char* buffer = RB_ReadPixels(x, y, width, height, &offset, &padlen);
	size_t memcount = (width * 3 + padlen) * height;

	// gamma correct
	if(glConfig.deviceSupportsGamma)
		R_GammaCorrect(buffer + offset, memcount);

	RE_SaveJPG(fileName, r_screenshotJpegQuality->integer, width, height, buffer + offset, padlen);
	ri.Hunk_FreeTempMemory(buffer);
}


static const void *RB_TakeScreenshotCmd( const void *data )
{
	const screenshotCommand_t *cmd = (const screenshotCommand_t *)data;

	if (cmd->jpeg)
		RB_TakeScreenshotJPEG( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName);
	else
		RB_TakeScreenshot( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName);

	return (const void *)(cmd + 1);
}


static const void *RB_TakeVideoFrameCmd( const void *data )
{
	const videoFrameCommand_t* cmd = (const videoFrameCommand_t *)data;
	GLint packAlign;
	qglGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);

	size_t linelen = cmd->width * 3;

	// Alignment stuff for glReadPixels
	int padwidth = PAD(linelen, packAlign);
	int padlen = padwidth - linelen;
	// AVI line padding
	int avipadwidth = PAD(linelen, AVI_LINE_PADDING);
	int avipadlen = avipadwidth - linelen;

	unsigned char* cBuf = PADP(cmd->captureBuffer, packAlign);

	qglReadPixels(0, 0, cmd->width, cmd->height, GL_RGB, GL_UNSIGNED_BYTE, cBuf);

	size_t memcount = padwidth * cmd->height;

	// gamma correct
	if(glConfig.deviceSupportsGamma)
		R_GammaCorrect(cBuf, memcount);

	if(cmd->motionJpeg)
    {
		memcount = RE_SaveJPGToBuffer(cmd->encodeBuffer, linelen * cmd->height, r_aviMotionJpegQuality->integer, cmd->width, cmd->height, cBuf, padlen);
		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, memcount);
	}
	else
    {
		unsigned char* lineend;

		unsigned char* srcptr = cBuf;
		unsigned char* destptr = cmd->encodeBuffer;
		unsigned char* memend = srcptr + memcount;

		// swap R and B and remove line paddings
		while(srcptr < memend)
        {
			lineend = srcptr + linelen;
			while(srcptr < lineend)
            {
				*destptr++ = srcptr[2];
				*destptr++ = srcptr[1];
				*destptr++ = srcptr[0];
				srcptr += 3;
			}

			memset(destptr, '\0', avipadlen);
			destptr += avipadlen;

			srcptr += padlen;
		}

		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, avipadwidth * cmd->height);
	}

	return (const void *)(cmd + 1);
}



/////////////////////////////////////////////////////////////////////////////////

void GL_Bind(image_t *image)
{
	if ( glState.currenttextures[glState.currenttmu] != image->texnum )
    {
		image->frameUsed = tr.frameCount;
		qglBindTexture(GL_TEXTURE_2D, image->texnum);
        glState.currenttextures[glState.currenttmu] = image->texnum;
	}
}


/*
** GL_SelectTexture
*/
void GL_SelectTexture( int unit )
{
	if ( glState.currenttmu == unit )
	{
		return;
	}

	if ( unit == 0 )
	{
		glActiveTextureARB( GL_TEXTURE0_ARB );
		glClientActiveTextureARB( GL_TEXTURE0_ARB );
	}
	else if ( unit == 1 )
    {
		glActiveTextureARB( GL_TEXTURE1_ARB );
		glClientActiveTextureARB( GL_TEXTURE1_ARB );
    }
    else
		ri.Error( ERR_DROP, "GL_SelectTexture: unit = %i", unit );


	glState.currenttmu = unit;
}



void GL_Cull( int cullType )
{
	if( glState.faceCulling == cullType )
    {
		return;
	}

	glState.faceCulling = cullType;

	if( cullType == CT_TWO_SIDED ) 
	{
		qglDisable( GL_CULL_FACE );
	} 
	else 
	{
		qboolean cullFront;
		qglEnable( GL_CULL_FACE );

		cullFront = (cullType == CT_FRONT_SIDED);
		if ( backEnd.viewParms.isMirror )
		{
			cullFront = !cullFront;
		}

		qglCullFace( cullFront ? GL_FRONT : GL_BACK );
	}
}


void GL_TexEnv( int env )
{
	if ( env == glState.texEnv[glState.currenttmu] )
	{
		return;
	}

	glState.texEnv[glState.currenttmu] = env;


	switch ( env )
	{
        case GL_MODULATE:
            qglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
            break;
        case GL_REPLACE:
            qglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
            break;
        case GL_DECAL:
            qglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL );
            break;
        case GL_ADD:
            qglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD );
            break;
        default:
            ri.Error( ERR_DROP, "GL_TexEnv: invalid env '%d' passed", env );
            break;
	}
}


/*
** GL_State
**
** This routine is responsible for setting the most commonly changed state in Q3.
*/
void GL_State( unsigned long stateBits )
{
	unsigned long diff = stateBits ^ glState.glStateBits;

	if ( !diff )
	{
		return;
	}

	//
	// check depthFunc bits
	//
	if ( diff & GLS_DEPTHFUNC_EQUAL )
	{
		if ( stateBits & GLS_DEPTHFUNC_EQUAL )
		{
			qglDepthFunc( GL_EQUAL );
		}
		else
		{
			qglDepthFunc( GL_LEQUAL );
		}
	}

	//
	// check blend bits
	//
	if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
	{
		GLenum srcFactor = GL_ONE, dstFactor = GL_ONE;

		if ( stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
		{
			switch ( stateBits & GLS_SRCBLEND_BITS )
			{
                case GLS_SRCBLEND_ZERO:
                    srcFactor = GL_ZERO;
                    break;
                case GLS_SRCBLEND_ONE:
                    srcFactor = GL_ONE;
                    break;
                case GLS_SRCBLEND_DST_COLOR:
                    srcFactor = GL_DST_COLOR;
                    break;
                case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
                    srcFactor = GL_ONE_MINUS_DST_COLOR;
                    break;
                case GLS_SRCBLEND_SRC_ALPHA:
                    srcFactor = GL_SRC_ALPHA;
                    break;
                case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
                    srcFactor = GL_ONE_MINUS_SRC_ALPHA;
                    break;
                case GLS_SRCBLEND_DST_ALPHA:
                    srcFactor = GL_DST_ALPHA;
                    break;
                case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
                    srcFactor = GL_ONE_MINUS_DST_ALPHA;
                    break;
                case GLS_SRCBLEND_ALPHA_SATURATE:
                    srcFactor = GL_SRC_ALPHA_SATURATE;
                    break;
                default:
                    ri.Error( ERR_DROP, "GL_State: invalid src blend state bits" );
                    break;
			}

			switch ( stateBits & GLS_DSTBLEND_BITS )
			{
                case GLS_DSTBLEND_ZERO:
                    dstFactor = GL_ZERO;
                    break;
                case GLS_DSTBLEND_ONE:
                    dstFactor = GL_ONE;
                    break;
                case GLS_DSTBLEND_SRC_COLOR:
                    dstFactor = GL_SRC_COLOR;
                    break;
                case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
                    dstFactor = GL_ONE_MINUS_SRC_COLOR;
                    break;
                case GLS_DSTBLEND_SRC_ALPHA:
                    dstFactor = GL_SRC_ALPHA;
                    break;
                case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
                    dstFactor = GL_ONE_MINUS_SRC_ALPHA;
                    break;
                case GLS_DSTBLEND_DST_ALPHA:
                    dstFactor = GL_DST_ALPHA;
                    break;
                case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
                    dstFactor = GL_ONE_MINUS_DST_ALPHA;
                    break;
                default:
                    ri.Error( ERR_DROP, "GL_State: invalid dst blend state bits" );
                    break;
			}

			qglEnable( GL_BLEND );
			qglBlendFunc( srcFactor, dstFactor );
		}
		else
		{
			qglDisable( GL_BLEND );
		}
	}

	//
	// check depthmask
	//
	if ( diff & GLS_DEPTHMASK_TRUE )
	{
		if ( stateBits & GLS_DEPTHMASK_TRUE )
		{
			qglDepthMask( GL_TRUE );
		}
		else
		{
			qglDepthMask( GL_FALSE );
		}
	}

	//
	// fill/line mode
	//
	if ( diff & GLS_POLYMODE_LINE )
	{
		if ( stateBits & GLS_POLYMODE_LINE )
		{
			qglPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
		else
		{
			qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
	}

	//
	// depthtest
	//
	if ( diff & GLS_DEPTHTEST_DISABLE )
	{
		if ( stateBits & GLS_DEPTHTEST_DISABLE )
		{
			qglDisable( GL_DEPTH_TEST );
		}
		else
		{
			qglEnable( GL_DEPTH_TEST );
		}
	}

	//
	// alpha test
	//
	if ( diff & GLS_ATEST_BITS )
	{
		switch ( stateBits & GLS_ATEST_BITS )
		{
            case 0:
                qglDisable( GL_ALPHA_TEST );
                break;
            case GLS_ATEST_GT_0:
                qglEnable( GL_ALPHA_TEST );
                qglAlphaFunc( GL_GREATER, 0.0f );
                break;
            case GLS_ATEST_LT_80:
                qglEnable( GL_ALPHA_TEST );
                qglAlphaFunc( GL_LESS, 0.5f );
                break;
            case GLS_ATEST_GE_80:
                qglEnable( GL_ALPHA_TEST );
                qglAlphaFunc( GL_GEQUAL, 0.5f );
                break;
            default:
                assert( 0 );
                break;
		}
	}

	glState.glStateBits = stateBits;
}

/*
=============
RE_StretchRaw

FIXME: not exactly backend
Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw(int x, int y, int w, int h, int cols, int rows, const unsigned char *data, int client, qboolean dirty)
{
	int	i, j;
	int	start = 0, end;

	if ( !tr.registered ) {
		return;
	}
	R_IssuePendingRenderCommands();

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// we definately want to sync every frame for the cinematics
	qglFinish();

	if( r_speeds->integer )
    {
		start = ri.Milliseconds();
	}

	// make sure rows and cols are powers of 2
	for ( i = 0 ; ( 1 << i ) < cols ; i++ ) {
	}
	for ( j = 0 ; ( 1 << j ) < rows ; j++ ) {
	}
	if ( ( 1 << i ) != cols || ( 1 << j ) != rows) {
		ri.Error (ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	GL_Bind( tr.scratchImage[client] );

	// if the scratchImage isn't in the format we want, specify it as a new texture
	if ( cols != tr.scratchImage[client]->width || rows != tr.scratchImage[client]->height ) {
		tr.scratchImage[client]->width = tr.scratchImage[client]->uploadWidth = cols;
		tr.scratchImage[client]->height = tr.scratchImage[client]->uploadHeight = rows;
		qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );

		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );	
	}
    else
    {
		if (dirty) {
			// otherwise, just subimage upload it so that drivers can tell we are going to be changing
			// it and don't try and do a texture compression
			qglTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, data );
		}
	}

	if ( r_speeds->integer ) {
		end = ri.Milliseconds();
		ri.Printf( PRINT_ALL, "qglTexSubImage2D %i, %i: %i msec\n", cols, rows, end - start );
	}

	RB_SetGL2D();

	qglColor3f( tr.identityLight, tr.identityLight, tr.identityLight );

	qglBegin (GL_QUADS);
	qglTexCoord2f ( 0.5f / cols,  0.5f / rows );
	qglVertex2f (x, y);
	qglTexCoord2f ( ( cols - 0.5f ) / cols ,  0.5f / rows );
	qglVertex2f (x+w, y);
	qglTexCoord2f ( ( cols - 0.5f ) / cols, ( rows - 0.5f ) / rows );
	qglVertex2f (x+w, y+h);
	qglTexCoord2f ( 0.5f / cols, ( rows - 0.5f ) / rows );
	qglVertex2f (x, y+h);
	qglEnd ();
}


void RE_UploadCinematic (int w, int h, int cols, int rows, const unsigned char* data, int client, qboolean dirty)
{
	GL_Bind( tr.scratchImage[client] );

	// if the scratchImage isn't in the format we want, specify it as a new texture
	if ( cols != tr.scratchImage[client]->width || rows != tr.scratchImage[client]->height )
    {
		tr.scratchImage[client]->width = tr.scratchImage[client]->uploadWidth = cols;
		tr.scratchImage[client]->height = tr.scratchImage[client]->uploadHeight = rows;
		qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );

		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );	
	}
    else
    {
		if (dirty)
        {
			// otherwise, just subimage upload it so that drivers can tell we are going to be changing
			// it and don't try and do a texture compression
			qglTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, data );
		}
	}
}



/*
==================
RB_ReadPixels: Reads an image but takes care of alignment issues for reading RGB images.

Reads a minimum offset for where the RGB data starts in the image from integer stored at pointer offset. 
When the function has returned the actual offset was written back to address offset. 
This address will always have an alignment of packAlign to ensure efficient copying.

Stores the length of padding after a line of pixels to address padlen

Return value must be freed with ri.Hunk_FreeTempMemory()
==================
*/

unsigned char* RB_ReadPixels(int x, int y, int width, int height, size_t *offset, int *padlen)
{
	GLint packAlign;
	qglGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);

	int linelen = width * 3;
	int padwidth = PAD(linelen, packAlign);

	// Allocate a few more bytes so that we can choose an alignment we like
	unsigned char* buffer = ri.Hunk_AllocateTempMemory(padwidth * height + *offset + packAlign - 1);

	unsigned char* bufstart = PADP((intptr_t) buffer + *offset, packAlign);
	qglReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, bufstart);

	*offset = bufstart - buffer;
	*padlen = padwidth - linelen;

	return buffer;
}



/*
===============
RB_ShowImages

Draw all the images to the screen, on top of whatever was there.
This is used to test for texture thrashing.

Also called by RE_EndRegistration
===============
*/
void RB_ShowImages( void )
{
    if ( !backEnd.projection2D )
    {
		RB_SetGL2D();
	}

	qglClear( GL_COLOR_BUFFER_BIT );

	qglFinish();

	int start = ri.Milliseconds();
    
    int i;
	for(i=0; i<tr.numImages ; i++ )
    {
		image_t* image = tr.images[i];

		float w = glConfig.vidWidth / 20;
		float h = glConfig.vidHeight / 15;
		float x = i % 20 * w;
		float y = i / 20 * h;

		// show in proportional size in mode 2
		if ( r_showImages->integer == 2 ) {
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		GL_Bind( image );
		qglBegin (GL_QUADS);
		qglTexCoord2f( 0, 0 );
		qglVertex2f( x, y );
		qglTexCoord2f( 1, 0 );
		qglVertex2f( x + w, y );
		qglTexCoord2f( 1, 1 );
		qglVertex2f( x + w, y + h );
		qglTexCoord2f( 0, 1 );
		qglVertex2f( x, y + h );
		qglEnd();
	}

	qglFinish();

	int end = ri.Milliseconds();
	ri.Printf( PRINT_ALL, "%i msec to draw all images\n", end - start );
}


void RB_ExecuteRenderCommands(const void *data)
{
	int	t1 = ri.Milliseconds();
	while( 1 )
    {
		data = PADP(data, sizeof(void *));

		switch( *(const int *)data )
        {
            case RC_SET_COLOR:
                data = RB_SetColor( data );
                break;
            case RC_STRETCH_PIC:
                //Check if it's time for BLOOM!
                data = RB_StretchPic( data );
                break;
            case RC_SWAP_BUFFERS:
                data = RB_SwapBuffers( data );
                break;

            case RC_DRAW_SURFS:
                data = RB_DrawSurfs( data );
                break;
            case RC_DRAW_BUFFER:
                data = RB_DrawBuffer( data );
                break;

            case RC_SCREENSHOT:
                data = RB_TakeScreenshotCmd( data );
                break;
            case RC_VIDEOFRAME:
                data = RB_TakeVideoFrameCmd( data );
                break;
            case RC_COLORMASK:
                data = RB_ColorMask(data);
                break;
            case RC_CLEARDEPTH:
                data = RB_ClearDepth(data);
                break;
            case RC_END_OF_LIST:
            default:
                // stop rendering
                backEnd.pc.msec = ri.Milliseconds() - t1;
                return;
		}
	}
}


void R_InitBackend(void)
{
	r_screenshotJpegQuality = ri.Cvar_Get("r_screenshotJpegQuality", "90", CVAR_ARCHIVE);
	r_aviMotionJpegQuality = ri.Cvar_Get("r_aviMotionJpegQuality", "90", CVAR_ARCHIVE);
   	r_showImages = ri.Cvar_Get("r_showImages", "0", CVAR_TEMP ); 

    r_clear = ri.Cvar_Get("r_clear", "0", CVAR_CHEAT); 

    r_finish = ri.Cvar_Get("r_finish", "0", CVAR_ARCHIVE);

	r_measureOverdraw = ri.Cvar_Get("r_measureOverdraw", "0", CVAR_CHEAT );

	r_speeds = ri.Cvar_Get("r_speeds", "0", CVAR_CHEAT);
    r_drawSun = ri.Cvar_Get( "r_drawSun", "0", CVAR_ARCHIVE);

    if(r_finish->integer)
    {
		ri.Printf( PRINT_ALL, "Forcing glFinish\n" );
	}
}
