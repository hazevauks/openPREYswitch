/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/




#include "tr_local.h"


static const int	FRAME_MEMORY_BYTES = 0x200000;
static const int	EXPAND_HEADERS = 1024;

idCVar idVertexCache::r_showVertexCache( "r_showVertexCache", "0", CVAR_INTEGER|CVAR_RENDERER, "" );
idCVar idVertexCache::r_vertexBufferMegs( "r_vertexBufferMegs", "32", CVAR_INTEGER|CVAR_RENDERER, "" );

idVertexCache		vertexCache;

/*
===========================================================================

OpenPrey: vertex pages

Static blocks are carved out of a few large GL buffers ("pages") instead of each
block owning its own buffer storage. Animated models re-create their vertex,
shadow and per-light caches every frame, hundreds per frame in busy maps. With a
buffer per block each of them was a glBufferData, which in Mesa's nouveau driver
means a new GPU allocation, a staging copy and a deferred free: busy frames of
game/feedingtowera made 700-1600 of them, stuttered, and crashed twice inside
nouveau_mm_allocate. With pages the driver only allocates when a page is added.

- Blocks are written through an unsynchronized map, a plain copy into the page
  with no staging and no wait.
- That is safe because a freed range is only handed out again after a fence
  shows the GPU has finished the frame that freed it.
- Ranges come in size classes (see PageClass) with a free list per class, so
  nothing has to be searched or merged.
- Blocks over PAGE_MAX_BLOCK and the frame temp buffers keep a buffer of their
  own. The frame temp buffers are written the same way, after a fence for the
  frame that last used them.

===========================================================================
*/

#ifdef __SWITCH__
#define VERTEX_PAGES_DEFAULT	"1"
#else
#define VERTEX_PAGES_DEFAULT	"0"
#endif
static idCVar r_vertexPages( "r_vertexPages", VERTEX_PAGES_DEFAULT, CVAR_RENDERER | CVAR_BOOL, "carve vertex cache blocks out of shared 8 MB buffers written through unsynchronized maps, instead of a GL buffer per block; read at startup" );

static const int	PAGE_BYTES = 8 << 20;
static const int	PAGE_MAX_BLOCK = 1 << 20;		// larger blocks keep a buffer of their own
static const int	PAGE_CLASSES = 52;				// size classes up to PAGE_MAX_BLOCK, see PageClass
static const int	PAGE_MAX_FENCES = 16;			// frames of retired ranges waiting for the GPU

typedef struct {
	GLuint			vbo;
	bool			indexBuffer;
	int				used;			// bytes handed out from the start of the page
} vertexPage_t;

typedef struct {
	int				page;
	int				offset;
	int				sizeClass;
} pageRange_t;

typedef struct {
	GLsync					sync;
	idList<pageRange_t>		ranges;
} pageFence_t;

static idList<vertexPage_t>	s_pages;
static int					s_openPage[2] = { -1, -1 };			// page being filled, for vertexes and indexes
static idList<pageRange_t>	s_freeRanges[2][PAGE_CLASSES];		// ranges ready to be handed out
static idList<pageRange_t>	s_retiredRanges;					// freed since the last fence
static pageFence_t			s_pageFences[PAGE_MAX_FENCES];		// ring of fenced ranges, oldest first
static int					s_pageFenceHead;
static int					s_pageFenceCount;

/*
==============
PageClass

Multiples of 64 bytes up to 256, then four classes per power of two (320, 384,
448, 512, 640, ...): a block wastes at most a quarter of its range, and every
range stays 64-byte aligned.
==============
*/
static int PageClass( int size, int &classBytes ) {
	if ( size <= 256 ) {
		const int c = ( size + 63 ) >> 6;
		classBytes = c << 6;
		return c - 1;
	}
	int e = 8;
	while ( ( 2 << e ) < size ) {
		e++;		// size is in ( 2^e, 2^(e+1) ]
	}
	const int step = 1 << ( e - 2 );
	const int k = ( size - ( 1 << e ) + step - 1 ) / step;	// 1..4
	classBytes = ( 1 << e ) + k * step;
	return 4 + ( e - 8 ) * 4 + ( k - 1 );
}

