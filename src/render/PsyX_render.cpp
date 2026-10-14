#include "PsyX/PsyX_public.h"

#include "../platform.h"
#include "../gpu/PsyX_GPU.h"

#include "PsyX/PsyX_render.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/util/timer.h"

#include <assert.h>
#include <string.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif //def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO					1
#define USE_OFFSCREEN_BLIT		1
#define USE_FRAMEBUFFER_BLIT	1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as framebuffer, so it's disabled
#define USE_PBO					(OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT		(OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT	(OGLES_VERSION == 3)

#endif

extern SDL_Window* g_window;


#define MAX_NUM_VERTEX_BUFFERS		(2)
#define PSX_SCREEN_ASPECT	(240.0f / 320.0f)			// PSX screen is mapped always to this aspect

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousStencilMode = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = { 0,0,0,0 };
RECT16 g_PreviousOffscreen = { 0,0,0,0 };

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
TextureID g_rgLutTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

// snow layer, see gpu_shader_snow
static float g_snowAmount = 0.0f;
static float g_snowGroundPos[3] = { 0, 0, 0 };		// a point on the road level, view space
static float g_snowGroundUp[3] = { 0, -1, 0 };		// unit vector pointing up, view space
static int g_snowSerial = 1;
static float g_fogAmount = 0.0f;
static float g_fogColor[3] = { 0.7f, 0.7f, 0.7f };
// headlight beam of the player car in view space, see beamLight in gpu_shader_snow
static float g_beamAmount = 0.0f;
static float g_beamPos[3] = { 0, 0, 0 };
static float g_beamDir[3] = { 0, 0, 1 };
// lamps and the headlight cone scattering in the air, see volumeLight in gpu_shader_snow
static int g_volLightCount = 0;
static float g_volLightPos[PSYX_MAX_VOLUME_LIGHTS * 3];
static float g_volLightCol[PSYX_MAX_VOLUME_LIGHTS * 4];
static float g_volDensity = 0.0f;
static float g_beamVolume = 0.0f;
// height fog and sun glow of the fog, see applyFog in gpu_shader_snow
static float g_hfFalloff = 0.0f;
static float g_hfDensity = 0.0f;
static float g_worldUp[3] = { 0, -1, 0 };
static float g_camHeight = 0.0f;
static float g_sunDir[3] = { 0, 0, 1 };
static float g_sunColor[3] = { 1, 1, 1 };
static float g_sunScatter = 0.0f;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;
int g_cfg_bilinearFiltering = 0;

int vram_need_update = 1;
int framebuffer_need_update = 0;

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif



#if USE_OPENGL
typedef struct
{
	GLenum fmt;
	GLuint* pbos;
	uint64_t num_pbos;
	uint64_t dx;
	uint64_t num_downloads;

	int width;
	int height;
	int nbytes; /* number of bytes in the pbo buffer. */
	unsigned char* pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO* pbo, GLenum format, int w, int h, int num)
{
	if (pbo->pbos)
	{
		eprinterr("Already initialized. Not necessary to initialize again; or shutdown first.");
		return -1;
	}

	if (0 >= num)
	{
		eprinterr("Invalid number of PBOs: %d", num);
		return -2;
	}

	pbo->fmt = format;
	pbo->width = w;
	pbo->height = h;
	pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
	if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height;
	}
	else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt)
	{
		pbo->nbytes = pbo->width * pbo->height * 3;
	}
	else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height * 4;
	}
	else
	{
		eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
		return -3;
	}

	if (pbo->nbytes == 0)
	{
		eprinterr("Invalid width or height given: %d x %d", pbo->width, pbo->height);
		return -4;
	}

	pbo->pbos = (GLuint*)malloc(sizeof(GLuint) * num);
	pbo->pixels = (u_char*)malloc(pbo->nbytes);

	glGenBuffers(num, pbo->pbos);
	for (int i = 0; i < num; ++i)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
	return 0;
}

void PBO_Destroy(GrPBO* pbo)
{
#if USE_PBO
	if(pbo->pbos)
	{
		glDeleteBuffers(pbo->num_pbos, pbo->pbos);
	
		free(pbo->pbos);
		pbo->num_pbos = 0;
		pbo->pbos = NULL;
	}

#endif
	if (pbo->pixels)
	{
		free(pbo->pixels);
		pbo->pixels = NULL;
	}

	pbo->num_downloads = 0;
	pbo->dx = 0;
	pbo->fmt = 0;
	pbo->nbytes = 0;
}

void PBO_Download(GrPBO* pbo)
{
	unsigned char* ptr;
	
#if USE_PBO
	if (pbo->num_downloads < pbo->num_pbos)
	{
		/*
		   First we need to make sure all our pbos are bound, so glMap/Unmap will
		   read from the oldest bound buffer first.
		*/
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);   /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used as offset into the buffer to read into. */
#endif
	}
	else
	{
		/* Read from the oldest bound pbo */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		ptr = (unsigned char*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (NULL != ptr)
		{
			memcpy(pbo->pixels, ptr, pbo->nbytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
			eprintwarn("Failed to map the buffer\n");

		/* Trigger the next read. */
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
	}

	++pbo->dx;
	pbo->dx = pbo->dx % pbo->num_pbos;

	pbo->num_downloads++;

	if (pbo->num_downloads == UINT64_MAX)
		pbo->num_downloads = pbo->num_pbos;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
	// FIXME: THIS is very slow
	// Do not use at all

	// glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not accidentilly using a PBO. */
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint		g_glVertexArray[2];
GLuint		g_glVertexBuffer[2];
int			g_curVertexBuffer = 0;

GLuint		g_glBlitFramebuffer;
GrPBO		g_glFramebufferPBO;

GLuint		g_glVRAMFramebuffer;

GLuint		g_glOffscreenFramebuffer;
GrPBO		g_glOffscreenPBO;

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char* windowName, int fullscreen)
{
	int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
	windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
	if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	if(g_windowWidth <= 0 || g_windowHeight <= 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);

	if (g_window == NULL)
	{
		eprinterr("Failed to initialise SDL window!\n");
		return 0;
	}
	
#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
	//Override to full screen.
	SDL_DisplayMode displayMode;
	if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0)
	{
		screenWidth = displayMode.w;
		windowWidth = displayMode.w;
		screenHeight = displayMode.h;
		windowHeight = displayMode.h;
	}
#endif

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if(!SDL_GL_CreateContext(g_window))
	{
		eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n", OGLES_VERSION);
		return 0;
	}

#elif defined(RENDERER_OGL)

	int major_version = 3;
	int minor_version = 3;
	int profile = SDL_GL_CONTEXT_PROFILE_CORE;

	// find best OpenGL version
	do
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

		if (SDL_GL_CreateContext(g_window))
			break;
	
		minor_version--;
		
	} while (minor_version >= 0);

	if (minor_version == -1)
	{
		eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.\n");
		return 0;
	}
#endif

	return 1;
}
#endif

