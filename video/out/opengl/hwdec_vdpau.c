/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <assert.h>

#include <GL/glx.h>

#include "hwdec.h"
#include "utils.h"
#include "video/vdpau.h"
#include "video/vdpau_mixer.h"

// This is a GL_NV_vdpau_interop specification bug, and headers (unfortunately)
// follow it. I'm not sure about the original nvidia headers.
#define BRAINDEATH(x) ((void *)(uintptr_t)(x))

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params);

struct priv {
    struct mp_log *log;
    struct mp_vdpau_ctx *ctx;
    uint64_t preemption_counter;
    struct mp_image_params image_params;
    GLuint gl_textures[4];
    bool vdpgl_initialized;
    GLvdpauSurfaceNV vdpgl_surface;
    VdpOutputSurface vdp_surface;
    GLvdpauSurfaceNV vdpgl_video_surface;
    struct mp_vdpau_mixer *mixer;
    bool mapped;
    struct gl_vao vao;
    struct gl_shader_cache *sc;
    struct fbotex fbos[2];
    GLenum target;
};

struct vertex {
    float position[2];
    float texcoord[2];
};

static const struct gl_vao_entry vertex_vao[] = {
    {"position",    2, GL_FLOAT,         false, offsetof(struct vertex, position)},
    {"texcoord" ,   2, GL_FLOAT,         false, offsetof(struct vertex, texcoord)},
    {0}
};

static void unmap(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    if (p->mapped) {
        //gl->VDPAUUnmapSurfacesNV(1, &p->vdpgl_surface);
        if (p->vdpgl_video_surface) {
            gl->VDPAUUnmapSurfacesNV(1, &p->vdpgl_video_surface);
            gl->VDPAUUnregisterSurfaceNV(p->vdpgl_video_surface);
            p->vdpgl_video_surface = 0;
        }
    }
    p->mapped = false;
}

static void mark_vdpau_objects_uninitialized(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;

    p->vdp_surface = VDP_INVALID_HANDLE;
    p->mapped = false;
}

static void destroy_objects(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    struct vdp_functions *vdp = &p->ctx->vdp;
    VdpStatus vdp_st;

    unmap(hw);

    if (p->vdpgl_surface)
        gl->VDPAUUnregisterSurfaceNV(p->vdpgl_surface);
    p->vdpgl_surface = 0;

    glDeleteTextures(4, p->gl_textures);
    memset(p->gl_textures, 0, sizeof(p->gl_textures));

    if (p->vdp_surface != VDP_INVALID_HANDLE) {
        vdp_st = vdp->output_surface_destroy(p->vdp_surface);
        CHECK_VDP_WARNING(p, "Error when calling vdp_output_surface_destroy");
    }
    p->vdp_surface = VDP_INVALID_HANDLE;

    gl_check_error(gl, hw->log, "Before uninitializing OpenGL interop");

    if (p->vdpgl_initialized)
        gl->VDPAUFiniNV();

    p->vdpgl_initialized = false;

    gl_check_error(gl, hw->log, "After uninitializing OpenGL interop");
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;

    destroy_objects(hw);
    mp_vdpau_mixer_destroy(p->mixer);
    if (p->ctx)
        hwdec_devices_remove(hw->devs, &p->ctx->hwctx);
    mp_vdpau_destroy(p->ctx);
}

