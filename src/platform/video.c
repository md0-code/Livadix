/* The presenter (the platform layer): layout, display modes, the
 * OpenGL path and the CRT shader.
 *
 * The window is opened for OpenGL 3.3 whenever it can be: the picture is
 * then drawn through a pass-through shader or the CRT shader, and the filter
 * is a texture parameter, so every option switches while Livadix runs. Only
 * the GL entry points that needs are resolved, through SDL, so no loader
 * library is pulled in. When any of it fails, the SDL renderer takes over
 * with everything but the CRT shader.
 */

#include "video.h"
#include "shaders.h"

#include <SDL_opengl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the OpenGL entry points ---------------------------------------------- */

typedef void (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void (APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef GLint (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void (APIENTRY *PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClear)(GLbitfield);
typedef void (APIENTRY *PFN_glFinish)(void);
typedef void (APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glPixelStorei)(GLenum, GLint);
typedef void (APIENTRY *PFN_glEnable)(GLenum);
typedef void (APIENTRY *PFN_glDisable)(GLenum);
typedef void (APIENTRY *PFN_glBlendFunc)(GLenum, GLenum);

static struct {
    PFN_glGenVertexArrays GenVertexArrays;
    PFN_glBindVertexArray BindVertexArray;
    PFN_glDeleteVertexArrays DeleteVertexArrays;
    PFN_glGenBuffers GenBuffers;
    PFN_glBindBuffer BindBuffer;
    PFN_glBufferData BufferData;
    PFN_glDeleteBuffers DeleteBuffers;
    PFN_glVertexAttribPointer VertexAttribPointer;
    PFN_glEnableVertexAttribArray EnableVertexAttribArray;
    PFN_glCreateShader CreateShader;
    PFN_glShaderSource ShaderSource;
    PFN_glCompileShader CompileShader;
    PFN_glGetShaderiv GetShaderiv;
    PFN_glGetShaderInfoLog GetShaderInfoLog;
    PFN_glDeleteShader DeleteShader;
    PFN_glCreateProgram CreateProgram;
    PFN_glAttachShader AttachShader;
    PFN_glLinkProgram LinkProgram;
    PFN_glGetProgramiv GetProgramiv;
    PFN_glGetProgramInfoLog GetProgramInfoLog;
    PFN_glUseProgram UseProgram;
    PFN_glDeleteProgram DeleteProgram;
    PFN_glGetUniformLocation GetUniformLocation;
    PFN_glUniform1i Uniform1i;
    PFN_glUniform1f Uniform1f;
    PFN_glUniform2f Uniform2f;
    PFN_glActiveTexture ActiveTexture;
    PFN_glGenTextures GenTextures;
    PFN_glDeleteTextures DeleteTextures;
    PFN_glBindTexture BindTexture;
    PFN_glTexParameteri TexParameteri;
    PFN_glTexImage2D TexImage2D;
    PFN_glTexSubImage2D TexSubImage2D;
    PFN_glViewport Viewport;
    PFN_glClearColor ClearColor;
    PFN_glClear Clear;
    PFN_glFinish Finish;
    PFN_glDrawArrays DrawArrays;
    PFN_glReadPixels ReadPixels;
    PFN_glPixelStorei PixelStorei;
    PFN_glEnable Enable;
    PFN_glDisable Disable;
    PFN_glBlendFunc BlendFunc;
} gl;

#define MC_GL_LOAD(name)                                                  \
    do {                                                                  \
        gl.name = (PFN_gl##name)SDL_GL_GetProcAddress("gl" #name);        \
        if (!gl.name) return false;                                       \
    } while (0)

static bool load_gl(void) {
    MC_GL_LOAD(GenVertexArrays);
    MC_GL_LOAD(BindVertexArray);
    MC_GL_LOAD(DeleteVertexArrays);
    MC_GL_LOAD(GenBuffers);
    MC_GL_LOAD(BindBuffer);
    MC_GL_LOAD(BufferData);
    MC_GL_LOAD(DeleteBuffers);
    MC_GL_LOAD(VertexAttribPointer);
    MC_GL_LOAD(EnableVertexAttribArray);
    MC_GL_LOAD(CreateShader);
    MC_GL_LOAD(ShaderSource);
    MC_GL_LOAD(CompileShader);
    MC_GL_LOAD(GetShaderiv);
    MC_GL_LOAD(GetShaderInfoLog);
    MC_GL_LOAD(DeleteShader);
    MC_GL_LOAD(CreateProgram);
    MC_GL_LOAD(AttachShader);
    MC_GL_LOAD(LinkProgram);
    MC_GL_LOAD(GetProgramiv);
    MC_GL_LOAD(GetProgramInfoLog);
    MC_GL_LOAD(UseProgram);
    MC_GL_LOAD(DeleteProgram);
    MC_GL_LOAD(GetUniformLocation);
    MC_GL_LOAD(Uniform1i);
    MC_GL_LOAD(Uniform1f);
    MC_GL_LOAD(Uniform2f);
    MC_GL_LOAD(ActiveTexture);
    MC_GL_LOAD(GenTextures);
    MC_GL_LOAD(DeleteTextures);
    MC_GL_LOAD(BindTexture);
    MC_GL_LOAD(TexParameteri);
    MC_GL_LOAD(TexImage2D);
    MC_GL_LOAD(TexSubImage2D);
    MC_GL_LOAD(Viewport);
    MC_GL_LOAD(ClearColor);
    MC_GL_LOAD(Clear);
    MC_GL_LOAD(Finish);
    MC_GL_LOAD(DrawArrays);
    MC_GL_LOAD(ReadPixels);
    MC_GL_LOAD(PixelStorei);
    MC_GL_LOAD(Enable);
    MC_GL_LOAD(Disable);
    MC_GL_LOAD(BlendFunc);
    return true;
}

/* --- the presenter ---------------------------------------------------------- */

struct mc_video {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *prescaled;     /* sharp without OpenGL: the picture at a whole scale */
    int pre_w, pre_h;
    int tex_w, tex_h;           /* what the texture currently holds */
    int tex_filter;             /* the scale mode the texture was given, -1 none */

    int screen_w, screen_h, aspect_w, aspect_h;   /* aspect_w 0: square pixels */
    int shape_w, shape_h;
    int scale;
    bool vsync;
    bool fullscreen;
    int win_x, win_y, win_w, win_h;   /* the window's place before full screen */
    int filter;
    bool crt;                   /* the shader draws the picture */
    int look, phosphor;         /* what it draws: MC_VIDEO_LOOK_*, MC_VIDEO_PHOSPHOR_* */

    struct {
        SDL_GLContext context;
        GLuint plain, crt, vao, vbo, texture, overlay;
        int overlay_w, overlay_h;
        bool ready;
    } gl;

    const uint32_t *overlay;    /* drawn over the picture (the caller's), overlay_w x overlay_h */
    int overlay_w, overlay_h;
    SDL_Texture *overlay_tex;   /* without OpenGL */
    int overlay_tex_w, overlay_tex_h;

    char shot_path[MC_PATH_MAX];
    bool shot_pending, shot_raw;
    char shot_done[MC_PATH_MAX];
    bool shot_result;
};

/* The picture's unit: the screen's width and the height it had on a
 * monitor of its shape (320x200 shown at 4:3 is 320x240 per step). */
static void unit(const mc_video *v, int *uw, int *uh) {
    *uw = v->screen_w;
    *uh = v->aspect_w > 0 ? v->screen_w * v->aspect_h / v->aspect_w : v->screen_h;
    if (*uh <= 0) *uh = v->screen_h;
}

/* Where the picture falls in an output of the given size. A whole scale N
 * gives N units, or the largest whole number that fits (at least 1); scale 0
 * takes the largest rectangle of the unit's shape that fits. */
static SDL_Rect layout(const mc_video *v, int out_w, int out_h) {
    int uw, uh;
    unit(v, &uw, &uh);
    SDL_Rect r;
    if (v->scale > 0) {
        int scale = v->scale, fits = out_w / uw < out_h / uh ? out_w / uw : out_h / uh;
        if (scale > fits) scale = fits > 1 ? fits : 1;
        r.w = uw * scale;
        r.h = uh * scale;
    } else if ((long)out_w * uh <= (long)out_h * uw) {
        r.w = out_w;
        r.h = (int)((long)out_w * uh / uw);
    } else {
        r.h = out_h;
        r.w = (int)((long)out_h * uw / uh);
    }
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

/* Nearest or linear for a picture `w` wide. A picture larger than the
 * screen is always smoothed, since it is mostly scaled down, where hard
 * sampling only aliases. Sharp reads a linear texture at shifted positions. */
static bool smooth(const mc_video *v, int w) {
    return w > v->screen_w || v->filter == MC_FILTER_LINEAR || v->filter == MC_FILTER_SHARP;
}
/* Sharp bilinear applies to the screen's picture in every look but the CRT,
 * whose preset does its own smoothing. */
static bool sharp(const mc_video *v, int w) {
    return v->filter == MC_FILTER_SHARP && w <= v->screen_w && !(v->crt && v->look == MC_VIDEO_LOOK_CRT);
}

static void save_pixels(mc_video *v, int w, int h, const void *pixels, int pitch, bool flip) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    v->shot_pending = false;
    v->shot_result = true;
    v->shot_done[0] = '\0';
    if (!s) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)pixels + (size_t)(flip ? h - 1 - y : y) * (size_t)pitch;
        memcpy((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch, src, (size_t)w * 4);
    }
    if (SDL_SaveBMP(s, v->shot_path) == 0) {
        snprintf(v->shot_done, sizeof v->shot_done, "%s", v->shot_path);
    } else {
        SDL_Log("could not save %s: %s", v->shot_path, SDL_GetError());
    }
    SDL_FreeSurface(s);
}

static GLuint compile(GLenum kind, const char *source, char *log, int log_size) {
    GLint ok = 0;
    GLuint shader = gl.CreateShader(kind);
    if (!shader) {
        return 0;
    }
    gl.ShaderSource(shader, 1, &source, NULL);
    gl.CompileShader(shader);
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (log && log_size > 0) {
            gl.GetShaderInfoLog(shader, log_size, NULL, log);
        }
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *vertex, const char *fragment, char *log, int log_size) {
    GLint ok = 0;
    GLuint vs = compile(GL_VERTEX_SHADER, vertex, log, log_size);
    if (!vs) {
        return 0;
    }
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, log, log_size);
    if (!fs) {
        gl.DeleteShader(vs);
        return 0;
    }
    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!ok) {
        if (log && log_size > 0) {
            gl.GetProgramInfoLog(program, log_size, NULL, log);
        }
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

/* A screen-filling quad: position then texture coordinate, with the picture's
 * first row at the top. */
static const GLfloat QUAD[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

static void gl_release(mc_video *v) {
    if (v->gl.ready) {
        if (v->gl.vbo) gl.DeleteBuffers(1, &v->gl.vbo);
        if (v->gl.vao) gl.DeleteVertexArrays(1, &v->gl.vao);
        if (v->gl.texture) gl.DeleteTextures(1, &v->gl.texture);
        if (v->gl.overlay) gl.DeleteTextures(1, &v->gl.overlay);
        if (v->gl.plain) gl.DeleteProgram(v->gl.plain);
        if (v->gl.crt) gl.DeleteProgram(v->gl.crt);
    }
    if (v->gl.context) SDL_GL_DeleteContext(v->gl.context);
    memset(&v->gl, 0, sizeof v->gl);
}

static bool gl_init(mc_video *v) {
    char log[512];
    log[0] = '\0';
    v->gl.context = SDL_GL_CreateContext(v->window);
    if (!v->gl.context) {
        SDL_Log("no OpenGL 3.3 context (%s); the CRT shader is unavailable", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(v->window, v->gl.context);
    SDL_GL_SetSwapInterval(v->vsync ? 1 : 0);
    if (!load_gl()) {
        SDL_Log("this OpenGL driver lacks entry points the presenter needs");
        return false;
    }
    v->gl.plain = link_program(MC_PRESENT_VERTEX, MC_NONE_FRAGMENT, log, (int)sizeof log);
    if (!v->gl.plain) {
        SDL_Log("the plain shader did not build (%s)", log);
        return false;
    }
    v->gl.crt = link_program(MC_PRESENT_VERTEX, MC_CRT_FRAGMENT, log, (int)sizeof log);
    if (!v->gl.crt) SDL_Log("the CRT shader did not build (%s); it is unavailable", log);
    gl.GenVertexArrays(1, &v->gl.vao);
    gl.BindVertexArray(v->gl.vao);
    gl.GenBuffers(1, &v->gl.vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, v->gl.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof QUAD, QUAD, GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat), (const void *)0);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat),
                           (const void *)(2 * sizeof(GLfloat)));
    gl.EnableVertexAttribArray(1);
    gl.GenTextures(1, &v->gl.texture);
    gl.BindTexture(GL_TEXTURE_2D, v->gl.texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.GenTextures(1, &v->gl.overlay);
    gl.BindTexture(GL_TEXTURE_2D, v->gl.overlay);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    v->gl.ready = true;
    return true;
}

/* The shader's parameters per look: the CRT is the original preset; plain
 * (used only for a phosphor colour) and the LCD turn every CRT effect off. */
static void gl_uniforms(const mc_video *v, GLuint program, const SDL_Rect *r, int w, int h) {
    const bool crt = v->look == MC_VIDEO_LOOK_CRT;
    /* The shader's MONOCHROME_TYPE per phosphor: gray 1, white 4. */
    static const float types[] = {MC_CRT_MONOCHROME_TYPE, 4.0f, 1.0f};
    const int phosphor = v->phosphor >= 0 && v->phosphor < 3 ? v->phosphor : 0;
    const struct {
        const char *name;
        float value;
    } p[] = {
        {"OVERSCAN_X", crt ? MC_CRT_OVERSCAN_X : 1.0f}, {"OVERSCAN_Y", crt ? MC_CRT_OVERSCAN_Y : 1.0f},
        {"CURVATURE_X", crt ? MC_CRT_CURVATURE_X : 0.0f}, {"CURVATURE_Y", crt ? MC_CRT_CURVATURE_Y : 0.0f},
        {"BLUR_STRENGTH", crt ? MC_CRT_BLUR_STRENGTH : 0.0f},
        {"MONOCHROME_DISPLAY", phosphor ? 1.0f : MC_CRT_MONOCHROME_DISPLAY},
        {"MONOCHROME_TYPE", types[phosphor]},
        {"MONOCHROME_COLORS", MC_CRT_MONOCHROME_COLORS},
        {"CORNER_SIZE", crt ? MC_CRT_CORNER_SIZE : 0.0f},
        {"SCANLINES_STRENGTH", MC_CRT_SCANLINES_STRENGTH},
        {"LIGHT_STRENGTH", crt ? MC_CRT_LIGHT_STRENGTH : 0.0f},
        {"GAMMA", crt ? MC_CRT_GAMMA : 1.0f}, {"CONTRAST", MC_CRT_CONTRAST},
        {"SATURATION", MC_CRT_SATURATION}, {"BRIGHTNESS", MC_CRT_BRIGHTNESS},
        {"CRT_MASK", crt ? MC_CRT_MASK : 0.0f},
        {"PIXEL_GRID", v->look == MC_VIDEO_LOOK_LCD ? MC_LCD_PIXEL_GRID : 0.0f},
        {"SHARP", sharp(v, w) ? 1.0f : 0.0f},
    };
    GLint at = gl.GetUniformLocation(program, "uSource");
    if (at >= 0) gl.Uniform1i(at, 0);
    if ((at = gl.GetUniformLocation(program, "uOutputSize")) >= 0) gl.Uniform2f(at, (GLfloat)r->w, (GLfloat)r->h);
    if ((at = gl.GetUniformLocation(program, "uTextureSize")) >= 0) gl.Uniform2f(at, (GLfloat)w, (GLfloat)h);
    if ((at = gl.GetUniformLocation(program, "uInputSize")) >= 0) gl.Uniform2f(at, (GLfloat)w, (GLfloat)h);
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
        if ((at = gl.GetUniformLocation(program, p[i].name)) >= 0) gl.Uniform1f(at, p[i].value);
    }
}

static void gl_present(mc_video *v, const uint32_t *frame, int w, int h) {
    int out_w, out_h;
    SDL_GL_GetDrawableSize(v->window, &out_w, &out_h);
    const SDL_Rect r = layout(v, out_w, out_h);
    const bool use_crt = v->crt && v->gl.crt;

    gl.Viewport(0, 0, out_w, out_h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(r.x, out_h - r.y - r.h, r.w, r.h);
    const GLuint program = use_crt ? v->gl.crt : v->gl.plain;
    gl.UseProgram(program);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, v->gl.texture);
    if (w != v->tex_w || h != v->tex_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        v->tex_w = w;
        v->tex_h = h;
    }
    /* The CRT shader samples smoothly whatever the filter says: that is the
     * one setting of its own preset. The other looks keep the filter. */
    const int f = (use_crt && v->look == MC_VIDEO_LOOK_CRT) || smooth(v, w) ? GL_LINEAR : GL_NEAREST;
    if (f != v->tex_filter) {
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        v->tex_filter = f;
    }
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, frame);
    gl_uniforms(v, program, &r, w, h);
    gl.BindVertexArray(v->gl.vao);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (v->overlay) {
        /* The overlay over the whole window, blended by its alpha: crisp at a
         * whole multiple, smoothed otherwise. */
        const int ow = v->overlay_w, oh = v->overlay_h;
        const SDL_Rect o = mc_video_overlay_rect(ow, oh, out_w, out_h);
        gl.Viewport(o.x, out_h - o.y - o.h, o.w, o.h);
        gl.UseProgram(v->gl.plain);
        gl.BindTexture(GL_TEXTURE_2D, v->gl.overlay);
        if (ow != v->gl.overlay_w || oh != v->gl.overlay_h) {
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ow, oh, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
            v->gl.overlay_w = ow;
            v->gl.overlay_h = oh;
        }
        const int whole = o.w % ow == 0 && o.h % oh == 0 && o.w / ow == o.h / oh;
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, whole ? GL_NEAREST : GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, whole ? GL_NEAREST : GL_LINEAR);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_BGRA, GL_UNSIGNED_BYTE, v->overlay);
        GLint at = gl.GetUniformLocation(v->gl.plain, "uSource");
        if (at >= 0) gl.Uniform1i(at, 0);
        if ((at = gl.GetUniformLocation(v->gl.plain, "SHARP")) >= 0) gl.Uniform1f(at, 0.0f);
        gl.Enable(GL_BLEND);
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl.Disable(GL_BLEND);
    }

    if (v->shot_pending && !v->shot_raw) {
        uint32_t *px = malloc((size_t)out_w * (size_t)out_h * sizeof *px);
        if (px) {
            gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.ReadPixels(0, 0, out_w, out_h, GL_BGRA, GL_UNSIGNED_BYTE, px);
            save_pixels(v, out_w, out_h, px, out_w * 4, true);
            free(px);
        }
    }
    /* With the configuration window over the screen, the frame is finished
     * before it is shown: some drivers (Remote Desktop's among them) show a
     * frame still being drawn, for a refresh or two, as black. */
    if (v->overlay && gl.Finish) gl.Finish();
    SDL_GL_SwapWindow(v->window);
}