int GR_InitialiseGLExt()
{
#ifdef USE_GLAD
	GLenum err = gladLoadGL();

	if (err == 0)
		return 0;
#endif
	
	const char* rend = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	eprintf("*Video adapter: %s by %s\n", rend, vendor);

	const char* versionStr = (const char*)glGetString(GL_VERSION);
	eprintf("*OpenGL version: %s\n", versionStr);

	const char* glslVersionStr = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	eprintf("*GLSL version: %s\n", glslVersionStr);

	return 1;
}

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen)
{
	g_windowWidth = width;
	g_windowHeight = height;

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitor");
#endif

#if USE_OPENGL
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (!GR_InitialiseGLContext(windowName, fullscreen))
	{
		eprinterr("Failed to Initialise GL Context!\n");
		return 0;
	}
#endif

	if (!GR_InitialiseGLExt())
	{
		eprinterr("Failed to Intialise GL extensions\n");
		return 0;
	}
#endif
	
	return 1;
}

void GR_Shutdown()
{
#if USE_OPENGL
	glDeleteVertexArrays(2, g_glVertexArray);
	glDeleteBuffers(2, g_glVertexBuffer);

	PBO_Destroy(&g_glFramebufferPBO);
	PBO_Destroy(&g_glOffscreenPBO);

	glDeleteFramebuffers(1, &g_glBlitFramebuffer);
	glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
	glDeleteFramebuffers(1, &g_glVRAMFramebuffer);

	GR_DestroyTexture(g_vramTexturesDouble[0]);
	GR_DestroyTexture(g_vramTexturesDouble[1]);

	GR_DestroyTexture(g_whiteTexture);
	GR_DestroyTexture(g_rgLutTexture);
	GR_DestroyTexture(g_fbTexture);
	GR_DestroyTexture(g_offscreenRTTexture);
#endif
}

void GR_UpdateSwapIntervalState(int swapInterval)
{
#if defined(RENDERER_OGL)
	SDL_GL_SetSwapInterval(swapInterval);
#endif
}