static int PageClassBytes( int sizeClass ) {
	if ( sizeClass < 4 ) {
		return ( sizeClass + 1 ) << 6;
	}
	const int e = 8 + ( sizeClass - 4 ) / 4;
	const int k = 1 + ( sizeClass - 4 ) % 4;
	return ( 1 << e ) + k * ( 1 << ( e - 2 ) );
}

static GLenum CacheTarget( bool indexBuffer ) {
	return indexBuffer ? GL_ELEMENT_ARRAY_BUFFER_ARB : GL_ARRAY_BUFFER_ARB;
}

/*
==============
WriteUnsynchronized

Copies into a bound buffer without waiting for the GPU or staging the data. The
caller guarantees the GPU no longer reads that range.
==============
*/
static void WriteUnsynchronized( GLenum target, int offset, int size, const void *data ) {
	void *dest = glMapBufferRange( target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT );
	if ( dest ) {
		SIMDProcessor->Memcpy( dest, data, size );
		glUnmapBuffer( target );
	} else {
		glBufferSubDataARB( target, offset, size, data );
	}
}

/*
==============
idVertexCache::AllocPageRange

Gives block (size already set) a range in a page and binds its buffer.
==============
*/
void idVertexCache::AllocPageRange( vertCache_t *block, bool indexBuffer ) {
	const GLenum target = CacheTarget( indexBuffer );
	int classBytes;
	const int sizeClass = PageClass( block->size, classBytes );

	idList<pageRange_t> &freeRanges = s_freeRanges[indexBuffer][sizeClass];
	pageRange_t range;
	if ( freeRanges.Num() > 0 ) {
		range = freeRanges[freeRanges.Num() - 1];
		freeRanges.RemoveIndex( freeRanges.Num() - 1 );
	} else {
		int &open = s_openPage[indexBuffer];
		if ( open < 0 || s_pages[open].used + classBytes > PAGE_BYTES ) {
			if ( open >= 0 ) {
				// the end of the full page becomes free ranges of the largest classes that fit
				vertexPage_t &full = s_pages[open];
				while ( PAGE_BYTES - full.used >= 64 ) {
					pageRange_t tail;
					tail.page = open;
					tail.offset = full.used;
					tail.sizeClass = PAGE_CLASSES - 1;
					while ( PageClassBytes( tail.sizeClass ) > PAGE_BYTES - full.used ) {
						tail.sizeClass--;
					}
					full.used += PageClassBytes( tail.sizeClass );
					s_freeRanges[indexBuffer][tail.sizeClass].Append( tail );
				}
			}
			vertexPage_t page;
			page.indexBuffer = indexBuffer;
			page.used = 0;
			glGenBuffersARB( 1, &page.vbo );
			glBindBufferARB( target, page.vbo );
			glBufferDataARB( target, PAGE_BYTES, NULL, GL_STREAM_DRAW_ARB );	// stream: memory the CPU maps directly
			open = s_pages.Append( page );
		}
		range.page = open;
		range.offset = s_pages[open].used;
		range.sizeClass = sizeClass;
		s_pages[open].used += classBytes;
	}

	block->page = range.page;
	block->pageClass = range.sizeClass;
	block->offset = range.offset;
	block->vbo = s_pages[range.page].vbo;
	glBindBufferARB( target, block->vbo );
}

/*
==============
idVertexCache::RetirePageRange
==============
*/
void idVertexCache::RetirePageRange( vertCache_t *block ) {
	pageRange_t range;
	range.page = block->page;
	range.offset = block->offset;
	range.sizeClass = block->pageClass;
	s_retiredRanges.Append( range );
	block->page = -1;
}