static int create(struct gl_hwdec *hw)
{
    GL *gl = hw->gl;
    Display *x11disp = glXGetCurrentDisplay();
    if (!x11disp)
        return -1;
    if (!(gl->mpgl_caps & MPGL_CAP_VDPAU))
        return -1;
    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;
    p->log = hw->log;
    p->ctx = mp_vdpau_create_device_x11(hw->log, x11disp, true);
    if (!p->ctx)
        return -1;
    if (mp_vdpau_handle_preemption(p->ctx, &p->preemption_counter) < 1)
        return -1;
    p->vdp_surface = VDP_INVALID_HANDLE;
    p->mixer = mp_vdpau_mixer_create(p->ctx, hw->log);
    if (hw->probing && mp_vdpau_guess_if_emulated(p->ctx)) {
        destroy(hw);
        return -1;
    }
    p->sc = gl_sc_create(gl, hw->log);
    gl_vao_init(&p->vao, gl, sizeof(struct vertex), vertex_vao);
    gl_sc_set_vao(p->sc, &p->vao);
    p->ctx->hwctx.driver_name = hw->driver->name;
    hwdec_devices_add(hw->devs, &p->ctx->hwctx);
    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    struct vdp_functions *vdp = &p->ctx->vdp;
    VdpStatus vdp_st;

    destroy_objects(hw);

    assert(params->imgfmt == hw->driver->imgfmt);
    p->image_params = *params;

    if (mp_vdpau_handle_preemption(p->ctx, &p->preemption_counter) < 0)
        return -1;

    gl->VDPAUInitNV(BRAINDEATH(p->ctx->vdp_device), p->ctx->get_proc_address);

    p->vdpgl_initialized = true;

    vdp_st = vdp->output_surface_create(p->ctx->vdp_device,
                                        VDP_RGBA_FORMAT_B8G8R8A8,
                                        params->w, params->h, &p->vdp_surface);
    CHECK_VDP_ERROR(p, "Error when calling vdp_output_surface_create");

    p->target = GL_TEXTURE_RECTANGLE;

    gl->GenTextures(4, p->gl_textures);
    for (int n = 0; n < 4; n++) {
        gl->BindTexture(p->target, p->gl_textures[n]);
        gl->TexParameteri(p->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(p->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->TexParameteri(p->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(p->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl->BindTexture(p->target, 0);

    /*
    p->vdpgl_surface = gl->VDPAURegisterOutputSurfaceNV(BRAINDEATH(p->vdp_surface),
                                                        GL_TEXTURE_2D,
                                                        1, p->gl_textures);
    if (!p->vdpgl_surface)
        return -1;

    gl->VDPAUSurfaceAccessNV(p->vdpgl_surface, GL_READ_ONLY);
    */

    gl_check_error(gl, hw->log, "After initializing vdpau OpenGL interop");

    //params->imgfmt = IMGFMT_RGB0;
    params->imgfmt = IMGFMT_NV12;

    return 0;
}

static int map_frame(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    struct vdp_functions *vdp = &p->ctx->vdp;
    VdpStatus vdp_st;

    int pe = mp_vdpau_handle_preemption(p->ctx, &p->preemption_counter);
    if (pe < 1) {
        mark_vdpau_objects_uninitialized(hw);
        if (pe < 0)
            return -1;
        if (reinit(hw, &p->image_params) < 0)
            return -1;
    }

    /*
    if (!p->vdpgl_surface)
        return -1;

    mp_vdpau_mixer_render(p->mixer, NULL, p->vdp_surface, NULL, hw_image, NULL);

    gl->VDPAUMapSurfacesNV(1, &p->vdpgl_surface);
    */

    VdpVideoSurface surface = (intptr_t)hw_image->planes[3];

    VdpChromaType s_chroma_type;
    uint32_t s_w, s_h;

    vdp_st = vdp->video_surface_get_parameters(surface, &s_chroma_type, &s_w, &s_h);
    CHECK_VDP_ERROR(hw, "Error when calling vdp_video_surface_get_parameters");

    p->vdpgl_video_surface = gl->VDPAURegisterVideoSurfaceNV(BRAINDEATH(surface),
                                                             p->target,
                                                             4, p->gl_textures);
    if (!p->vdpgl_video_surface)
        return -1;

    gl->VDPAUSurfaceAccessNV(p->vdpgl_video_surface, GL_READ_ONLY);
    gl->VDPAUMapSurfacesNV(1, &p->vdpgl_video_surface);

    int sx[4] = {0, 1};
    int sy[4] = {0, 1};

    for (int plane = 0; plane < 2; plane++) {
        int d_w = s_w >> sx[plane];
        int d_h = s_h >> sy[plane];

        fbotex_change(&p->fbos[plane], gl, hw->log, d_w, d_h,
                      plane ? GL_RG8 : GL_R8, 0);

        for (int n = 0; n < 2; n++) {
            gl_sc_uniform_sampler(p->sc, n ? "t1" : "t0", p->target, n);
            gl->ActiveTexture(GL_TEXTURE0 + n);
            gl->BindTexture(p->target, p->gl_textures[plane * 2 + n]);
        }

        struct vertex va[4] = {0};

        for (int n = 0; n < 4; n++) {
            struct vertex *v = &va[n];
            v->position[0] = n / 2 * 2 - 1;
            v->position[1] = n % 2 * 2 - 1;
            v->texcoord[0] = (n / 2) * (s_w >> sx[plane]);
            v->texcoord[1] = (n % 2) * (s_h >> sy[plane]) / 2;
        }

        gl_sc_add(p->sc, "color = fract(gl_FragCoord.y / 2) < 0.5 ? texture(t0, texcoord) "
                         ": texture(t1, texcoord);");
        gl_sc_gen_shader_and_reset(p->sc);

        gl->BindFramebuffer(GL_FRAMEBUFFER, p->fbos[plane].fbo);

        gl->Viewport(0, 0, d_w, d_h);
        gl_vao_draw_data(&p->vao, GL_TRIANGLE_STRIP, va, 4);
    }

    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);

    for (int n = 0; n < 2; n++) {
        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(p->target, 0);
    }
    gl->ActiveTexture(GL_TEXTURE0);

    p->mapped = true;
    *out_frame = (struct gl_hwdec_frame){
        .interlaced = false,
    };
    for (int plane = 0; plane < 2; plane++) {
        out_frame->planes[plane] = (struct gl_hwdec_plane){
            .gl_texture = p->fbos[plane].texture,
            .gl_target = GL_TEXTURE_2D,
            .tex_w = s_w >> sx[plane],
            .tex_h = s_h >> sy[plane],
        };
    };
    return 0;
}

const struct gl_hwdec_driver gl_hwdec_vdpau = {
    .name = "vdpau-glx",
    .api = HWDEC_VDPAU,
    .imgfmt = IMGFMT_VDPAU,
    .create = create,
    .reinit = reinit,
    .map_frame = map_frame,
    .unmap = unmap,
    .destroy = destroy,
};