void GR_BeginScene()
{
	g_lastBoundTexture = 0;

#if USE_OPENGL
#ifdef RENDERER_OGLES
	//glClearDepthf(1.0f);
#else
	glClearDepth(1.0f);
#endif
	glClear(GL_DEPTH_BUFFER_BIT);
	glClear(GL_STENCIL_BUFFER_BIT);
#endif

	GR_UpdateVRAM();
	GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

	if (g_dbg_wireframeMode)
	{
		GR_SetWireframe(1);

#if USE_OPENGL
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
}

void GR_EndScene()
{
	framebuffer_need_update = 1;
	
	if (g_dbg_wireframeMode)
		GR_SetWireframe(0);

#if USE_OPENGL
	glBindVertexArray(0);
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
static u_char rgLUT[LUT_WIDTH * LUT_HEIGHT * sizeof(u_int)];

void GR_ResetDevice()
{
	GR_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

#if USE_OPENGL
	GLint projectionLoc;
	GLint projection3DLoc;
	GLint bilinearFilterLoc;
	GLint texelSizeLoc;
	GLint texLoc;
	GLint lutLoc;
	GLint snowLoc;
	GLint snowGroundPosLoc;
	GLint snowGroundUpLoc;
	GLint fogLoc;
	GLint fogColorLoc;
	GLint beamLoc;
	GLint beamPosLoc;
	GLint beamDirLoc;
	GLint lightCountLoc;
	GLint lightPosLoc;
	GLint lightColLoc;
	GLint volDensityLoc;
	GLint beamVolumeLoc;
	GLint viewScaleLoc;
	GLint hfFalloffLoc;
	GLint hfDensityLoc;
	GLint worldUpLoc;
	GLint camHeightLoc;
	GLint sunDirLoc;
	GLint sunColorLoc;
	GLint sunScatterLoc;
#endif
	int snowSerial;		// last g_snowSerial uploaded to this program
} PSXGPU_Shader;

PSXGPU_Shader g_gpu_shader_4;
PSXGPU_Shader g_gpu_shader_8;
PSXGPU_Shader g_gpu_shader_16;
PSXGPU_Shader g_gpu_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_texelSizeLoc;

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC\
    "   // returns 16 bit colour\n"\
    "   vec2 samplePSX(vec2 tc) {\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (255.0 / 16.0);\n"\
    "       float f = floor(v + 0.001);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return VRAM(clut_pos);\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns 16 bit colour\n"\
	"	vec2 samplePSX(vec2 tc) {\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * 255.0 * c_VRAMTexel.x;\n"\
	"		vec2 color_rg = VRAM(clut_pos);\n"\
	"		return VRAM(clut_pos);\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	vec2 samplePSX(vec2 tc) {\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 color_rg = VRAM(uv);\n"\
	"		return color_rg;\n"\
	"	}\n"

#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)

#define GPU_FETCH_VRAM_FUNC \
		"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).ra; }\n"
#else

#define GPU_FETCH_VRAM_FUNC \
		"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n"
#endif

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#	define GPU_DITHERING \
		"	const mat4 c_dither = mat4(\n"\
		"		-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"		+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"		-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"		+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"	vec4 dither(vec4 color) {\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);\n"\
		"		color.xyz += vec3(c_dither[dc.x][dc.y] * mod(v_texcoord.w, 2.0));\n"\
		"		return color;\n"\
		"	}\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { return array[idx]; }\n"

#else

#	define GPU_DITHERING \
		"	vec4 dither(vec4 color) { return color; }\n"

#	define GPU_ARRAY_FUNC \
		"	float _idx2(vec2 array, int idx) { return idx == 0 ? array.x : array.y; }\n"

#endif

#define GPU_FRAGMENT_SAMPLE_SHADER(bit) \
	GPU_FETCH_VRAM_FUNC\
	GPU_ARRAY_FUNC\
	GPU_SAMPLE_TEXTURE_## bit ##BIT_FUNC\
	"	uniform sampler2D s_rgLut;\n"\
	"	const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);\n"\
	"	vec4 lut(vec2 rg) { return texture2D(s_rgLut, rg - c_LUTTexel * 0.0001); }\n"\
	"	vec4 bilinearTextureSample(vec2 P) {\n"\
	"		vec2 frac = fract(P);\n"\
	"		vec2 pixel = floor(P);\n"\
	"		vec2 C11 = samplePSX(pixel);\n"\
	"		vec2 C21 = samplePSX(pixel + vec2(1.0, 0.0));\n"\
	"		vec2 C12 = samplePSX(pixel + vec2(0.0, 1.0));\n"\
	"		vec2 C22 = samplePSX(pixel + vec2(1.0, 1.0));\n"\
	"		float ax1 = mix(float(C11.r + C11.g > 0.0), float(C21.r + C21.g > 0.0), frac.x);\n"\
	"		float ax2 = mix(float(C12.r + C12.g > 0.0), float(C22.r + C22.g > 0.0), frac.x);\n"\
	"		float axm = mix(ax1, ax2, frac.y);\n"\
	"		if(axm < 0.5) { discard; }\n"\
	"		vec4 x1 = mix(lut(C11), lut(C21), frac.x);\n"\
	"		vec4 x2 = mix(lut(C12), lut(C22), frac.x);\n"\
	"		vec4 t = mix(x1, x2, frac.y);\n"\
	"		t.w = 1.0 - t.w;\n"\
	"		return t;\n"\
	"	}\n"\
	"	vec4 nearestTextureSample(vec2 P) {\n"\
	"		vec2 rg = samplePSX(P);\n"\
	"		float rgm = rg.x + rg.y;\n"\
	"		if (rgm == 0.0) { discard; }\n"\
	"		vec4 t = lut(rg);\n"\
	"		t.w = 1.0 - t.w;\n"\
	"		return t;\n"\
	"	}\n"\
	"	uniform int bilinearFilter;\n"\
	"	void main() {\n"\
	"		vec4 color = (bilinearFilter > 0) ? bilinearTextureSample(v_texcoord.xy) : nearestTextureSample(v_texcoord.xy);\n"\
	"		vec4 base = applySnow(color);\n"\
	"		vec4 lit = base * v_color;\n"\
	"		lit.rgb += base.rgb * beamLight();\n"\
	"		fragColor = dither(applyFog(lit) + vec4(volumeLight(), 0.0));\n"\
	"	}\n"

static const char* gpu_shader_common = R"(
	varying vec4 v_texcoord;
	varying vec4 v_color;
	varying vec4 v_page_clut;
	varying float v_z;
	varying vec4 v_view;	// xyz = view space position / 128, w = 1 for 3D (PGXP) geometry
)";

#if USE_PGXP && (defined(RENDERER_OGL) || (OGLES_VERSION == 3))
// Snow on the road: faces that point up (normal from screen space derivatives of the view space position) and lie near
// the road level (both given by the game in view space). The layer is closed and white, switched on at once.
static const char* gpu_shader_snow = R"(
	uniform float u_snow;
	uniform vec3 u_groundPos;
	uniform vec3 u_groundUp;

	vec4 applySnow(vec4 color) {
		vec3 p = v_view.xyz * 128.0;
		vec3 n = cross(dFdx(p), dFdy(p));	// must be outside of any branch
		float len = length(n);

		if (u_snow <= 0.0 || v_view.w < 0.5 || (v_view.w > 1.5 && v_view.w < 2.5) || len < 0.0001)	// w = 2: no snow at all
			return color;

		n /= len;
		if (dot(n, p) > 0.0)
			n = -n;		// face the camera

		// only the road: (almost) horizontal faces within ~2 m of the road level, no roofs and no walls
		float up = dot(n, u_groundUp);
		float height = abs(dot(p - u_groundPos, u_groundUp));
		float snow = smoothstep(0.8, 0.95, up) * (1.0 - smoothstep(250.0, 400.0, height)) * u_snow;

		color.rgb = mix(color.rgb, vec3(0.93, 0.95, 1.0), snow);
		return color;
	}

	uniform vec2 u_viewScale;	// draw size * 128: v_view.xy hold view x, y / 128 / draw size - 0.5 (ScreenCoordsToEmulator)

	// applied after the vertex lighting. Falls off with the square of the distance so the near street stays clear.
	uniform float u_fog;
	uniform vec3 u_fogColor;

	// Height fog density * exp(-falloff * height), integrated along the ray from the camera (height u_camHeight above
	// sea level) to the pixel; the fog colour turns towards u_sunColor when looking into the sun (Henyey-Greenstein).
	uniform float u_hfFalloff;
	uniform float u_hfDensity;
	uniform vec3 u_worldUp;		// view space, GTE units (y down)
	uniform float u_camHeight;
	uniform vec3 u_sunDir;
	uniform vec3 u_sunColor;
	uniform float u_sunScatter;

	vec4 applyFog(vec4 color) {
		float d = length(v_view.xyz) * 128.0;
		float k = d * u_fog * 0.000108;
		float f = v_view.w > 0.5 ? 1.0 - exp(-k * k) : 0.0;
		vec3 fogColor = u_fogColor;

		if (u_hfDensity > 0.0 && v_view.w > 0.5)
		{
			vec3 p = vec3((v_view.xy + 0.5) * u_viewScale, v_view.z * 128.0);
			float t = length(p);

			// the sky dome lies 256 times farther out (PGXP z scale of sky.c) and keeps its own colours
			if (t > 1.0 && t < 400000.0)
			{
				float x = u_hfFalloff * dot(p, u_worldUp);	// falloff * height gained between camera and pixel
				float shape = abs(x) > 0.001 ? (1.0 - exp(-x)) / x : 1.0 - 0.5 * x;
				float tau = u_hfDensity * exp(-u_hfFalloff * u_camHeight) * t * shape;
				f = 1.0 - (1.0 - f) * exp(-tau);

				float g = 0.6;
				float c = dot(p / t, u_sunDir);
				float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5) * (1.0 - g) * (1.0 - g) / (1.0 + g);	// 1 into the sun
				fogColor = mix(fogColor, u_sunColor, clamp(u_sunScatter * phase, 0.0, 1.0));
			}
		}

		color.rgb = mix(color.rgb, fogColor, f);
		return color;
	}

	// Headlight of the player car: a cone in view space, added on top of the vertex lighting.
	uniform float u_beam;
	uniform vec3 u_beamPos;
	uniform vec3 u_beamDir;

	vec3 beamLight() {
		vec3 p = vec3((v_view.xy + 0.5) * u_viewScale, v_view.z * 128.0);
		vec3 n = cross(dFdx(p), dFdy(p));	// must be outside of any branch
		float len = length(n);

		if (u_beam <= 0.0 || v_view.w < 0.5 || len < 0.0001)
			return vec3(0.0);

		n /= len;
		if (dot(n, p) > 0.0)
			n = -n;

		vec3 toLight = u_beamPos - p;
		float dist = max(length(toLight), 1.0);
		vec3 l = toLight / dist;

		float cone = smoothstep(0.80, 0.95, dot(-l, u_beamDir));
		float atten = clamp(1.0 - dist / 9000.0, 0.0, 1.0) * smoothstep(300.0, 2500.0, dist);	// no hot spot right at the nose
		float ndl = mix(0.4, 1.0, max(dot(n, l), 0.0));
		float dark = clamp(1.3 - dot(v_color.rgb, vec3(0.333)), 0.25, 1.0);	// less in daylight

		// the road is lit at a grazing angle (ndl near 0.4), hence the gain
		return vec3(1.0, 0.92, 0.75) * (u_beam * cone * atten * ndl * dark * 1.5);
	}

	// Light scattered by the air between the camera and the pixel: point lights (closed form of the integral of
	// 1 / (d^2 + k) along the ray) and the headlight cone (marched). Added after the fog, so it lies on top of it.
	uniform int u_lightCount;
	uniform vec3 u_lightPos[16];
	uniform vec4 u_lightCol[16];	// rgb, w = radius
	uniform float u_volDensity;
	uniform float u_beamVolume;

	vec3 volumeLight() {
		vec3 p = vec3((v_view.xy + 0.5) * u_viewScale, v_view.z * 128.0);
		float t = length(p);

		if (v_view.w < 0.5 || t < 1.0 || (u_volDensity <= 0.0 && u_beamVolume <= 0.0))
			return vec3(0.0);

		vec3 r = p / t;
		vec3 sum = vec3(0.0);

		for (int i = 0; i < 16; i++)
		{
			if (i >= u_lightCount)
				break;

			vec3 L = u_lightPos[i];
			float radius = u_lightCol[i].w;	// halo radius
			float k = radius * 0.15;		// core size: no infinite hot spot when the ray passes through the lamp
			float b = dot(L, r);
			float miss2 = max(dot(L, L) - b * b, 0.0);	// squared distance of the ray from the lamp
			float h = sqrt(miss2 + k * k);
			float scat = (atan((t - b) / h) - atan(-b / h)) * k / h;	// pi at the core
			// soft edge (no visible disc), and no lamp whose halo holds the camera (it would tint the whole view)
			float dist = length(L);
			float reach = exp(-miss2 / (radius * radius)) * smoothstep(radius * 2.0, radius * 6.0, dist) * (1.0 - smoothstep(12000.0, 24000.0, dist));

			sum += u_lightCol[i].rgb * (scat * reach);
		}

		sum *= u_volDensity * 0.05;

		if (u_beamVolume > 0.0 && u_beam > 0.0)
		{
			float seg = min(t, 9000.0);
			float beam = 0.0;

			for (int j = 0; j < 8; j++)
			{
				vec3 s = r * (seg * (float(j) + 0.5) / 8.0);
				vec3 toLight = u_beamPos - s;
				float dist = max(length(toLight), 1.0);
				float cone = smoothstep(0.80, 0.95, dot(-toLight / dist, u_beamDir));
				float atten = clamp(1.0 - dist / 9000.0, 0.0, 1.0) * smoothstep(300.0, 2500.0, dist);
				beam += cone * atten * atten;
			}

			// sum of the samples times the step length: a wall right in front of the car gets little haze
			sum += vec3(1.0, 0.92, 0.75) * (beam * seg / (8.0 * 9000.0) * u_beam * u_beamVolume * 0.15);
		}

		return sum;
	}
)";
#else
static const char* gpu_shader_snow = R"(
	vec4 applySnow(vec4 color) { return color; }
	vec4 applyFog(vec4 color) { return color; }
	vec3 beamLight() { return vec3(0.0); }
	vec3 volumeLight() { return vec3(0.0); }
)";
#endif