/*
==============
idVertexCache::ReclaimPageRanges

Called at the end of each frame: fences the ranges retired during it, and hands
out again the ranges of every fence the GPU has passed.
==============
*/
void idVertexCache::ReclaimPageRanges( void ) {
	while ( s_pageFenceCount > 0 ) {
		pageFence_t &oldest = s_pageFences[s_pageFenceHead];
		// only wait when the ring is full and a new fence is needed
		const bool mustWait = ( s_pageFenceCount == PAGE_MAX_FENCES && s_retiredRanges.Num() > 0 );
		const GLenum status = glClientWaitSync( oldest.sync, mustWait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, mustWait ? 1000000000ull : 0 );
		if ( status == GL_TIMEOUT_EXPIRED ) {
			if ( mustWait ) {
				continue;
			}
			break;
		}
		if ( status == GL_WAIT_FAILED ) {
			common->Warning( "vertex pages: fence wait failed" );
		}
		for ( int i = 0; i < oldest.ranges.Num(); i++ ) {
			const pageRange_t &range = oldest.ranges[i];
			s_freeRanges[s_pages[range.page].indexBuffer][range.sizeClass].Append( range );
		}
		oldest.ranges.SetNum( 0, false );
		glDeleteSync( oldest.sync );
		oldest.sync = NULL;
		s_pageFenceHead = ( s_pageFenceHead + 1 ) % PAGE_MAX_FENCES;
		s_pageFenceCount--;
	}

	if ( s_retiredRanges.Num() > 0 ) {
		pageFence_t &fence = s_pageFences[( s_pageFenceHead + s_pageFenceCount ) % PAGE_MAX_FENCES];
		fence.sync = glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
		fence.ranges.Swap( s_retiredRanges );	// the slot's list was emptied when it was reclaimed
		s_retiredRanges.SetNum( 0, false );
		s_pageFenceCount++;
	}
}

/*
==============
idVertexCache::ListPages
==============
*/
void idVertexCache::ListPages( void ) {
	if ( !usePages ) {
		common->Printf( "Vertex pages are off (r_vertexPages 0).\n" );
		return;
	}
	int freeBytes = 0;
	for ( int kind = 0; kind < 2; kind++ ) {
		for ( int c = 0; c < PAGE_CLASSES; c++ ) {
			freeBytes += s_freeRanges[kind][c].Num() * PageClassBytes( c );
		}
	}
	int pendingRanges = s_retiredRanges.Num();
	for ( int i = 0; i < s_pageFenceCount; i++ ) {
		pendingRanges += s_pageFences[( s_pageFenceHead + i ) % PAGE_MAX_FENCES].ranges.Num();
	}
	common->Printf( "%i vertex pages of %ik (%ik), %ik in free ranges, %i ranges waiting on %i fences\n",
		s_pages.Num(), PAGE_BYTES / 1024, s_pages.Num() * ( PAGE_BYTES / 1024 ), freeBytes / 1024, pendingRanges, s_pageFenceCount );
}

/*
==============
R_ListVertexCache_f
==============
*/
static void R_ListVertexCache_f( const idCmdArgs &args ) {
	vertexCache.List();
}

