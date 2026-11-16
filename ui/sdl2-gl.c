/*
 * QEMU SDL display driver -- opengl support
 *
 * Copyright (c) 2014 Red Hat
 *
 * Authors:
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"

static void sdl2_set_scanout_mode(struct sdl2_console *scon, bool scanout)
{
    if (scon->scanout_mode == scanout) {
        return;
    }

    scon->scanout_mode = scanout;
    if (!scon->scanout_mode) {
        egl_fb_destroy(&scon->guest_fb);
        if (scon->surface) {
            surface_gl_destroy_texture(scon->gls, scon->surface);
            surface_gl_create_texture(scon->gls, scon->surface);
        }
    }
}

static void sdl2_gl_render_surface(struct sdl2_console *scon)
{
    int ww, wh;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    surface_gl_setup_viewport(scon->gls, scon->surface, ww, wh);

    surface_gl_render_texture(scon->gls, scon->surface);
    SDL_GL_SwapWindow(scon->real_window);
}

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    if (!scon->real_window) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_update_texture(scon->gls, scon->surface, x, y, w, h);
    scon->updates++;
}

void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_destroy_texture(scon->gls, scon->surface);

    scon->surface = new_surface;

    if (surface_is_placeholder(new_surface) && qemu_console_get_index(dcl->con)) {
        qemu_gl_fini_shader(scon->gls);
        scon->gls = NULL;
        sdl2_window_destroy(scon);
        return;
    }

    if (!scon->real_window) {
        sdl2_window_create(scon);
        scon->gls = qemu_gl_init_shader();
    } else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(new_surface)) ||
                (surface_height(old_surface) != surface_height(new_surface)))) {
        sdl2_window_resize(scon);
    }

    surface_gl_create_texture(scon->gls, scon->surface);
}

void sdl2_gl_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    graphic_hw_update(dcl->con);
    if (scon->updates && scon->real_window) {
        scon->updates = 0;
        sdl2_gl_render_surface(scon);
    }
    sdl2_poll_events(scon);
}

void sdl2_gl_redraw(struct sdl2_console *scon)
{
    assert(scon->opengl);

    if (scon->scanout_mode) {
        /* sdl2_gl_scanout_flush actually only care about
         * the first argument. */
        return sdl2_gl_scanout_flush(&scon->dcl, 0, 0, 0, 0);
    }
    if (scon->surface) {
        sdl2_gl_render_surface(scon);
    }
}

QEMUGLContext sdl2_gl_create_context(DisplayGLCtx *dgc,
                                     QEMUGLParams *params)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext ctx, current_ctx;

    assert(scon->opengl);

    current_ctx = SDL_GL_GetCurrentContext();

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (scon->opts->gl == DISPLAY_GL_MODE_ON ||
        scon->opts->gl == DISPLAY_GL_MODE_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
    } else if (scon->opts->gl == DISPLAY_GL_MODE_ES) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, params->minor_ver);

    ctx = SDL_GL_CreateContext(scon->real_window);

    /* If SDL fail to create a GL context and we use the "on" flag,
     * then try to fallback to GLES.
     */
    if (!ctx && scon->opts->gl == DISPLAY_GL_MODE_ON) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        ctx = SDL_GL_CreateContext(scon->real_window);
    }

    SDL_GL_MakeCurrent(scon->real_window, current_ctx);

    return (QEMUGLContext)ctx;
}

void sdl2_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    SDL_GL_DeleteContext(sdlctx);
}

int sdl2_gl_make_context_current(DisplayGLCtx *dgc,
                                 QEMUGLContext ctx)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    assert(scon->opengl);

    return SDL_GL_MakeCurrent(scon->real_window, sdlctx);
}

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->w = 0;
    scon->h = 0;
    sdl2_set_scanout_mode(scon, false);
}