const char* gpu_shader_4 = GPU_FRAGMENT_SAMPLE_SHADER(4);
const char* gpu_shader_8 = GPU_FRAGMENT_SAMPLE_SHADER(8);
const char* gpu_shader_16 = GPU_FRAGMENT_SAMPLE_SHADER(16);
const char* gpu_shader_32_rgba = 
	"	uniform sampler2D s_texture;\n"\
	"	uniform vec2 texelSize;\n"\
	"	void main() {\n"\
	"		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"\
	"		vec4 color = texture2D(s_texture, tc);\n"\
	"		vec4 base = applySnow(color);\n"\
	"		vec4 lit = base * v_color;\n"\
	"		lit.rgb += base.rgb * beamLight();\n"\
	"		fragColor = dither(applyFog(lit) + vec4(volumeLight(), 0.0));\n"\
	"	}\n";

#if USE_PGXP
#	define GTE_PERSPECTIVE_CORRECTION \
		"	mat4 ofsMat = mat4(\n"\
		"		vec4(1.0,  0.0,  0.0,  0.0),\n"\
		"		vec4(0.0,  1.0,  0.0,  0.0),\n"\
		"		vec4(0.0,  0.0,  1.0,  0.0),\n"\
		"		vec4(a_zw.z, -a_zw.w,  0.0,  1.0));\n"\
		"	vec2 geom_ofs = vec2(0.5, 0.5);\n"\
		"	vec4 fragPosition = (a_zw.y > 100.0 ? ofsMat * (Projection3D * vec4((a_position.xy + geom_ofs) * vec2(1.0,-1.0) * a_zw.y, a_zw.x, 1.0)) : (Projection * vec4(a_position.xy, 0.5, 1.0)));\n" \
		"	gl_Position = fragPosition;\n"\
		"	v_view = a_zw.y > 100.0 ? vec4(a_position.xy, a_zw.x, a_texcoord.w > 1.5 ? 2.0 : 1.0) : vec4(0.0);\n"
#else
#	define GTE_PERSPECTIVE_CORRECTION \
		"	gl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"\
		"	v_view = vec4(0.0);\n"
#endif

#define GTE_VERTEX_SHADER \
	"	attribute vec4 a_position;\n"\
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"\
	"	attribute vec4 a_color;\n"\
	"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"\
	"	attribute vec4 a_zw;\n"\
	"	uniform mat4 Projection;\n"\
	"	uniform mat4 Projection3D;\n"\
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"\
	"	void main() {\n"\
	"		v_texcoord = a_texcoord;\n"\
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"\
	"		v_color = a_color;\n"\
	"		v_color.xyz *= a_texcoord.z;\n"\
	"		v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"\
	"		v_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"\
	"		v_page_clut.z = fract(a_position.w / 64.0);\n"\
	"		v_page_clut.w = floor(a_position.w / 64.0) / 512.0;\n"\
	"		v_page_clut.xy += c_UVFudge;\n"\
	"		v_page_clut.zw += c_UVFudge;\n"\
	GTE_PERSPECTIVE_CORRECTION\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"	}\n"

int GR_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024];
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
		return 1;
	
	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024];
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
		return 1;

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

ShaderID GR_Shader_Compile(const char* source, int isPsxShader)
{
#if defined(ES2_SHADERS)
	const char* GLSL_HEADER_VERT = R"(
		#version 100
		precision lowp  int;
		precision highp float;
	)";

	const char* GLSL_HEADER_FRAG = R"(
		#version 100
		precision lowp  int;
		precision highp float;
		#define fragColor gl_FragColor
	)";