/*
==============
idVertexCache::ActuallyFree
==============
*/
void idVertexCache::ActuallyFree( vertCache_t *block ) {
	if (!block) {
		common->Error( "idVertexCache Free: NULL pointer" );
	}

	if ( block->user ) {
		// let the owner know we have purged it
		*block->user = NULL;
		block->user = NULL;
	}

	// temp blocks are in a shared space that won't be freed
	if ( block->tag != TAG_TEMP ) {
		staticAllocTotal -= block->size;
		staticCountTotal--;

		if ( block->page >= 0 ) {
			// OpenPrey: the range goes back to its page once the GPU is done with it
			RetirePageRange( block );
		} else if ( block->vbo && usePages ) {
			// OpenPrey: with pages only large blocks own storage; don't let free headers hold it
			glBindBufferARB( CacheTarget( block->indexBuffer ), block->vbo );
			glBufferDataARB( CacheTarget( block->indexBuffer ), 0, NULL, GL_STATIC_DRAW_ARB );
		} else if ( block->vbo ) {
#if 0		// this isn't really necessary, it will be reused soon enough
			// filling with zero length data is the equivalent of freeing
			glBindBufferARB(GL_ARRAY_BUFFER_ARB, block->vbo);
			glBufferDataARB(GL_ARRAY_BUFFER_ARB, 0, 0, GL_DYNAMIC_DRAW_ARB);
#endif
		} else if ( block->virtMem ) {
			Mem_Free( block->virtMem );
			block->virtMem = NULL;
		}
	}
	block->tag = TAG_FREE;		// mark as free

	// unlink stick it back on the free list
	block->next->prev = block->prev;
	block->prev->next = block->next;

#if 1
	// stick it on the front of the free list so it will be reused immediately
	block->next = freeStaticHeaders.next;
	block->prev = &freeStaticHeaders;
#else
	// stick it on the back of the free list so it won't be reused soon (just for debugging)
	block->next = &freeStaticHeaders;
	block->prev = freeStaticHeaders.prev;
#endif

	block->next->prev = block;
	block->prev->next = block;
}

/*
==============
idVertexCache::Position

this will be a real pointer with virtual memory,
but it will be an int offset cast to a pointer with
ARB_vertex_buffer_object

The ARB_vertex_buffer_object will be bound
==============
*/
void *idVertexCache::Position( vertCache_t *buffer ) {
	if ( !buffer || buffer->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache::Position: bad vertCache_t" );
	}

	// the ARB vertex object just uses an offset
	if ( buffer->vbo ) {
		if ( r_showVertexCache.GetInteger() == 2 ) {
			if ( buffer->tag == TAG_TEMP ) {
				common->Printf( "GL_ARRAY_BUFFER_ARB = %i + %i (%i bytes)\n", buffer->vbo, buffer->offset, buffer->size ); 
			} else {
				common->Printf( "GL_ARRAY_BUFFER_ARB = %i (%i bytes)\n", buffer->vbo, buffer->size ); 
			}
		}
		if ( buffer->indexBuffer ) {
			glBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, buffer->vbo );
		} else {
			glBindBufferARB( GL_ARRAY_BUFFER_ARB, buffer->vbo );
		}
		return (void *)buffer->offset;
	}

	// virtual memory is a real pointer
	return (void *)((byte *)buffer->virtMem + buffer->offset);
}

void idVertexCache::UnbindIndex() {
	glBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, 0 );
}


//================================================================================

