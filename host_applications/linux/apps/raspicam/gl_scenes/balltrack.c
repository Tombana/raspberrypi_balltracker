/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Tim Gover
All rights reserved.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "balltrack.h"
#include "RaspiTex.h"
#include "RaspiTexUtil.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define BALLTRACK_VSHADER_SOURCE \
    "attribute vec2 vertex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "   texcoord = 0.5 * (vertex + 1.0);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n" \
    "}\n"

// Balltrack shader first phase
#define BALLTRACK_FSHADER_SOURCE_1 \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col = texture2D(tex, texcoord);\n" \
    "    gl_FragColor = col;\n" \
    "    //const vec4  kRGBToI     = vec4 (0.596, -0.275, -0.321, 0.0);\n" \
    "    //const vec4  kRGBToQ     = vec4 (0.212, -0.523, 0.311, 0.0);\n" \
    "    //float I = dot(col, kRGBToI);\n" \
    "    //float Q = dot(col, kRGBToQ);\n" \
    "    //float hue = atan(Q, I); // in range -pi,pi where 0,pi is the 0-180 part of Hue \n" \
    "    // We use a piecewise definition for Hue. Since we are only interested in a small\n" \
    "    // range, we only compute one of the three parts.\n" \
    "    // The `piece' we use here lies in [-1,1]. The other parts lie in [1,5].\n" \
    "    // Multiply by 60 to get degrees.\n" \
    "    float value = max(col.r, max(col.g, col.b));\n" \
    "    float chroma= value - min(col.r, min(col.g, col.b));\n" \
    "    float sat = (value > 0.0 ? (chroma / value) : 0.0); \n" \
    "    float hue = (col.r == value ? ((col.g - col.b) / chroma) : (1.0));\n" \
    "    // daytime test\n" \
    "    //if (hue > 0.15 && hue < 0.73 && sat > 0.30 && sat < 0.85 && value > 0.55 && value < 0.98 ) {\n" \
    "    // nighttime test\n" \
    "    if (hue > 0.05 && hue < 1.0 && sat > 0.10 && sat < 0.95 && value > 0.25 && value < 0.98 ) {\n" \
    "       gl_FragColor.a = 1.0;\n" \
    "    } else {\n" \
    "       gl_FragColor.a = 0.0;\n" \
    "    }\n" \
    "}\n"

// Balltrack shader second phase
// Ouput is 4X smaller in both directions!
// So we have to take care of half-integer texture coordinate stuff
// The center of the output pixel (=texcoord) is at the intersection
// of four input pixels.
//          0    .5     1
// Input:   |--|--|--|--|
// Output:  |-----*-----|
// where the star is the point that we get in texcoord.
#define BALLTRACK_FSHADER_SOURCE_2 \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    \n" \
    "    vec2 tshift = texcoord - 1.5 * tex_unit;\n" \
    "    float minVal = 1.0;\n" \
    "    for (int i = 0; i < 4; ++i) {\n" \
    "    for (int j = 0; j < 4; ++j) {\n" \
    "       vec4 p = texture2D(tex, tshift + vec2(i,j) * tex_unit);\n" \
    "       minVal = min(minVal, p.a);\n" \
    "    }\n" \
    "    }\n" \
    "    \n" \
    "    if (minVal > 0.5) {\n" \
    "        gl_FragColor.a = 1.0;\n" \
    "    } else {\n" \
    "        gl_FragColor.a = 0.0;\n" \
    "    }\n" \
    "}\n"

// Balltrack shader third phase
#define BALLTRACK_FSHADER_SOURCE_3 \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex_camera;\n" \
    "uniform sampler2D tex_filter;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    \n" \
    "    vec4 col = texture2D(tex_camera, texcoord);\n" \
    "    float maxVal = 0.0;\n" \
    "    //for (int i = -2; i <= 2; ++i) {\n" \
    "    //for (int j = -2; j <= 2; ++j) {\n" \
    "    //   vec4 p = texture2D(tex_filter, texcoord + vec2(i,j) * tex_unit);\n" \
    "    //   maxVal = max(maxVal, p.a);\n" \
    "    //}\n" \
    "    //}\n" \
    "    vec4 p = texture2D(tex_filter, texcoord + vec2(1,0) * tex_unit);\n" \
    "    maxVal = p.a;\n" \
    "    \n" \
    "    if (maxVal > 0.5) {\n" \
    "        gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n" \
    "    } else {\n" \
    "        gl_FragColor = col;\n" \
    "    }\n" \
    "}\n"