#elif defined(ES3_SHADERS)
	const char* GLSL_HEADER_VERT = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

	const char* GLSL_HEADER_FRAG = R"(
		#version 300 es
		precision lowp  int;
		precision highp float;
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#else
	const char* GLSL_HEADER_VERT = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define varying   out
		#define attribute in
		#define texture2D texture
	)";

	const char* GLSL_HEADER_FRAG = R"(
		#version 140
		precision lowp  int;
		precision highp float;
		#define varying     in
		#define texture2D   texture
		out vec4 fragColor;
	)";
#endif

	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;

	strcat(extra_vs_defines, "#define VERTEX\n");
	strcat(extra_fs_defines, "#define FRAGMENT\n");
	if (g_cfg_bilinearFiltering)
	{
		strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
	}

	const char* vs_list_psx[] = { 
		GLSL_HEADER_VERT,
		extra_vs_defines,
		gpu_shader_common,
		GTE_VERTEX_SHADER
	};
	const char* fs_list_psx[] = {
		GLSL_HEADER_FRAG,
		extra_fs_defines,
		gpu_shader_common,
		GPU_DITHERING,
		gpu_shader_snow,
		source
	};

	const char* vs_list_src[] = { 
		GLSL_HEADER_VERT,
		extra_vs_defines,
		source,
	};
	const char* fs_list_src[] = {
		GLSL_HEADER_FRAG,
		extra_fs_defines,
		source
	};

	const char** vs_list = isPsxShader ? vs_list_psx : vs_list_src;
	const char** fs_list = isPsxShader ? fs_list_psx : fs_list_src;
	const int vs_list_cnt = isPsxShader ? 4 : 3;
	const int fs_list_cnt = isPsxShader ? 6 : 3;

	GLuint program = glCreateProgram();

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vertexShader, vs_list_cnt, vs_list, NULL);
		glCompileShader(vertexShader);

		if( GR_Shader_CheckShaderStatus(vertexShader) == 0 )
			eprinterr("Failed to compile Vertex Shader!\n");
	
		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fragmentShader, fs_list_cnt, fs_list, NULL);
		glCompileShader(fragmentShader);

		if(GR_Shader_CheckShaderStatus(fragmentShader) == 0)
			eprinterr("Failed to compile Fragment Shader!\n");
	
		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");

#if USE_PGXP
	glBindAttribLocation(program, a_zw, "a_zw");
#endif

	glLinkProgram(program);
	if(GR_Shader_CheckProgramStatus(program) == 0)
		eprinterr("Failed to link Shader!\n");

	GLint sampler = 0;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &sampler);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures()
{
	unsigned int whitePixelData = 0xFFFFFFFF;

#if USE_OPENGL
	glGenTextures(1, &g_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}

	glGenTextures(1, &g_rgLutTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgLUT);

		glBindTexture(GL_TEXTURE_2D, 0);
	}
#endif
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/)
{
	TextureID newTexture;
	glGenTextures(1, &newTexture);

	glBindTexture(GL_TEXTURE_2D, newTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	
	// another WebGL stuff. Texture will be black without clamp to edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);

	return newTexture;
}

void GR_CompilePSXShader(PSXGPU_Shader* sh, const char* source)
{
	sh->shader = GR_Shader_Compile(source, true);

#if USE_OPENGL
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->texLoc = glGetUniformLocation(sh->shader, "s_texture");
	sh->lutLoc = glGetUniformLocation(sh->shader, "s_rgLut");
	sh->snowLoc = glGetUniformLocation(sh->shader, "u_snow");
	sh->snowGroundPosLoc = glGetUniformLocation(sh->shader, "u_groundPos");
	sh->snowGroundUpLoc = glGetUniformLocation(sh->shader, "u_groundUp");
	sh->fogLoc = glGetUniformLocation(sh->shader, "u_fog");
	sh->fogColorLoc = glGetUniformLocation(sh->shader, "u_fogColor");
	sh->beamLoc = glGetUniformLocation(sh->shader, "u_beam");
	sh->beamPosLoc = glGetUniformLocation(sh->shader, "u_beamPos");
	sh->beamDirLoc = glGetUniformLocation(sh->shader, "u_beamDir");
	sh->lightCountLoc = glGetUniformLocation(sh->shader, "u_lightCount");
	sh->lightPosLoc = glGetUniformLocation(sh->shader, "u_lightPos[0]");
	sh->lightColLoc = glGetUniformLocation(sh->shader, "u_lightCol[0]");
	sh->volDensityLoc = glGetUniformLocation(sh->shader, "u_volDensity");
	sh->beamVolumeLoc = glGetUniformLocation(sh->shader, "u_beamVolume");
	sh->viewScaleLoc = glGetUniformLocation(sh->shader, "u_viewScale");
	sh->hfFalloffLoc = glGetUniformLocation(sh->shader, "u_hfFalloff");
	sh->hfDensityLoc = glGetUniformLocation(sh->shader, "u_hfDensity");
	sh->worldUpLoc = glGetUniformLocation(sh->shader, "u_worldUp");
	sh->camHeightLoc = glGetUniformLocation(sh->shader, "u_camHeight");
	sh->sunDirLoc = glGetUniformLocation(sh->shader, "u_sunDir");
	sh->sunColorLoc = glGetUniformLocation(sh->shader, "u_sunColor");
	sh->sunScatterLoc = glGetUniformLocation(sh->shader, "u_sunScatter");
	sh->snowSerial = 0;
#if USE_PGXP
	sh->projection3DLoc = glGetUniformLocation(sh->shader, "Projection3D");
#endif
#endif
}

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gpu_shader_4, gpu_shader_4);
	GR_CompilePSXShader(&g_gpu_shader_8, gpu_shader_8);
	GR_CompilePSXShader(&g_gpu_shader_16, gpu_shader_16);
	GR_CompilePSXShader(&g_gpu_shader_32_rgba, gpu_shader_32_rgba);
}

void GR_InitRG8LUT()
{
	for (u_short y = 0; y < LUT_HEIGHT; y++)
	{
		u_char* row = rgLUT + y * (LUT_HEIGHT * 4);
		for (u_short x = 0; x < LUT_WIDTH; x++)
		{
			const u_short c = (y << 8) | x;
			u_char* pixel = row + x * 4;
			pixel[0] = (u_char)((c & 31)) << 3;
			pixel[1] = (u_char)((c >> 5) & 31) << 3;
			pixel[2] = (u_char)((c >> 10) & 31) << 3;
			pixel[3] = (u_char)((c >> 15) & 1) << 7;
		}
	}
}