/*
===========
idVertexCache::Init
===========
*/
void idVertexCache::Init() {
	cmdSystem->AddCommand( "listVertexCache", R_ListVertexCache_f, CMD_FL_RENDERER, "lists vertex cache" );

	if ( r_vertexBufferMegs.GetInteger() < 8 ) {
		r_vertexBufferMegs.SetInteger( 8 );
	}

	virtualMemory = false;

	// use ARB_vertex_buffer_object unless explicitly disabled
	if( r_useVertexBuffers.GetInteger() && glConfig.ARBVertexBufferObjectAvailable ) {
		common->Printf( "using ARB_vertex_buffer_object memory\n" );
	} else {
		virtualMemory = true;
		r_useIndexBuffers.SetBool( false );
		common->Printf( "WARNING: vertex array range in virtual memory (SLOW)\n" );
	}

	// OpenPrey: vertex pages need unsynchronized maps and fences
	usePages = !virtualMemory && r_vertexPages.GetBool() && glMapBufferRange != NULL && glUnmapBuffer != NULL
		&& glFenceSync != NULL && glClientWaitSync != NULL && glDeleteSync != NULL;
	if ( usePages ) {
		common->Printf( "vertex cache: %i KB pages\n", PAGE_BYTES / 1024 );
		// busy frames retire and allocate over a thousand ranges
		s_retiredRanges.SetGranularity( 1024 );
		for ( int i = 0; i < PAGE_MAX_FENCES; i++ ) {
			s_pageFences[i].ranges.SetGranularity( 1024 );
		}
		for ( int kind = 0; kind < 2; kind++ ) {
			for ( int c = 0; c < PAGE_CLASSES; c++ ) {
				s_freeRanges[kind][c].SetGranularity( 256 );
			}
		}
	}
	for ( int i = 0; i < NUM_VERTEX_FRAMES; i++ ) {
		tempFences[i] = NULL;
	}

	// initialize the cache memory blocks
	freeStaticHeaders.next = freeStaticHeaders.prev = &freeStaticHeaders;
	staticHeaders.next = staticHeaders.prev = &staticHeaders;
	freeDynamicHeaders.next = freeDynamicHeaders.prev = &freeDynamicHeaders;
	dynamicHeaders.next = dynamicHeaders.prev = &dynamicHeaders;
	deferredFreeList.next = deferredFreeList.prev = &deferredFreeList;

	// set up the dynamic frame memory
	frameBytes = FRAME_MEMORY_BYTES;
	staticAllocTotal = 0;

	byte	*junk = (byte *)Mem_Alloc( frameBytes );
	for ( int i = 0 ; i < NUM_VERTEX_FRAMES ; i++ ) {
		allocatingTempBuffer = true;	// force the alloc to use GL_STREAM_DRAW_ARB
		Alloc( junk, frameBytes, &tempBuffers[i] );
		allocatingTempBuffer = false;
		tempBuffers[i]->tag = TAG_FIXED;
		// unlink these from the static list, so they won't ever get purged
		tempBuffers[i]->next->prev = tempBuffers[i]->prev;
		tempBuffers[i]->prev->next = tempBuffers[i]->next;
	}
	Mem_Free( junk );

	EndFrame();
}

/*
===========
idVertexCache::PurgeAll

Used when toggling vertex programs on or off, because
the cached data isn't valid
===========
*/
void idVertexCache::PurgeAll() {
	while( staticHeaders.next != &staticHeaders ) {
		ActuallyFree( staticHeaders.next );
	}
}

/*
===========
idVertexCache::Shutdown
===========
*/
void idVertexCache::Shutdown() {
//	PurgeAll();	// !@#: also purge the temp buffers

	headerAllocator.Shutdown();

	// OpenPrey: forget the vertex pages, they belong to the GL context
	s_pages.Clear();
	s_openPage[0] = s_openPage[1] = -1;
	for ( int kind = 0; kind < 2; kind++ ) {
		for ( int c = 0; c < PAGE_CLASSES; c++ ) {
			s_freeRanges[kind][c].Clear();
		}
	}
	s_retiredRanges.Clear();
	for ( int i = 0; i < PAGE_MAX_FENCES; i++ ) {
		s_pageFences[i].sync = NULL;
		s_pageFences[i].ranges.Clear();
	}
	s_pageFenceHead = s_pageFenceCount = 0;
}

