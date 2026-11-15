// Post passes that run in the middle of the draw list (a DR_PSYX_SUNSHAFTS packet, see PsyX_GPU.cpp DrawSplit).
// Sun shafts: copy colour and depth of the back buffer at half size, march from every pixel towards the sun and sum
// the sky pixels on the way (sky = depth of the sky dome, above the horizon), add the result over the back buffer.
// The GL state caches of PsyX_render.cpp are restored or invalidated at the end, so the following splits draw as before.

#include "PsyX_postfx.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"

#include "../platform.h"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

// render state of PsyX_render.cpp
extern int g_PreviousBlendMode;
extern int g_PreviousDepthMode;
extern int g_PreviousScissorState;
extern ShaderID g_PreviousShader;
extern TextureID g_lastBoundTexture;
extern GLuint g_glVertexArray[2];
extern int g_curVertexBuffer;

// one triangle over the whole target, drawn without vertex data; the fragment part follows in #else
#define FULLSCREEN_TRIANGLE_VS R"(
	varying vec2 v_uv;
#ifdef VERTEX
	const vec2 c_corner[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
	void main() {
		v_uv = c_corner[gl_VertexID];
		gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
	}
#else
)"

static const char* sunshafts_blur_shader = FULLSCREEN_TRIANGLE_VS R"(
	uniform sampler2D s_color;
	uniform sampler2D s_depth;
	uniform vec2 u_sun;			// texture space
	uniform float u_horizon;	// texture space y, sky only above it
	uniform float u_aspect;		// width / height

	// limit: sky depth measured 0.9996-0.9998 (sky z x256, zFar 1000), upgrade: if the projection changes
	const float c_skyDepth = 0.9996;
	const int c_samples = 48;
	const float c_density = 0.9;
	const float c_decay = 0.96;

	float skyLight(vec2 uv) {
		if (uv.y < u_horizon || texture2D(s_depth, uv).r < c_skyDepth)
			return 0.0;
		// light comes from the brightest sky near the sun only: a wide source gives a flat veil, not rays
		float lum = dot(texture2D(s_color, uv).rgb, vec3(0.299, 0.587, 0.114));
		vec2 d = (uv - u_sun) * vec2(u_aspect, 1.0);
		return smoothstep(0.55, 0.95, lum) * exp(-dot(d, d) * 30.0);
	}

	// GPU Gems 3 radial blur, divided by the sum of the decay weights so the result stays 0..1
	void main() {
		vec2 delta = (v_uv - u_sun) * (c_density / float(c_samples));
		vec2 uv = v_uv;
		float decay = 1.0;
		float sum = 0.0;
		float total = 0.0;
		for (int i = 0; i < c_samples; i++) {
			uv -= delta;
			sum += skyLight(uv) * decay;
			total += decay;
			decay *= c_decay;
		}
		fragColor = vec4(sum / total, 0.0, 0.0, 1.0);
	}
#endif
)";

static const char* sunshafts_composite_shader = FULLSCREEN_TRIANGLE_VS R"(
	uniform sampler2D s_rays;
	uniform vec3 u_color;		// colour times exposure
	uniform vec2 u_sun;
	uniform float u_aspect;
	void main() {
		// nothing on the sun itself: keeps the sun sprite and the flare sample (sky.c) as they were
		vec2 d = (v_uv - u_sun) * vec2(u_aspect, 1.0);
		float keepSun = smoothstep(0.05, 0.3, length(d));
		fragColor = vec4(texture2D(s_rays, v_uv).r * keepSun * u_color, 0.0);
	}
#endif
)";

// strength of the rays at amount 255
static const float c_shaftExposure = 0.6f;

static int s_failed = 0;
static int s_width = 0;
static int s_height = 0;
static int s_checkedFirstPass = 0;

static GLuint s_emptyVAO = 0;
static GLuint s_copyFBO = 0;
static GLuint s_raysFBO = 0;
static GLuint s_colorTex = 0;
static GLuint s_depthTex = 0;
static GLuint s_raysTex = 0;

static ShaderID s_blurShader = 0;
static ShaderID s_compositeShader = 0;
static GLint s_blurSunLoc = -1;
static GLint s_blurHorizonLoc = -1;
static GLint s_blurAspectLoc = -1;
static GLint s_compositeColorLoc = -1;
static GLint s_compositeSunLoc = -1;
static GLint s_compositeAspectLoc = -1;

static void PostFx_DestroyTargets()
{
	if (s_copyFBO) glDeleteFramebuffers(1, &s_copyFBO);
	if (s_raysFBO) glDeleteFramebuffers(1, &s_raysFBO);
	if (s_colorTex) glDeleteTextures(1, &s_colorTex);
	if (s_depthTex) glDeleteTextures(1, &s_depthTex);
	if (s_raysTex) glDeleteTextures(1, &s_raysTex);

	s_copyFBO = s_raysFBO = s_colorTex = s_depthTex = s_raysTex = 0;
	s_width = s_height = 0;
}