int GR_InitialisePSX()
{
	SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
	GR_InitRG8LUT();
	GR_GenerateCommonTextures();
	GR_InitialisePSXShaders();

#if USE_OPENGL
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

	// gen framebuffer
	{
		memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
		PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// make a special texture
		// it will be resized later
		glGenTextures(1, &g_fbTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_fbTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glBlitFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen offscreen RT
	{
		memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
		PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// offscreen texture render target
		glGenTextures(1, &g_offscreenRTTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glOffscreenFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_offscreenRTTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen VRAM textures.
	// double-buffered
	{
		int i;

		glGenTextures(2, g_vramTexturesDouble);

		for(i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// set storage size
			glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
		}

		g_vramTexture = g_vramTexturesDouble[0];

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &g_glVRAMFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(g_glVertexArray[i]);

			glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
		}

		glBindVertexArray(0);
	}
#else
#error
#endif

	GR_ResetDevice();

	return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

#if USE_OPENGL 
	// -1..1
	float z = (znear + zfar) / (znear - zfar);
#endif

	float ortho[16] = {
		a, 0, 0, 0,
		0, b, 0, 0,
		0, 0, c, 0,
		x, y, z, 1
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	float sinF, cosF;
	sinF = sinf(0.5f * fov);
	cosF = cosf(0.5f * fov);

	float h = cosF / sinF;
	float w = (h * height) / width;

	float persp[16] = {
		w, 0, 0, 0,
		0, h, 0, 0,
		0, 0, (zFar + zNear) / (zFar - zNear), 1,
		0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(	rect->x - activeDispEnv.disp.x > 0 ||
			rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 ||
			rect->h < activeDispEnv.disp.h - 1));

	GR_SetScissorState(scissorOn);

	if (!scissorOn)
		return;

#if USE_PGXP
	const float emuScreenAspect = 1.0f / (PSX_SCREEN_ASPECT * (float)g_windowWidth / (float)g_windowHeight);
#else
	const float emuScreenAspect = 1.0f;
#endif

	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	// first map to 0..1
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	// then map to screen
	{
		clipRectX -= 0.5f;

		clipRectX *= emuScreenAspect;
		clipRectW *= emuScreenAspect;

		clipRectX += 0.5f;
	}

#if USE_OPENGL
	// adjust scissor rectangle by the backbuffer size (window dimensions)
	const float flipOffset = g_windowHeight - clipRectH * (float)g_windowHeight;
	const float crx = clipRectX * (float)g_windowWidth;
	const float cry = clipRectY * (float)g_windowHeight;
	const float crw = clipRectW * (float)g_windowWidth;
	const float crh = clipRectH * (float)g_windowHeight;

	glScissor(crx, flipOffset - cry, crw, crh);
#endif
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{
#if USE_PGXP
	float psxScreenW, psxScreenH;
	float emuScreenAspect;

	emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);

	psxScreenW = activeDispEnv.disp.w;
	psxScreenH = activeDispEnv.disp.h;

	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;

	rect->w = psxScreenW * emuScreenAspect * PSX_SCREEN_ASPECT; // windowWidth;
	rect->h = psxScreenH; // windowHeight;

	rect->x -= (rect->w - activeDispEnv.disp.w) / 2;

	rect->w += rect->x;
#else
	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = activeDispEnv.disp.w;
	rect->h = activeDispEnv.disp.h;
#endif
}

void GR_SetShader(const ShaderID shader)
{
	if (g_PreviousShader != shader)
	{
#if USE_OPENGL
		glUseProgram(shader);
#else
#error
#endif

		g_PreviousShader = shader;
	}
}


void PsyX_SetSnow(float amount, const float* groundPos, const float* groundUp)
{
	g_snowAmount = amount;
	memcpy(g_snowGroundPos, groundPos, sizeof(g_snowGroundPos));
	memcpy(g_snowGroundUp, groundUp, sizeof(g_snowGroundUp));
	g_snowSerial++;
}

void PsyX_SetFog(float amount, const float* color)
{
	g_fogAmount = amount;
	memcpy(g_fogColor, color, sizeof(g_fogColor));
	g_snowSerial++;
}

void PsyX_SetBeam(float amount, const float* pos, const float* dir)
{
	g_beamAmount = amount;
	memcpy(g_beamPos, pos, sizeof(g_beamPos));
	memcpy(g_beamDir, dir, sizeof(g_beamDir));
	g_snowSerial++;
}

void PsyX_SetVolumeLights(int count, const float* pos, const float* color, float density)
{
	if (count > PSYX_MAX_VOLUME_LIGHTS)
		count = PSYX_MAX_VOLUME_LIGHTS;

	g_volLightCount = count > 0 ? count : 0;
	memcpy(g_volLightPos, pos, sizeof(float) * 3 * g_volLightCount);
	memcpy(g_volLightCol, color, sizeof(float) * 4 * g_volLightCount);
	g_volDensity = g_volLightCount > 0 ? density : 0.0f;
	g_snowSerial++;
}

void PsyX_SetVolumeBeam(float amount)
{
	g_beamVolume = amount;
}

void PsyX_SetAtmosphere(float heightFalloff, float heightDensity, const float* worldUpView, float camHeight, const float* sunDirView, const float* sunColor, float sunScatter)
{
	g_hfFalloff = heightFalloff;
	g_hfDensity = heightDensity;
	memcpy(g_worldUp, worldUpView, sizeof(g_worldUp));
	g_camHeight = camHeight;
	memcpy(g_sunDir, sunDirView, sizeof(g_sunDir));
	memcpy(g_sunColor, sunColor, sizeof(g_sunColor));
	g_sunScatter = sunScatter;
	g_snowSerial++;
}

// uniforms belong to a program, so every program has to get the current weather state once after it changed
static void GR_UploadSnow(PSXGPU_Shader* sh)
{
#if USE_OPENGL
	// glows and shadows are blended, the beam only lights opaque geometry
	if (sh->beamLoc != -1)
		glUniform1f(sh->beamLoc, g_PreviousBlendMode == BM_NONE ? g_beamAmount : 0.0f);

	// blended primitives (glows, shadows) would turn into grey patches, so fog is only for opaque ones
	if (sh->fogLoc != -1)
		glUniform1f(sh->fogLoc, g_PreviousBlendMode == BM_NONE ? g_fogAmount : 0.0f);

	if (sh->hfDensityLoc != -1)
		glUniform1f(sh->hfDensityLoc, g_PreviousBlendMode == BM_NONE ? g_hfDensity : 0.0f);

	// same for snow: car light reflections and shadows lie flat on the road and would turn into white squares
	if (sh->snowLoc != -1)
		glUniform1f(sh->snowLoc, g_PreviousBlendMode == BM_NONE ? g_snowAmount : 0.0f);

	// the air in front of a glow sprite or a shadow is already lit by the opaque geometry behind it
	if (sh->volDensityLoc != -1)
	{
		glUniform1f(sh->volDensityLoc, g_PreviousBlendMode == BM_NONE ? g_volDensity : 0.0f);
		glUniform1f(sh->beamVolumeLoc, g_PreviousBlendMode == BM_NONE ? g_beamVolume : 0.0f);

		if (activeDrawEnv.dfe)
			glUniform2f(sh->viewScaleLoc, activeDispEnv.disp.w * 128.0f, activeDispEnv.disp.h * 128.0f);
		else
			glUniform2f(sh->viewScaleLoc, activeDrawEnv.clip.w * 128.0f, activeDrawEnv.clip.h * 128.0f);
	}

	if (sh->snowSerial == g_snowSerial)
		return;

	sh->snowSerial = g_snowSerial;

	if (sh->snowLoc == -1)
		return;

	glUniform3fv(sh->snowGroundPosLoc, 1, g_snowGroundPos);
	glUniform3fv(sh->snowGroundUpLoc, 1, g_snowGroundUp);
	glUniform3fv(sh->fogColorLoc, 1, g_fogColor);

	if (sh->hfFalloffLoc != -1)
	{
		glUniform1f(sh->hfFalloffLoc, g_hfFalloff);
		glUniform3fv(sh->worldUpLoc, 1, g_worldUp);
		glUniform1f(sh->camHeightLoc, g_camHeight);
		glUniform3fv(sh->sunDirLoc, 1, g_sunDir);
		glUniform3fv(sh->sunColorLoc, 1, g_sunColor);
		glUniform1f(sh->sunScatterLoc, g_sunScatter);
	}

	if (sh->beamPosLoc != -1)
	{
		glUniform3fv(sh->beamPosLoc, 1, g_beamPos);
		glUniform3fv(sh->beamDirLoc, 1, g_beamDir);
	}

	if (sh->lightCountLoc != -1)
	{
		glUniform1i(sh->lightCountLoc, g_volLightCount);

		if (g_volLightCount > 0)
		{
			glUniform3fv(sh->lightPosLoc, g_volLightCount, g_volLightPos);
			glUniform4fv(sh->lightColLoc, g_volLightCount, g_volLightCol);
		}	}
#endif
}

void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	GLint texLoc = 0;
	GLint lutLoc = 0;
	GLint bilinearFilterLoc = 0;
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(g_gpu_shader_4.shader);
		GR_UploadSnow(&g_gpu_shader_4);
		bilinearFilterLoc = g_gpu_shader_4.bilinearFilterLoc;
		u_projectionLoc = g_gpu_shader_4.projectionLoc;
		u_projection3DLoc = g_gpu_shader_4.projection3DLoc;
		texLoc = g_gpu_shader_4.texLoc;
		lutLoc = g_gpu_shader_4.lutLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_8_BIT:
		GR_SetShader(g_gpu_shader_8.shader);
		GR_UploadSnow(&g_gpu_shader_8);
		bilinearFilterLoc = g_gpu_shader_8.bilinearFilterLoc;
		u_projectionLoc = g_gpu_shader_8.projectionLoc;
		u_projection3DLoc = g_gpu_shader_8.projection3DLoc;
		texLoc = g_gpu_shader_8.texLoc;
		lutLoc = g_gpu_shader_8.lutLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_16_BIT:
		GR_SetShader(g_gpu_shader_16.shader);
		GR_UploadSnow(&g_gpu_shader_16);
		bilinearFilterLoc = g_gpu_shader_16.bilinearFilterLoc;
		u_projectionLoc = g_gpu_shader_16.projectionLoc;
		u_projection3DLoc = g_gpu_shader_16.projection3DLoc;
		texLoc = g_gpu_shader_16.texLoc;
		lutLoc = g_gpu_shader_16.lutLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(g_gpu_shader_32_rgba.shader);
		GR_UploadSnow(&g_gpu_shader_32_rgba);
		bilinearFilterLoc = -1;
		u_projectionLoc = g_gpu_shader_32_rgba.projectionLoc;
		u_projection3DLoc = g_gpu_shader_32_rgba.projection3DLoc;
		texLoc = g_gpu_shader_32_rgba.texLoc;
		lutLoc = -1;
		u_texelSizeLoc = g_gpu_shader_32_rgba.texelSizeLoc;
		break;
	}

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glUniform1i(texLoc, 0);
	glUniform1i(lutLoc, 1);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, g_rgLutTexture);

	glActiveTexture(GL_TEXTURE0);

	if(bilinearFilterLoc != -1)
		glUniform1i(bilinearFilterLoc, g_cfg_bilinearFiltering);
#endif

	g_lastBoundTexture = texture;
}

void GR_SetOverrideTextureSize(int width, int height)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version
	float vec[] = { 1.0f / (float)width, 1.0f / (float)height };
	glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture == -1)
		return;