static bool renderer_init(mc_video *v) {
    Uint32 flags = SDL_RENDERER_ACCELERATED;
    if (v->vsync) flags |= SDL_RENDERER_PRESENTVSYNC;
    v->renderer = SDL_CreateRenderer(v->window, -1, flags);
    if (!v->renderer) v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_SOFTWARE);
    return v->renderer != NULL;
}

static void renderer_present(mc_video *v, const uint32_t *frame, int w, int h) {
    int out_w, out_h;
    SDL_GetRendererOutputSize(v->renderer, &out_w, &out_h);
    const SDL_Rect r = layout(v, out_w, out_h);
    if (!v->texture || w != v->tex_w || h != v->tex_h) {
        if (v->texture) SDL_DestroyTexture(v->texture);
        v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!v->texture) return;
        v->tex_w = w;
        v->tex_h = h;
        v->tex_filter = -1;
    }
    /* Sharp: the picture drawn nearest at the largest whole scale that fits,
     * then that scaled linearly over the rest (none needed at a whole scale;
     * below 1x, plain linear). */
    const int whole = r.w / w < r.h / h ? r.w / w : r.h / h;
    bool prescale = sharp(v, w) && whole >= 1 && (r.w != whole * w || r.h != whole * h) &&
                    SDL_RenderTargetSupported(v->renderer);
    if (prescale && (!v->prescaled || v->pre_w != whole * w || v->pre_h != whole * h)) {
        if (v->prescaled) SDL_DestroyTexture(v->prescaled);
        v->prescaled = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, whole * w,
                                         whole * h);
        v->pre_w = whole * w;
        v->pre_h = whole * h;
        if (v->prescaled) SDL_SetTextureScaleMode(v->prescaled, SDL_ScaleModeLinear);
        else prescale = false;
    }
    const int f = sharp(v, w) ? (whole < 1 || (!prescale && (r.w != whole * w || r.h != whole * h))) : smooth(v, w);
    if (f != v->tex_filter) {
        SDL_SetTextureScaleMode(v->texture, f ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        v->tex_filter = f;
    }
    SDL_UpdateTexture(v->texture, NULL, frame, w * (int)sizeof *frame);
    if (prescale) {
        SDL_SetRenderTarget(v->renderer, v->prescaled);
        SDL_RenderCopy(v->renderer, v->texture, NULL, NULL);
        SDL_SetRenderTarget(v->renderer, NULL);
    }
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    SDL_RenderCopy(v->renderer, prescale ? v->prescaled : v->texture, NULL, &r);
    if (v->overlay) {
        const int ow = v->overlay_w, oh = v->overlay_h;
        if (!v->overlay_tex || ow != v->overlay_tex_w || oh != v->overlay_tex_h) {
            if (v->overlay_tex) SDL_DestroyTexture(v->overlay_tex);
            v->overlay_tex = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
            v->overlay_tex_w = ow;
            v->overlay_tex_h = oh;
            if (v->overlay_tex) SDL_SetTextureBlendMode(v->overlay_tex, SDL_BLENDMODE_BLEND);
        }
        if (v->overlay_tex) {
            const SDL_Rect o = mc_video_overlay_rect(ow, oh, out_w, out_h);
            const int exact = o.w % ow == 0 && o.h % oh == 0 && o.w / ow == o.h / oh;
            SDL_SetTextureScaleMode(v->overlay_tex, exact ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
            SDL_UpdateTexture(v->overlay_tex, NULL, v->overlay, ow * (int)sizeof *v->overlay);
            SDL_RenderCopy(v->renderer, v->overlay_tex, NULL, &o);
        }
    }
    if (v->shot_pending && !v->shot_raw) {
        uint32_t *px = malloc((size_t)out_w * (size_t)out_h * sizeof *px);
        if (px && SDL_RenderReadPixels(v->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, px, out_w * 4) == 0) {
            save_pixels(v, out_w, out_h, px, out_w * 4, false);
        }
        free(px);
    }
    SDL_RenderPresent(v->renderer);
}

mc_video *mc_video_open(const char *title, const mc_settings *s, int screen_w, int screen_h, int aspect_w,
                        int aspect_h, bool hidden, char *err, size_t errlen) {
    mc_video *v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    v->screen_w = screen_w;
    v->screen_h = screen_h;
    v->shape_w = aspect_w;
    v->shape_h = aspect_h;
    v->aspect_w = s->aspect == MC_ASPECT_SQUARE ? 0 : aspect_w;
    v->aspect_h = aspect_h;
    v->scale = s->integer_scale;
    v->vsync = s->vsync;
    v->filter = s->filter;
    v->crt = s->crt;
    v->look = s->crt ? MC_VIDEO_LOOK_CRT : MC_VIDEO_LOOK_PLAIN;
    v->tex_filter = -1;

    int uw, uh;
    unit(v, &uw, &uh);
    const int scale = s->window_scale > 0 ? s->window_scale : 3;
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (hidden) flags |= SDL_WINDOW_HIDDEN;
    const bool fullscreen = !hidden && s->fullscreen;   /* set once the window is made (mc_video_set_fullscreen) */

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    v->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, uw * scale, uh * scale,
                                 flags | SDL_WINDOW_OPENGL);
    if (v->window && gl_init(v)) {
        if (fullscreen) mc_video_set_fullscreen(v, true);
        return v;
    }
    if (v->window) {
        gl_release(v);
        SDL_DestroyWindow(v->window);
    }
    /* No OpenGL 3.3: the SDL renderer, everything but the CRT shader. */
    v->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, uw * scale, uh * scale, flags);
    if (!v->window) {
        snprintf(err, errlen, "could not create a window: %s", SDL_GetError());
        free(v);
        return NULL;
    }
    if (!renderer_init(v)) {
        snprintf(err, errlen, "could not create a renderer: %s", SDL_GetError());
        SDL_DestroyWindow(v->window);
        free(v);
        return NULL;
    }
    if (fullscreen) mc_video_set_fullscreen(v, true);
    return v;
}