static GLuint PostFx_CreateTexture(GLint internalFormat, GLenum format, GLenum type, int w, int h, GLint filter)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return tex;
}

// targets at half the window size; the depth copy needs the format of the back buffer, else the blit fails
static int PostFx_CreateTargets(int w, int h)
{
	PostFx_DestroyTargets();

	GLint depthBits = 0, stencilBits = 0;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);

	GLint depthFormat;
	GLenum depthDataFormat, depthType, depthAttachment = GL_DEPTH_ATTACHMENT;
	if (depthBits == 24 && stencilBits == 8)
	{
		depthFormat = GL_DEPTH24_STENCIL8; depthDataFormat = GL_DEPTH_STENCIL; depthType = GL_UNSIGNED_INT_24_8;
		depthAttachment = GL_DEPTH_STENCIL_ATTACHMENT;
	}
	else if (depthBits == 32 && stencilBits == 8)
	{
		depthFormat = GL_DEPTH32F_STENCIL8; depthDataFormat = GL_DEPTH_STENCIL; depthType = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
		depthAttachment = GL_DEPTH_STENCIL_ATTACHMENT;
	}
	else if (depthBits == 24)
	{
		depthFormat = GL_DEPTH_COMPONENT24; depthDataFormat = GL_DEPTH_COMPONENT; depthType = GL_UNSIGNED_INT;
	}
	else if (depthBits == 16)
	{
		depthFormat = GL_DEPTH_COMPONENT16; depthDataFormat = GL_DEPTH_COMPONENT; depthType = GL_UNSIGNED_SHORT;
	}
	else if (depthBits == 32)
	{
		depthFormat = GL_DEPTH_COMPONENT32F; depthDataFormat = GL_DEPTH_COMPONENT; depthType = GL_FLOAT;
	}
	else
	{
		eprinterr("sun shafts: back buffer depth %d stencil %d not supported, sun shafts off\n", depthBits, stencilBits);
		return 0;
	}

	const int qw = w > 1 ? w / 2 : 1;
	const int qh = h > 1 ? h / 2 : 1;

	s_colorTex = PostFx_CreateTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, qw, qh, GL_LINEAR);
	s_depthTex = PostFx_CreateTexture(depthFormat, depthDataFormat, depthType, qw, qh, GL_NEAREST);
	s_raysTex = PostFx_CreateTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, qw, qh, GL_LINEAR);

	glGenFramebuffers(1, &s_copyFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, s_copyFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_colorTex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, depthAttachment, GL_TEXTURE_2D, s_depthTex, 0);
	const GLenum copyStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glGenFramebuffers(1, &s_raysFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, s_raysFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_raysTex, 0);
	const GLenum raysStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (copyStatus != GL_FRAMEBUFFER_COMPLETE || raysStatus != GL_FRAMEBUFFER_COMPLETE)
	{
		eprinterr("sun shafts: framebuffer incomplete (0x%x, 0x%x), sun shafts off\n", copyStatus, raysStatus);
		return 0;
	}

	eprintf("sun shafts: %dx%d targets, back buffer depth %d stencil %d\n", qw, qh, depthBits, stencilBits);

	s_width = w;
	s_height = h;
	return 1;
}

static int PostFx_CreateShaders()
{
	s_blurShader = GR_Shader_Compile(sunshafts_blur_shader, 0);
	s_compositeShader = GR_Shader_Compile(sunshafts_composite_shader, 0);

	if (!s_blurShader || !s_compositeShader)
		return 0;

	glUseProgram(s_blurShader);
	glUniform1i(glGetUniformLocation(s_blurShader, "s_color"), 0);
	glUniform1i(glGetUniformLocation(s_blurShader, "s_depth"), 1);
	s_blurSunLoc = glGetUniformLocation(s_blurShader, "u_sun");
	s_blurHorizonLoc = glGetUniformLocation(s_blurShader, "u_horizon");
	s_blurAspectLoc = glGetUniformLocation(s_blurShader, "u_aspect");

	glUseProgram(s_compositeShader);
	glUniform1i(glGetUniformLocation(s_compositeShader, "s_rays"), 0);
	s_compositeColorLoc = glGetUniformLocation(s_compositeShader, "u_color");
	s_compositeSunLoc = glGetUniformLocation(s_compositeShader, "u_sun");
	s_compositeAspectLoc = glGetUniformLocation(s_compositeShader, "u_aspect");

	glGenVertexArrays(1, &s_emptyVAO);
	return 1;
}

