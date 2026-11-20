/*
HOST_GL.C (the iOS port's; from OpenCE's port/android/host/host_gl.c)

OpenGL ES for the guest: Apple's OpenGL ES 3.0, through SDL's UIKit view.
The guest's generated entry points (guest_gl.c) import hostgl_<function>,
which ios_imports.c (ios/tools/gen_imports.py) calls with host pointers;
these are the host services around them.
*/

#include "host.h"

#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#include <string.h>

/* a function of the guest's GL list that Apple's OpenGL ES 3.0 lacks: the
renderer checks the version and extensions before it calls those, so this
says if one is reached anyway */
void host_gl_missing(const char *name)
{
	static const char *reported[16];
	int index;

	for (index = 0; index < 16 && reported[index]; index++)
	{
		if (reported[index] == name)
			return;
	}
	if (index < 16)
		reported[index] = name;
	host_logf(HOST_LOG_WARN, "GL: %s is not available on this device", name);
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = index >= 0 ? glGetStringi(name, (GLuint)index) : glGetString(name);

	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* copies size bytes of a buffer object (the snapshots of the visibility
tests' counters, d3d8_gl.c); ES has no glGetBufferSubData, and the mapping
it offers instead is a host pointer. Binds GL_COPY_READ_BUFFER, which the
renderer uses only for its copies, and leaves it unbound. */
void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	const void *mapping;

	glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = glMapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it: drivers queue several frames, and a draw still
waiting to run would otherwise read a later frame's vertices. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

static void finish_frame_without_fence(void)
{
	static int reported;

	if (!reported)
	{
		host_logf(HOST_LOG_WARN, "GPU: frame fence unavailable or incomplete; finishing queued work before buffer reuse");
		reported = 1;
	}
	glFinish();
}

void host_gl_fence_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!frame_fences[slot])
		finish_frame_without_fence();
}

void host_gl_wait_frame(uint32_t slot)
{
	GLenum status;

	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* A timeout or failed wait does not release the GPU's ownership of the
	ring slot. Its next unsynchronized upload must wait for completion too. */
	status = glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
		finish_frame_without_fence();
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target. The renderer streams a
range per draw, so a frame makes hundreds of these, and the cost per call
rather than per byte is what a frame is made of.

GL_MAP_INVALIDATE_RANGE_BIT was what made that cost ruinous. It tells the
driver the range's previous contents are undefined and must be discarded,
which is the very work GL_MAP_UNSYNCHRONIZED_BIT exists to avoid: that one
promises the caller that no queued draw is reading the range. Asked to do
both, Adreno pays for the discard - about 0.85 ms a call on a Galaxy Z
Flip 4, whatever the range written - and with a few hundred calls a frame
that came to 96% of a 550 ms frame at 1.8 fps, with the GPU idle throughout.

Without the invalidation the same call is well under a microsecond and the
same game runs at the display's refresh rate. The promise unsynchronized
makes still holds: the renderer only writes ranges that no queued draw reads,
because host_gl_wait_frame releases the ring slot first. */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping = glMapBufferRange(target, offset, size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}

/* ---------- the default framebuffer

iOS has none: SDL's UIKit view draws to a framebuffer object of its own and
presents its color renderbuffer. The renderer binds framebuffer 0 to draw to
the window (d3d8_gl.c's present, xgpu_post.c), so 0 is that object, and a
query of the binding says 0 for it. */

static GLuint default_framebuffer;

void host_gl_set_default_framebuffer(uint32_t framebuffer)
{
	/* (SDL binds its view's framebuffer when it makes the context) */
	if (!framebuffer)
	{
		GLint bound = 0;

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
		framebuffer = (uint32_t)bound;
	}
	default_framebuffer = framebuffer;
	host_logf(HOST_LOG_INFO, "GL: the window's framebuffer is %u", (unsigned)framebuffer);
}

void host_gl_bind_framebuffer(GLenum target, GLuint framebuffer)
{
	glBindFramebuffer(target, framebuffer ? framebuffer : default_framebuffer);
}

void host_gl_get_integerv(GLenum name, GLint *data)
{
	glGetIntegerv(name, data);
	if (data && default_framebuffer &&
		(name == GL_DRAW_FRAMEBUFFER_BINDING || name == GL_READ_FRAMEBUFFER_BINDING) &&
		(GLuint)*data == default_framebuffer)
		*data = 0;
}