void mc_video_close(mc_video *v) {
    if (!v) return;
    gl_release(v);
    if (v->prescaled) SDL_DestroyTexture(v->prescaled);
    if (v->overlay_tex) SDL_DestroyTexture(v->overlay_tex);
    if (v->texture) SDL_DestroyTexture(v->texture);
    if (v->renderer) SDL_DestroyRenderer(v->renderer);
    if (v->window) SDL_DestroyWindow(v->window);
    free(v);
}

void mc_video_present(mc_video *v, const uint32_t *frame, int w, int h) {
    if (v->shot_pending && v->shot_raw) save_pixels(v, w, h, frame, w * 4, false);
    if (v->gl.ready) gl_present(v, frame, w, h);
    else renderer_present(v, frame, w, h);
}

/* Where the overlay falls in an output: the largest rectangle of its shape. */
SDL_Rect mc_video_overlay_rect(int w, int h, int out_w, int out_h) {
    SDL_Rect r = {0, 0, out_w, out_h};
    if (w <= 0 || h <= 0) return r;
    if ((long)out_w * h <= (long)out_h * w) r.h = (int)((long)out_w * h / w);
    else r.w = (int)((long)out_h * w / h);
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

void mc_video_set_overlay(mc_video *v, const uint32_t *argb, int w, int h) {
    v->overlay = argb && w > 0 && h > 0 ? argb : NULL;
    v->overlay_w = w;
    v->overlay_h = h;
}

void mc_video_set_geometry(mc_video *v, int w, int h) {
    if (w <= 0 || h <= 0) return;
    v->screen_w=w; v->screen_h=h;
    v->shape_w=w; v->shape_h=h;
    if (v->aspect_w) { v->aspect_w=w; v->aspect_h=h; }
    v->tex_filter=-1;
}

/* Full screen is a borderless window over the display it is on (windowed
 * full screen: other windows, notifications and Alt+Tab work as on the
 * desktop, no mode change). It is one pixel taller than the display: a
 * window that covers a display exactly is taken over by the graphics
 * driver as exclusive full screen when it presents through OpenGL. */
void mc_video_set_icon(mc_video *v, const uint32_t *argb, int size) {
    SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom((void *)(uintptr_t)argb, size, size, 32, size * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!icon) return;
    SDL_SetWindowIcon(v->window, icon);
    SDL_FreeSurface(icon);
}

void mc_video_set_fullscreen(mc_video *v, bool on) {
    if (on == v->fullscreen) return;
    if (on) {
        SDL_GetWindowPosition(v->window, &v->win_x, &v->win_y);
        SDL_GetWindowSize(v->window, &v->win_w, &v->win_h);
        int d = SDL_GetWindowDisplayIndex(v->window);
        SDL_Rect b;
        if (d < 0 || SDL_GetDisplayBounds(d, &b) != 0) return;
        SDL_SetWindowResizable(v->window, SDL_FALSE);
        SDL_SetWindowBordered(v->window, SDL_FALSE);
        SDL_SetWindowPosition(v->window, b.x, b.y);
        SDL_SetWindowSize(v->window, b.w, b.h + 1);
        SDL_RaiseWindow(v->window);
    } else {
        SDL_SetWindowBordered(v->window, SDL_TRUE);
        SDL_SetWindowResizable(v->window, SDL_TRUE);
        SDL_SetWindowSize(v->window, v->win_w, v->win_h);
        SDL_SetWindowPosition(v->window, v->win_x, v->win_y);
    }
    v->fullscreen = on;
}

bool mc_video_fullscreen(const mc_video *v) { return v->fullscreen; }

void mc_video_set_filter(mc_video *v, int filter) {
    v->filter = filter;
    v->tex_filter = -1;
}

void mc_video_set_integer_scale(mc_video *v, int scale) { v->scale = scale; v->tex_filter = -1; }

void mc_video_set_aspect(mc_video *v, int aspect) {
    v->aspect_w = aspect == MC_ASPECT_SQUARE ? 0 : v->shape_w;
    v->aspect_h = v->shape_h;
    v->tex_filter = -1;
}

void mc_video_set_vsync(mc_video *v, bool on) {
    v->vsync = on;
    if (v->gl.ready) SDL_GL_SetSwapInterval(on ? 1 : 0);
    else if (v->renderer) SDL_RenderSetVSync(v->renderer, on ? 1 : 0);
}

bool mc_video_set_crt(mc_video *v, bool on) {
    return mc_video_set_look(v, on ? MC_VIDEO_LOOK_CRT : MC_VIDEO_LOOK_PLAIN, MC_VIDEO_PHOSPHOR_COLOR);
}

bool mc_video_set_look(mc_video *v, int look, int phosphor) {
    const bool shader = look != MC_VIDEO_LOOK_PLAIN || phosphor != MC_VIDEO_PHOSPHOR_COLOR;
    if (shader && !mc_video_crt_available(v)) return false;
    v->crt = shader;
    v->look = look;
    v->phosphor = phosphor;
    v->tex_filter = -1;
    return true;
}

bool mc_video_crt_available(const mc_video *v) { return v->gl.ready && v->gl.crt; }

bool mc_video_to_screen(const mc_video *v, int wx, int wy, int *gx, int *gy) {
    /* Mouse events come in window points; the layout is computed in the
     * same units so a high-DPI drawable does not skew the mapping. */
    int ww, wh;
    SDL_GetWindowSize(v->window, &ww, &wh);
    const SDL_Rect r = layout(v, ww, wh);
    if (r.w <= 0 || r.h <= 0) return false;
    int x = (int)((long)(wx - r.x) * v->screen_w / r.w);
    int y = (int)((long)(wy - r.y) * v->screen_h / r.h);
    const bool inside = wx >= r.x && wy >= r.y && x < v->screen_w && y < v->screen_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= v->screen_w) x = v->screen_w - 1;
    if (y >= v->screen_h) y = v->screen_h - 1;
    *gx = x;
    *gy = y;
    return inside;
}

void mc_video_request_shot(mc_video *v, const char *path, bool raw) {
    snprintf(v->shot_path, sizeof v->shot_path, "%s", path);
    v->shot_pending = true;
    v->shot_raw = raw;
}

bool mc_video_take_shot_result(mc_video *v, char *path, size_t len) {
    if (!v->shot_result) return false;
    snprintf(path, len, "%s", v->shot_done);
    v->shot_result = false;
    return true;
}

SDL_Window *mc_video_window(mc_video *v) { return v->window; }