#if USE_OPENGL
	glDeleteTextures(1, &texture);
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;

	u_short* dst = vram + x + y * VRAM_WIDTH;

	if (x + w > VRAM_WIDTH)
		w = VRAM_WIDTH - x;

	if (y + h > VRAM_HEIGHT)
		h = VRAM_HEIGHT - y;

	// clear VRAM region with given color
	for (int i = 0; i < h; i++)
	{
		u_short* tmp = dst;

		for (int j = 0; j < w; j++)
			*tmp++ = r | (g << 5) | (b << 11);

		dst += VRAM_WIDTH;
	}
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;

#if USE_OPENGL
	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

void GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

	FILE* fp = fopen(outputFileName, "wb");
	if (fp == NULL)
		return;

	unsigned char TGAheader[12] = { 0,0,2,0,0,0,0,0,0,0,0,0 };
	unsigned char header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(unsigned char), 12, fp);
	fwrite(header, sizeof(unsigned char), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int* src, int x, int y, int w, int h, int update_vram, int flip_y)
{
	assert(x >= 0);
	assert(y >= 0);
	assert(x + w <= VRAM_WIDTH);
	assert(y + h <= VRAM_WIDTH);

	ushort* fb = (ushort*)malloc(w * h * sizeof(ushort));
	uint* data_src = (uint*)src;
	ushort* data_dst = (ushort*)fb;

	for (int i = 0; i < h; i++)
	{
		for (int j = 0; j < w; j++)
		{
			uint c = *data_src++;

			u_char b = ((c >> 3) & 0x1F);
			u_char g = ((c >> 11) & 0x1F);
			u_char r = ((c >> 19) & 0x1F);
			//u_char a = ((c >> 24) & 0x1F);

			int a = r == g == b == 0 ? 0 : 1;

			*data_dst++ = r | (g << 5) | (b << 10) | (a << 15);
		}
	}

	ushort* ptr = (ushort*)vram + VRAM_WIDTH * y + x;

	for (int fy = 0; fy < h; fy++)
	{
		int py = flip_y ? (h - fy - 1) : fy;
		ushort* fb_ptr = fb + (h * py / h) * w;

		for (int fx = 0; fx < w; fx++)
			ptr[fx] = fb_ptr[w * fx / w];

		ptr += VRAM_WIDTH;
	}

	free(fb);

	if (update_vram)
		vram_need_update = 1;
}