// the splits after this one expect the state that PsyX_render.cpp has cached
static void PostFx_RestoreState(GLboolean stencilTest)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, g_windowWidth, g_windowHeight);

	glBindVertexArray(g_glVertexArray[g_curVertexBuffer ^ 1]);	// the one GR_UpdateVertexBuffer bound last

	if (g_PreviousDepthMode && g_cfg_pgxpZBuffer)
		glEnable(GL_DEPTH_TEST);

	if (stencilTest)
		glEnable(GL_STENCIL_TEST);

	if (g_PreviousScissorState)
		glEnable(GL_SCISSOR_TEST);

	// blending is off now: let GR_SetBlendMode switch the cached mode on again
	const BlendMode blendMode = (BlendMode)g_PreviousBlendMode;
	glBlendColor(1.f, 1.f, 1.f, 1.f);
	glDisable(GL_BLEND);
	g_PreviousBlendMode = BM_NONE;
	if (blendMode != BM_NONE)
		GR_SetBlendMode(blendMode);

	g_PreviousShader = (ShaderID)-1;
	g_lastBoundTexture = (TextureID)-1;
	glActiveTexture(GL_TEXTURE0);
}

void PsyX_PostFx_SunShafts(int amount, float sunX, float sunY, float horizonY, const float* color)
{
	if (s_failed || amount <= 0 || g_windowWidth <= 0 || g_windowHeight <= 0)
		return;

	if (!s_blurShader && !PostFx_CreateShaders())
	{
		eprinterr("sun shafts: shader setup failed, sun shafts off\n");
		s_failed = 1;
		return;
	}

	if ((s_width != g_windowWidth || s_height != g_windowHeight) && !PostFx_CreateTargets(g_windowWidth, g_windowHeight))
	{
		s_failed = 1;
		return;
	}

	const int w = g_windowWidth;
	const int h = g_windowHeight;
	const int qw = w > 1 ? w / 2 : 1;
	const int qh = h > 1 ? h / 2 : 1;

	// PSX screen (-0.5..0.5) to texture space: the 2D projection keeps the height and centres the 4:3 width
	const float psxToU = (float)h / (0.75f * (float)w);
	const float sunU = 0.5f + sunX * psxToU;
	const float sunV = 0.5f - sunY;
	const float horizonV = 0.5f - horizonY;

	if (!s_checkedFirstPass)
	{
		while (glGetError() != GL_NO_ERROR) {}	// errors of earlier draws
	}

	const GLboolean stencilTest = glIsEnabled(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	// half size copy of the frame so far
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_copyFBO);
	glBlitFramebuffer(0, 0, w, h, 0, 0, qw, qh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glBlitFramebuffer(0, 0, w, h, 0, 0, qw, qh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

	glBindVertexArray(s_emptyVAO);

	// rays at half size
	glBindFramebuffer(GL_FRAMEBUFFER, s_raysFBO);
	glViewport(0, 0, qw, qh);
	glUseProgram(s_blurShader);
	glUniform2f(s_blurSunLoc, sunU, sunV);
	glUniform1f(s_blurHorizonLoc, horizonV);
	glUniform1f(s_blurAspectLoc, (float)w / (float)h);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, s_depthTex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_colorTex);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// add them over the back buffer, keep its alpha
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, w, h);
	glUseProgram(s_compositeShader);
	const float exposure = (float)amount / 255.0f * c_shaftExposure;
	glUniform3f(s_compositeColorLoc, color[0] * exposure, color[1] * exposure, color[2] * exposure);
	glUniform2f(s_compositeSunLoc, sunU, sunV);
	glUniform1f(s_compositeAspectLoc, (float)w / (float)h);
	glBindTexture(GL_TEXTURE_2D, s_raysTex);
	glEnable(GL_BLEND);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	if (!s_checkedFirstPass)
	{
		// a blit with a depth format that differs from the back buffer fails here
		const GLenum err = glGetError();
		eprintf("sun shafts: first pass, GL error 0x%x\n", err);
		s_checkedFirstPass = 1;
		if (err != GL_NO_ERROR)
			s_failed = 1;
	}

	PostFx_RestoreState(stencilTest);
}

void PsyX_PostFx_Shutdown(void)
{
	PostFx_DestroyTargets();

	if (s_blurShader) glDeleteProgram(s_blurShader);
	if (s_compositeShader) glDeleteProgram(s_compositeShader);
	if (s_emptyVAO) glDeleteVertexArrays(1, &s_emptyVAO);

	s_blurShader = s_compositeShader = 0;
	s_emptyVAO = 0;
}

#else

void PsyX_PostFx_SunShafts(int amount, float sunX, float sunY, float horizonY, const float* color)
{
}

void PsyX_PostFx_Shutdown(void)
{
}

#endif