void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             void *d3d_tex2d)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->x = x;
    scon->y = y;
    scon->w = w;
    scon->h = h;
    scon->y0_top = backing_y_0_top;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    sdl2_set_scanout_mode(scon, true);
    egl_fb_setup_for_tex(&scon->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

/* Test-only GPU queries inspect the completed back buffer. Only aggregate
 * sample counts cross to the CPU; no image/texture pixel data is downloaded. */
static void lm_scanout_check(int width, int height, unsigned frame, GLuint source)
{
    if (g_strcmp0(getenv("LM_WIN32_SCANOUT_CHECK"), "1") != 0 ||
        (frame != 61 && frame != 97 && frame != 149 && frame != 180)) { return; }
    GLuint textures[2], framebuffer, vao, query, shaders[2], program;
    const char *vertex = "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fragment = "#version 330 core\nuniform sampler2D image;uniform int metric;uniform int height;out vec4 color;void main(){ivec2 p=ivec2(gl_FragCoord.xy);ivec3 c=ivec3(round(texelFetch(image,p,0).rgb*255.0));bool bar=all(equal(c,ivec3(22,29,46)));bool pink=all(equal(c,ivec3(255,159,203)));bool ok=metric==6?true:metric==0?any(greaterThan(c,ivec3(0))):metric==1?bar&&p.y>=height-40:metric==2?bar&&p.y<40:metric==3?pink:metric==4?all(equal(c,ivec3(255,74,100))):pink&&((p.x+3*p.y)%127<63);if(!ok)discard;color=vec4(1);}";
    glGenTextures(2, textures);
    for (unsigned i=0;i<2;i++) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,width,height);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    glGenFramebuffers(1,&framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,source ? framebuffer : 0);
    if(source) glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source,0);
    glReadBuffer(source ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBindTexture(GL_TEXTURE_2D,textures[0]);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[1],0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"LM_WIN32_CHECK_FAILED framebuffer\n");exit(1);}
    shaders[0]=glCreateShader(GL_VERTEX_SHADER);shaders[1]=glCreateShader(GL_FRAGMENT_SHADER);
    for(unsigned i=0;i<2;i++){
        const char *shader_source=i?fragment:vertex;GLint ok=0;
        glShaderSource(shaders[i],1,&shader_source,NULL);glCompileShader(shaders[i]);
        glGetShaderiv(shaders[i],GL_COMPILE_STATUS,&ok);
        if(!ok){char log[2048];glGetShaderInfoLog(shaders[i],sizeof(log),NULL,log);fprintf(stderr,"LM_WIN32_CHECK_FAILED shader %s\n",log);exit(1);}
    }
    program=glCreateProgram();glAttachShader(program,shaders[0]);glAttachShader(program,shaders[1]);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if(!linked){fprintf(stderr,"LM_WIN32_CHECK_FAILED link\n");exit(1);}
    glUseProgram(program);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[0]);
    glUniform1i(glGetUniformLocation(program,"image"),0);glUniform1i(glGetUniformLocation(program,"height"),height);
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenQueries(1,&query);
    glViewport(0,0,width,height);
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_RASTERIZER_DISCARD);
    GLuint counts[7]={0};
    for(int metric=0;metric<7;metric++){
        glUniform1i(glGetUniformLocation(program,"metric"),metric);
        glBeginQuery(GL_SAMPLES_PASSED,query);glDrawArrays(GL_TRIANGLES,0,3);glEndQuery(GL_SAMPLES_PASSED);
        glFlush();GLint ready=0;int64_t deadline=g_get_monotonic_time()+2000000;
        while(!ready&&g_get_monotonic_time()<deadline){glGetQueryObjectiv(query,GL_QUERY_RESULT_AVAILABLE,&ready);if(!ready)g_usleep(1000);}
        if(!ready){fprintf(stderr,"LM_WIN32_CHECK_FAILED query timeout\n");exit(1);}
        glGetQueryObjectuiv(query,GL_QUERY_RESULT,&counts[metric]);
    }
    GLenum error=glGetError();
    fprintf(stderr,"LM_WIN32_GPU_FRAME_CHECK source=%u frame=%u size=%dx%d nonblack=%u topbar=%u bottombar=%u pink=%u red=%u signature=%u control=%u gl_error=%x renderer=%s no_pixel_download\n",source,frame,width,height,counts[0],counts[1],counts[2],counts[3],counts[4],counts[5],counts[6],error,glGetString(GL_RENDERER));
    glUseProgram(0);glBindVertexArray(0);glBindTexture(GL_TEXTURE_2D,0);glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteQueries(1,&query);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(shaders[0]);glDeleteShader(shaders[1]);glDeleteFramebuffers(1,&framebuffer);glDeleteTextures(2,textures);
    if(error!=GL_NO_ERROR){fprintf(stderr,"LM_WIN32_CHECK_FAILED gl error\n");exit(1);}
}
void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    int ww, wh;

    assert(scon->opengl);
    if (!scon->scanout_mode) {
        return;
    }
    if (!scon->guest_fb.framebuffer) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    SDL_GL_SetSwapInterval(0);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    egl_fb_setup_default(&scon->win_fb, ww, wh, 0, 0);
    if (g_strcmp0(getenv("LM_WIN32_GPU_SCANOUT"), "1") == 0) {
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0,0);
    glBindTexture(GL_TEXTURE_2D,scon->guest_fb.texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    egl_texture_blit(scon->gls, &scon->win_fb, &scon->guest_fb, !scon->y0_top);
    } else {
        egl_fb_blit(&scon->win_fb, &scon->guest_fb, !scon->y0_top);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    static unsigned checked_frame = 0;
    ++checked_frame;
    lm_scanout_check(ww, wh, checked_frame, scon->guest_fb.texture);
    lm_scanout_check(ww, wh, checked_frame, 0);
    SDL_GL_SwapWindow(scon->real_window);

    /* Keep poll rate high while frames are being presented */
    scon->idle_counter = 0;
    scon->dcl.update_interval = SDL2_REFRESH_INTERVAL_BUSY;
}