void GR_ReadFramebufferDataToVRAM()
{
	int x, y, w, h;
	if (!framebuffer_need_update)
		return;

	framebuffer_need_update = 0;

	x = g_PreviousFramebuffer.x;
	y = g_PreviousFramebuffer.y;
	w = g_PreviousFramebuffer.w;
	h = g_PreviousFramebuffer.h;

	// now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
	// read the texture
	if(g_glFramebufferPBO.pixels)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		PBO_Download(&g_glFramebufferPBO);
		glBindTexture(GL_TEXTURE_2D, 0);
		GR_CopyRGBAFramebufferToVRAM((u_int*)g_glFramebufferPBO.pixels, x, y, w, h, 0, 0);
	}
#endif
}

void GR_SetScissorState(int enable)
{
	if (g_PreviousScissorState == enable)
		return;

#if USE_OPENGL
	if (g_PreviousScissorState)
		glDisable(GL_SCISSOR_TEST);
	else
		glEnable(GL_SCISSOR_TEST);
#endif
	g_PreviousScissorState = enable;
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable)
	{
		// setup render target viewport
#if USE_PGXP
		GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
#else
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
#endif
	}
	else
	{
		// setup default viewport
#if USE_PGXP

		// these constants below are guessed
		const float perspectiveFOV = 0.9265f;
		const float perspectiveZNear = 0.25f;
		const float perspectiveZFar = 1000.0f;

		const float emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);

		GR_Ortho2D(-0.5f * emuScreenAspect * PSX_SCREEN_ASPECT, 0.5f * emuScreenAspect * PSX_SCREEN_ASPECT, 0.5f, -0.5f, -1.0f, 1.0f);
		GR_Perspective3D(perspectiveFOV, 1.0f, 1.0f / (emuScreenAspect * PSX_SCREEN_ASPECT), perspectiveZNear, perspectiveZFar);
#else
		GR_Ortho2D(0, activeDispEnv.disp.w, activeDispEnv.disp.h, 0, -1.0f, 1.0f);
#endif
	}

	if (g_PreviousOffscreenState == enable)
		return;

	g_PreviousOffscreenState = enable;

#if USE_OPENGL
	if (enable)
	{
		// set storage size first
		if (g_PreviousOffscreen.w != offscreenRect->w &&
			g_PreviousOffscreen.h != offscreenRect->h)
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreenRect->w, offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		g_PreviousOffscreen = *offscreenRect;

		GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
		glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

		// clear it out
		glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

#if USE_OFFSCREEN_BLIT
		// before drawing set source and target
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			// rebind texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

			// setup draw and read framebuffers
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);					// source is backbuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

			glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h, 
								g_PreviousOffscreen.x, g_PreviousOffscreen.y + g_PreviousOffscreen.h, g_PreviousOffscreen.x + g_PreviousOffscreen.w, g_PreviousOffscreen.y,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);

			// done, unbind
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}
#endif
		
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		// copy rendering results to VRAM texture
		{
			// reat the texture
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			//glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			PBO_Download(&g_glOffscreenPBO);
			glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

			// Don't forcely update VRAM
			GR_CopyRGBAFramebufferToVRAM((u_int*)g_glOffscreenPBO.pixels, 
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w, g_PreviousOffscreen.h, 
				USE_OFFSCREEN_BLIT == 0, 1);
		}

	}
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
#if USE_OPENGL
	// set storage size first
	if (g_PreviousFramebuffer.w != w ||
		g_PreviousFramebuffer.h != h)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	g_PreviousFramebuffer.x = x;
	g_PreviousFramebuffer.y = y;
	g_PreviousFramebuffer.w = w;
	g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
	glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

	// before drawing set source and target
	{
		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Blit framebuffer to VRAM screen area

		// before drawing set source and target
		glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

		// rebind vram texture
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

		glBlitFramebuffer(0, 0, w, h,
			x, y + h, x + w, y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		
		// done, unbind
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}

	// after drawing
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();
#endif

	GR_ReadFramebufferDataToVRAM();
#endif
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;

	int stride = w;

	if (!src)
	{
		framebuffer_need_update = 1;

		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++) {
		SDL_memcpy(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++) {
		SDL_memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	if (!vram_need_update)
		return;

	vram_need_update = 0;

#if USE_OPENGL
	g_vramTexture = g_vramTexturesDouble[g_vramTextureIdx];
	g_vramTextureIdx++;
	g_vramTextureIdx &= 1;

	glBindTexture(GL_TEXTURE_2D, g_vramTexture);

#if defined(RENDERER_OGL)
	glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#else
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#endif

#endif
}

void GR_SwapWindow()
{
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	SDL_GL_SwapWindow(g_window);
#endif

	//glFinish();
}

void GR_EnableDepth(int enable)
{
	if (g_PreviousDepthMode == enable)
		return;

	g_PreviousDepthMode = enable;

#if USE_OPENGL
	if (enable && g_cfg_pgxpZBuffer)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

void GR_SetStencilMode(int drawPrim)
{
	if (g_PreviousStencilMode == drawPrim)
		return;

	g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
#endif
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (g_PreviousBlendMode == blendMode)
		return;

#if USE_OPENGL
	if (blendMode == BM_NONE)
	{
		if (g_PreviousBlendMode != BM_NONE)
		{
			glBlendColor(1.f, 1.f, 1.f, 1.f);
			glDisable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(1);
		return;
	}
	else
	{
		if(g_PreviousBlendMode == BM_NONE)
		{
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
			glEnable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(0);
	}

	glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
	switch (blendMode) {
	case BM_AVERAGE:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case BM_ADD:
	case BM_SUBTRACT:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE); 
		break;
	}
#endif

	g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float ofs)
{
#if USE_OPENGL
	if (ofs == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, ofs);
	}
#endif
}

void GR_SetViewPort(int x, int y, int width, int height)
{
#if USE_OPENGL
	glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable)
{
#if defined(RENDERER_OGL)
	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer()
{
#if USE_OPENGL
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);

	glEnableVertexAttribArray(a_position);
	glEnableVertexAttribArray(a_texcoord);
	glEnableVertexAttribArray(a_color);
	glEnableVertexAttribArray(a_extra);

#if USE_PGXP
	glVertexAttribPointer(a_position, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
	glVertexAttribPointer(a_zw, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->z);

	glEnableVertexAttribArray(a_zw);
#else
	glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
#endif
	glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->u);
	glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex*)NULL)->r);
	glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->tcx);

	g_curVertexBuffer++;
	g_curVertexBuffer &= 1;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	//assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
	GR_BindVertexBuffer();

#if USE_OPENGL
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
#if USE_OPENGL
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char* label)
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPushDebugGroup)
		return;
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel()
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPopDebugGroup)
		return;
	glPopDebugGroup();
#endif
}