/*
===========
idVertexCache::Alloc
===========
*/
void idVertexCache::Alloc( void *data, int size, vertCache_t **buffer, bool indexBuffer ) {
	vertCache_t	*block;

	if ( size <= 0 ) {
		common->Error( "idVertexCache::Alloc: size = %i\n", size );
	}

	// if we can't find anything, it will be NULL
	*buffer = NULL;

	// if we don't have any remaining unused headers, allocate some more
	if ( freeStaticHeaders.next == &freeStaticHeaders ) {

		for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
			block = headerAllocator.Alloc();
			block->next = freeStaticHeaders.next;
			block->prev = &freeStaticHeaders;
			block->next->prev = block;
			block->prev->next = block;

			block->vbo = 0;
			block->ownVbo = 0;
			block->page = -1;
			block->pageClass = 0;
			block->virtMem = NULL;
			if( !virtualMemory ) {
				glGenBuffersARB( 1, & block->ownVbo );
			}
		}
	}

	block = freeStaticHeaders.next;

	// move it from the freeStaticHeaders list to the staticHeaders list
	block->next->prev = block->prev;
	block->prev->next = block->next;
	block->next = staticHeaders.next;
	block->prev = &staticHeaders;
	block->next->prev = block;
	block->prev->next = block;

	block->size = size;
	block->offset = 0;
	block->tag = TAG_USED;

	// save data for debugging
	staticAllocThisFrame += block->size;
	staticCountThisFrame++;
	staticCountTotal++;
	staticAllocTotal += block->size;

	// this will be set to zero when it is purged
	block->user = buffer;
	*buffer = block;

	// allocation doesn't imply used-for-drawing, because at level
	// load time lots of things may be created, but they aren't
	// referenced by the GPU yet, and can be purged if needed.
	block->frameUsed = currentFrame - NUM_VERTEX_FRAMES;

	block->indexBuffer = indexBuffer;

	// copy the data
	if ( !virtualMemory && usePages && !allocatingTempBuffer && size <= PAGE_MAX_BLOCK ) {
		// OpenPrey: a range in a vertex page
		AllocPageRange( block, indexBuffer );
		WriteUnsynchronized( CacheTarget( indexBuffer ), block->offset, size, data );
		staticPagedThisFrame++;
	} else if ( !virtualMemory ) {
		block->vbo = block->ownVbo;
		if ( indexBuffer ) {
			glBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, block->vbo );
			glBufferDataARB( GL_ELEMENT_ARRAY_BUFFER_ARB, (GLsizeiptrARB)size, data, GL_STATIC_DRAW_ARB );
		} else {
			glBindBufferARB( GL_ARRAY_BUFFER_ARB, block->vbo );
			if ( allocatingTempBuffer ) {
				glBufferDataARB( GL_ARRAY_BUFFER_ARB, (GLsizeiptrARB)size, data, GL_STREAM_DRAW_ARB );
			} else {
				glBufferDataARB( GL_ARRAY_BUFFER_ARB, (GLsizeiptrARB)size, data, GL_STATIC_DRAW_ARB );
			}
		}
	} else {
		block->virtMem = Mem_Alloc( size );
		SIMDProcessor->Memcpy( block->virtMem, data, size );
	}
}

/*
===========
idVertexCache::Touch
===========
*/
void idVertexCache::Touch( vertCache_t *block ) {
	if ( !block ) {
		common->Error( "idVertexCache Touch: NULL pointer" );
	}

	if ( block->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache Touch: freed pointer" );
	}
	if ( block->tag == TAG_TEMP ) {
		common->FatalError( "idVertexCache Touch: temporary pointer" );
	}

	block->frameUsed = currentFrame;

	// move to the head of the LRU list
	block->next->prev = block->prev;
	block->prev->next = block->next;

	block->next = staticHeaders.next;
	block->prev = &staticHeaders;
	staticHeaders.next->prev = block;
	staticHeaders.next = block;
}

/*
===========
idVertexCache::Free
===========
*/
void idVertexCache::Free( vertCache_t *block ) {
	if (!block) {
		return;
	}

	if ( block->tag == TAG_FREE ) {
		common->FatalError( "idVertexCache Free: freed pointer" );
	}
	if ( block->tag == TAG_TEMP ) {
		common->FatalError( "idVertexCache Free: temporary pointer" );
	}

	// this block still can't be purged until the frame count has expired,
	// but it won't need to clear a user pointer when it is
	block->user = NULL;

	block->next->prev = block->prev;
	block->prev->next = block->next;

	block->next = deferredFreeList.next;
	block->prev = &deferredFreeList;
	deferredFreeList.next->prev = block;
	deferredFreeList.next = block;
}