#ifdef CONFIG_GBM
void sdl2_gl_scanout_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    const int *fds;

    assert(scon->opengl);
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    egl_dmabuf_import_texture(dmabuf);
    if (!qemu_dmabuf_get_texture(dmabuf)) {
        fds = qemu_dmabuf_get_fds(dmabuf, NULL);
        error_report("%s: failed fd=%d", __func__, fds ? fds[0] : -1);
        return;
    }

    sdl2_gl_scanout_texture(dcl, qemu_dmabuf_get_texture(dmabuf), false,
                            qemu_dmabuf_get_width(dmabuf),
                            qemu_dmabuf_get_height(dmabuf),
                            0, 0,
                            qemu_dmabuf_get_width(dmabuf),
                            qemu_dmabuf_get_height(dmabuf),
                            NULL);

    if (qemu_dmabuf_get_allow_fences(dmabuf)) {
        scon->guest_fb.dmabuf = dmabuf;
    }
}

void sdl2_gl_release_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf)
{
    egl_dmabuf_release_texture(dmabuf);
}

bool sdl2_gl_has_dmabuf(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    return scon->has_dmabuf;
}
#endif

void sdl2_gl_console_init(struct sdl2_console *scon)
{
    bool hidden = scon->hidden;

    scon->hidden = true;
    scon->surface = qemu_create_displaysurface(1, 1);
    sdl2_window_create(scon);

    /*
     * QEMU checks whether console supports dma-buf before switching
     * to the console.  To break this chicken-egg problem we pre-check
     * dma-buf availability beforehand using a dummy SDL window.
     */
    scon->has_dmabuf = qemu_egl_has_dmabuf();

    sdl2_window_destroy(scon);
    qemu_free_displaysurface(scon->surface);

    scon->surface = NULL;
    scon->hidden = hidden;
}