// Balltrack plain shader
#define BALLTRACK_FSHADER_SOURCE_PLAIN \
    "#extension GL_OES_EGL_image_external : require\n" \
    "//uniform samplerExternalOES tex;\n" \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col = texture2D(tex, texcoord);\n" \
    "    gl_FragColor = col;\n" \
    "}\n"


static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f,
   -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f,
};

static GLuint quad_vbo; // vertex buffer object
static GLuint fbo;      // frame buffer object for render-to-texture
static GLuint rtt_tex1; // Texture for render-to-texture
static GLuint rtt_tex2; // Texture for render-to-texture

static uint8_t* pixelbuffer; // For reading out result

// Divisions by 2 of 720p with correct aspect ratio
// 1280,720
//  640,360
//  320,180
//  160, 90
//   80, 45

// -- Source (1080p or 720p)
static int width0  = 1280;
static int height0 = 720;
// -- From source to phase 1: (maintain 720p aspect ratio)
static int width1  = 640;
static int height1 = 360;
// -- From phase 1 to phase 2: (maintain 720p aspect ratio)
static int width2  = 80;
static int height2 = 45;
// -- From phase 2 to screen
// state->width,state->height

static RASPITEXUTIL_SHADER_PROGRAM_T balltrack_shader_1 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_1,
    .uniform_names = {"tex"},
    .attribute_names = {"vertex"},
};

static RASPITEXUTIL_SHADER_PROGRAM_T balltrack_shader_2 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_2,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static RASPITEXUTIL_SHADER_PROGRAM_T balltrack_shader_3 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_3,
    .uniform_names = {"tex_camera", "tex_unit", "tex_filter"},
    .attribute_names = {"vertex"},
};

static RASPITEXUTIL_SHADER_PROGRAM_T balltrack_shader_plain =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_PLAIN,
    .uniform_names = {"tex"},
    .attribute_names = {"vertex"},
};


static const EGLint balltrack_egl_config_attribs[] =
{
   EGL_RED_SIZE,   8,
   EGL_GREEN_SIZE, 8,
   EGL_BLUE_SIZE,  8,
   EGL_ALPHA_SIZE, 8,
   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
   EGL_NONE
};


/**
 * Initialisation of shader uniforms.
 *
 * @param width Width of the EGL image.
 * @param width Height of the EGL image.
 */
static int shader_set_uniforms(RASPITEXUTIL_SHADER_PROGRAM_T *shader,
      int width, int height, int texunit, int extratex)
{
   GLCHK(glUseProgram(shader->program));
   GLCHK(glUniform1i(shader->uniform_locations[0], 0)); // Texture unit

   if (texunit) {
       /* Dimensions of a single pixel in texture co-ordinates */
       GLCHK(glUniform2f(shader->uniform_locations[1],
                   1.0 / (float) width, 1.0 / (float) height));
   }
   
   if (extratex) {
       GLCHK(glUniform1i(shader->uniform_locations[2], 1)); // Extra texture
   }

   /* Enable attrib 0 as vertex array */
   GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
   return 0;
}

static GLuint createFilterTexture(int w, int h) {
    GLuint id;
    GLCHK(glGenTextures(1, &id));
    glBindTexture(GL_TEXTURE_2D, id);
    //Scaling: nearest (=no) interpolation for scaling down and up.
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    //Wrapping: repeat. Only use (s,t) as we are using a 2D texture
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
    GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
    return id;
}