/*
===========
idVertexCache::AllocFrameTemp

A frame temp allocation must never be allowed to fail due to overflow.
We can't simply sync with the GPU and overwrite what we have, because
there may still be future references to dynamically created surfaces.
===========
*/
vertCache_t	*idVertexCache::AllocFrameTemp( void *data, int size ) {
	vertCache_t	*block;

	if ( size <= 0 ) {
		common->Error( "idVertexCache::AllocFrameTemp: size = %i\n", size );
	}

	if ( dynamicAllocThisFrame + size > frameBytes ) {
		// if we don't have enough room in the temp block, allocate a static block,
		// but immediately free it so it will get freed at the next frame
		tempOverflow = true;
		Alloc( data, size, &block );
		Free( block);
		return block;
	}

	// this data is just going on the shared dynamic list

	// if we don't have any remaining unused headers, allocate some more
	if ( freeDynamicHeaders.next == &freeDynamicHeaders ) {

		for ( int i = 0; i < EXPAND_HEADERS; i++ ) {
			block = headerAllocator.Alloc();
			block->next = freeDynamicHeaders.next;
			block->prev = &freeDynamicHeaders;
			block->next->prev = block;
			block->prev->next = block;
		}
	}

	// move it from the freeDynamicHeaders list to the dynamicHeaders list
	block = freeDynamicHeaders.next;
	block->next->prev = block->prev;
	block->prev->next = block->next;
	block->next = dynamicHeaders.next;
	block->prev = &dynamicHeaders;
	block->next->prev = block;
	block->prev->next = block;

	block->size = size;
	block->tag = TAG_TEMP;
	block->indexBuffer = false;
	block->offset = dynamicAllocThisFrame;
	dynamicAllocThisFrame += block->size;
	dynamicCountThisFrame++;
	block->user = NULL;
	block->frameUsed = 0;

	// copy the data
	block->virtMem = tempBuffers[listNum]->virtMem;
	block->vbo = tempBuffers[listNum]->vbo;

	if ( block->vbo && usePages ) {
		// OpenPrey: once the GPU is done with the frame that last used this temp
		// buffer, write it directly (a glBufferSubData into a buffer in use this
		// frame made the driver stage every write)
		if ( tempFences[listNum] ) {
			glClientWaitSync( tempFences[listNum], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull );
			glDeleteSync( tempFences[listNum] );
			tempFences[listNum] = NULL;
		}
		glBindBufferARB( GL_ARRAY_BUFFER_ARB, block->vbo );
		WriteUnsynchronized( GL_ARRAY_BUFFER_ARB, block->offset, size, data );
	} else if ( block->vbo ) {
		glBindBufferARB( GL_ARRAY_BUFFER_ARB, block->vbo );
		glBufferSubDataARB( GL_ARRAY_BUFFER_ARB, block->offset, (GLsizeiptrARB)size, data );
	} else {
		SIMDProcessor->Memcpy( (byte *)block->virtMem + block->offset, data, size );
	}

	return block;
}

/*
===========
idVertexCache::EndFrame
===========
*/
void idVertexCache::EndFrame() {
	// display debug information
	if ( r_showVertexCache.GetBool() ) {
		int	staticUseCount = 0;
		int staticUseSize = 0;

		for ( vertCache_t *block = staticHeaders.next ; block != &staticHeaders ; block = block->next ) {
			if ( block->frameUsed == currentFrame ) {
				staticUseCount++;
				staticUseSize += block->size;
			}
		}

		const char *frameOverflow = tempOverflow ? "(OVERFLOW)" : "";

		common->Printf( "vertex dynamic:%i=%ik%s, static alloc:%i=%ik used:%i=%ik total:%i=%ik\n",
			dynamicCountThisFrame, dynamicAllocThisFrame/1024, frameOverflow,
			staticCountThisFrame, staticAllocThisFrame/1024,
			staticUseCount, staticUseSize/1024,
			staticCountTotal, staticAllocTotal/1024 );
	}

#if 0
	// if our total static count is above our working memory limit, start purging things
	while ( staticAllocTotal > r_vertexBufferMegs.GetInteger() * 1024 * 1024 ) {
		// free the least recently used

	}
#endif

	if( !virtualMemory ) {
		// unbind vertex buffers so normal virtual memory will be used in case
		// r_useVertexBuffers / r_useIndexBuffers
		glBindBufferARB( GL_ARRAY_BUFFER_ARB, 0 );
		glBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, 0 );
	}


	R_AddVertexCachePerf( staticCountThisFrame, staticPagedThisFrame, staticAllocThisFrame, dynamicAllocThisFrame, tempOverflow );

	// OpenPrey: fence this frame's use of its temp buffer (see AllocFrameTemp)
	if ( usePages && dynamicCountThisFrame > 0 ) {
		if ( tempFences[listNum] ) {
			glDeleteSync( tempFences[listNum] );
		}
		tempFences[listNum] = glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
	}

	currentFrame = tr.frameCount;
	listNum = currentFrame % NUM_VERTEX_FRAMES;
	staticAllocThisFrame = 0;
	staticCountThisFrame = 0;
	staticPagedThisFrame = 0;
	dynamicAllocThisFrame = 0;
	dynamicCountThisFrame = 0;
	tempOverflow = false;

	// free all the deferred free headers
	while( deferredFreeList.next != &deferredFreeList ) {
		ActuallyFree( deferredFreeList.next );
	}

	if ( usePages ) {
		ReclaimPageRanges();
	}

	// free all the frame temp headers
	vertCache_t	*block = dynamicHeaders.next;
	if ( block != &dynamicHeaders ) {
		block->prev = &freeDynamicHeaders;
		dynamicHeaders.prev->next = freeDynamicHeaders.next;
		freeDynamicHeaders.next->prev = dynamicHeaders.prev;
		freeDynamicHeaders.next = block;

		dynamicHeaders.next = dynamicHeaders.prev = &dynamicHeaders;
	}
}

/*
=============
idVertexCache::List
=============
*/
void idVertexCache::List( void ) {
	int	numActive = 0;
	int	numDeferred = 0;
	int frameStatic = 0;
	int	totalStatic = 0;
	int	deferredSpace = 0;

	vertCache_t *block;
	for ( block = staticHeaders.next ; block != &staticHeaders ; block = block->next) {
		numActive++;

		totalStatic += block->size;
		if ( block->frameUsed == currentFrame ) {
			frameStatic += block->size;
		}
	}

	int	numFreeStaticHeaders = 0;
	for ( block = freeStaticHeaders.next ; block != &freeStaticHeaders ; block = block->next ) {
		numFreeStaticHeaders++;
	}

	int	numFreeDynamicHeaders = 0;
	for ( block = freeDynamicHeaders.next ; block != &freeDynamicHeaders ; block = block->next ) {
		numFreeDynamicHeaders++;
	}

	common->Printf( "%i megs working set\n", r_vertexBufferMegs.GetInteger() );
	common->Printf( "%i dynamic temp buffers of %ik\n", NUM_VERTEX_FRAMES, frameBytes / 1024 );
	common->Printf( "%5i active static headers\n", numActive );
	common->Printf( "%5i free static headers\n", numFreeStaticHeaders );
	common->Printf( "%5i free dynamic headers\n", numFreeDynamicHeaders );

	if ( !virtualMemory  ) {
		common->Printf( "Vertex cache is in ARB_vertex_buffer_object memory (FAST).\n");
	} else {
		common->Printf( "Vertex cache is in virtual memory (SLOW)\n" );
	}

	if ( r_useIndexBuffers.GetBool() ) {
		common->Printf( "Index buffers are accelerated.\n" );
	} else {
		common->Printf( "Index buffers are not used.\n" );
	}

	ListPages();
}

/*
=============
idVertexCache::IsFast

just for gfxinfo printing
=============
*/
bool idVertexCache::IsFast() {
	if ( virtualMemory ) {
		return false;
	}
	return true;
}