/**
 * Creates the OpenGL ES 2.X context and builds the shaders.
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int balltrack_init(RASPITEX_STATE *raspitex_state)
{
    // width,height is the size of the preview window
    int rc = 0;
    int width = raspitex_state->width;
    int height = raspitex_state->height;

    vcos_log_trace("%s", VCOS_FUNCTION);
    raspitex_state->egl_config_attribs = balltrack_egl_config_attribs;
    rc = raspitexutil_gl_init_2_0(raspitex_state);
    if (rc != 0)
        goto end;

    rc = raspitexutil_build_shader_program(&balltrack_shader_1);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_1, width0, height0, 0, 0);
    if (rc != 0)
        goto end;

    rc = raspitexutil_build_shader_program(&balltrack_shader_2);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_2, width1, height1, 1, 0);
    if (rc != 0)
        goto end;

    rc = raspitexutil_build_shader_program(&balltrack_shader_3);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_3, width2, height2, 1, 1);
    if (rc != 0)
        goto end;

    rc = raspitexutil_build_shader_program(&balltrack_shader_plain);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_plain, 0, 0, 0, 0);
    if (rc != 0)
        goto end;

    // Buffer to read out pixels from screen
    uint32_t buffer_size = width * height * 4;
    pixelbuffer = calloc(buffer_size, 1);
    if (!pixelbuffer) {
        rc = -1;
        goto end;
    }

    // Create frame buffer object for render-to-texture
    GLCHK(glGenFramebuffersOES(1, &fbo));
    GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo));
    GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0)); // unbind it

    rtt_tex1 = createFilterTexture(width1, height1);
    rtt_tex2 = createFilterTexture(width2, height2);

    GLCHK(glGenBuffers(1, &quad_vbo));
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));
    GLCHK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));

end:
    return rc;
}

// If target_tex is zero, then target is the screen
static int render_pass(RASPITEXUTIL_SHADER_PROGRAM_T* shader, GLuint source_type, GLuint source_tex, GLuint target_tex, int targetWidth, int targetHeight) {
    GLCHK(glUseProgram(shader->program));
    if (target_tex) {
        // Enable Render-to-texture and set the output texture
        GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo));
        GLCHK(glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, target_tex, 0));
        GLCHK(glViewport(0, 0, targetWidth, targetHeight));
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        // Unset frame buffer object. Now draw to screen
        GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0));
        GLCHK(glViewport(0, 0, targetWidth, targetHeight));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    // Bind the input texture
    GLCHK(glActiveTexture(GL_TEXTURE0));
    GLCHK(glBindTexture(source_type, source_tex));
    // Bind the vertex buffer
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
    GLCHK(glVertexAttribPointer(shader->attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
    // Draw
    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));
    return 0;
}

static int balltrack_readout(int width, int height)
{
    // Read texture
    if (pixelbuffer) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixelbuffer);
        if (glGetError() == GL_NO_ERROR) {
            // pixelbuffer[i*height + j] is i pixels from bottom and j from left
            uint32_t avgx = 0, avgy = 0;
            uint32_t count = 0;

            uint32_t* ptr = (uint32_t*)pixelbuffer;
            for (int i = 0; i < height; ++i) {
                for (int j = 0; j < width; ++j) {
                    // R,G,B,A = FF, 00, FF, FF
                    //if (*ptr++ == 0xffff00ff) {
                    if ( ((*ptr++) & 0xff000000) == 0xff000000) {
                        avgx += j;
                        avgy += i;
                        count++;
                    }
                }
            }
            if (count) {
                avgx /= count;
                avgy /= count;
                vcos_log_error("Average x,y is (%u, %u)!", avgx, avgy);
            }
        } else {
            vcos_log_error("glReadPixels failed!");
        }
    }
    return 0;
}


/* Redraws the scene with the latest luma buffer.
 *
 * @param raspitex_state A pointer to the GL preview state.
 * @return Zero if successful.
 */
static int balltrack_redraw(RASPITEX_STATE* state)
{
    int width = state->width;
    int height = state->height;
    // Width,height is the size of the preview window
    // The source image might be much larger

    // First pass: hue filter into small texture
    render_pass(&balltrack_shader_1, GL_TEXTURE_EXTERNAL_OES, state->texture, rtt_tex1, width1, height1);
    // Second pass: erode and downsample further
    render_pass(&balltrack_shader_2, GL_TEXTURE_2D, rtt_tex1, rtt_tex2, width2, height2);
    // Readout result
    balltrack_readout(width2, height2);
    // Third pass: dilate and render to screen
    //render_pass(&balltrack_shader_3, GL_TEXTURE_2D, rtt_tex2, 0, width, height);
    GLCHK(glActiveTexture(GL_TEXTURE1));
    GLCHK(glBindTexture(GL_TEXTURE_2D, rtt_tex2));
    render_pass(&balltrack_shader_3, GL_TEXTURE_EXTERNAL_OES, state->texture, 0, width, height);

    // Debug shader to draw a texture
    //render_pass(&balltrack_shader_plain, GL_TEXTURE_2D, rtt_tex1, 0, width, height);
    return 0;
}

int balltrack_open(RASPITEX_STATE *state)
{
   state->ops.gl_init = balltrack_init;
   state->ops.redraw = balltrack_redraw;
   //state->ops.update_y_texture = raspitexutil_update_y_texture;
   state->ops.update_texture = raspitexutil_update_texture;
   return 0;
}

