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

#include "BalltrackCore.h"
#include <string.h>

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
// -- 2x2 pixels to 1 pixel
static int width1  = 640 / 2; // RGBA packs two pairs
static int height1 = 360;
// -- From phase 1 to phase 2: (maintain 720p aspect ratio)
// -- 8x8 pixels to 1 pixel
static int width2  = 80 / 2; // RGBA packs to pairs
static int height2 = 45;
// -- From phase 2 to screen
// state->width,state->height


#define BALLTRACK_VSHADER_SOURCE \
    "attribute vec2 vertex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "   texcoord = 0.5 * (vertex + 1.0);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n" \
    "}\n"
#define BALLTRACK_VSHADER_YFLIP_SOURCE \
    "attribute vec2 vertex;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "   texcoord.x = 0.5 * (1.0 + vertex.x);\n" \
    "   texcoord.y = 0.5 * (1.0 - vertex.y);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n" \
    "}\n"


// The output pixel coordinate (=texcoord) is always the center of an output pixel.

// Balltrack shader first phase
// To pack to pairs into an RGBA we have in width-direction:
// tex_unit:0  1  2  3  4
// Input:   |--|--|--|--|  each is size tex_unit
// texcoord:|-----*-----|
// samples: |--*--|--*--|
// Output:    RG    BA
// For the height direction it is simpler:
// tex_unit:0  1  2
// Input:   |--|--|
// texcoord:|--*--|
// samples: |--*--|

// Ball range:  Hue 
// Field range: Hue: 135-175
//                   2.25-2.75
//                   125 - 150
//                   2.1 - 2.5
char BALLTRACK_FSHADER_SOURCE_1[] =  \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex;\n" \
    "uniform vec2 tex_unit;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col1 = texture2D(tex, texcoord - vec2(1,0) * tex_unit);\n" \
    "    vec4 col2 = texture2D(tex, texcoord + vec2(1,0) * tex_unit);\n" \
    "    // We use a piecewise definition for Hue.\n" \
    "    // We only compute two of the three parts.\n" \
    "    // The `red piece' lies in [-1,1]. The green in [1,3]. The blue in [3,5].\n" \
    "    // Multiply by 60 to get degrees.\n" \
    "    float value1 = max(col1.r, max(col1.g, col1.b));\n" \
    "    float chroma1= value1 - min(col1.r, min(col1.g, col1.b));\n" \
    "    float sat1 = (value1 > 0.0 ? (chroma1 / value1) : 0.0); \n" \
    "    gl_FragColor.r = 0.0;\n" \
    "    if (col1.r == value1) {\n" \
    "        float hue = (col1.g - col1.b) / chroma1;\n" \
    "        if (hue > 0.20 && hue < 0.45 && sat1 > 0.40 && sat1 < 0.95 && value1 > 0.40 && value1 < 0.98 ) {\n" \
    "           gl_FragColor.r = 1.0;\n" \
    "        }\n" \
    "    }\n" \
    "    gl_FragColor.g = 0.0;\n" \
    "    if (col1.g == value1) {\n" \
    "        float hue = (col1.b - col1.r) / chroma1;\n" \
    "        if (hue > 0.0 && hue < 0.6 && sat1 > 0.15 && sat1 < 0.75 && value1 > 0.15 && value1 < 0.75 ) {\n" \
    "           gl_FragColor.g = 1.0;\n" \
    "        }\n" \
    "    }\n" \
    "    \n" \
    "    float value2 = max(col2.r, max(col2.g, col2.b));\n" \
    "    float chroma2= value2 - min(col2.r, min(col2.g, col2.b));\n" \
    "    float sat2 = (value2 > 0.0 ? (chroma2 / value2) : 0.0); \n" \
    "    gl_FragColor.b = 0.0;\n" \
    "    if (col2.r == value2) {\n" \
    "        float hue = (col2.g - col2.b) / chroma2;\n" \
    "        if (hue > 0.20 && hue < 0.45 && sat2 > 0.40 && sat2 < 0.95 && value2 > 0.40 && value2 < 0.98 ) {\n" \
    "           gl_FragColor.b = 1.0;\n" \
    "        }\n" \
    "    }\n" \
    "    gl_FragColor.a = 0.0;\n" \
    "    if (col2.g == value2) {\n" \
    "        float hue = (col2.b - col2.r) / chroma2;\n" \
    "        if (hue > 0.0 && hue < 0.6 && sat2 > 0.15 && sat2 < 0.75 && value2 > 0.15 && value2 < 0.75 ) {\n" \
    "           gl_FragColor.a = 1.0;\n" \
    "        }\n" \
    "    }\n" \
    "    \n" \
    "    // daytime test\n" \
    "    //if (hue > 0.15 && hue < 0.73 && sat > 0.30 && sat < 0.85 && value > 0.55 && value < 0.98 ) {\n" \
    "    // nighttime test\n" \
    "    //if (hue > 0.05 && hue < 1.0 && sat > 0.10 && sat < 0.95 && value > 0.25 && value < 0.98 ) {\n" \
    "}\n";

// Balltrack shader second phase
// Ouput is 8X smaller in both directions!
// We will use GL_LINEAR so that the GPU samples 4 texels at once.
// The center of the output pixel (=texcoord) is at the intersection
// of four input pixels.
// tex_unit is size of input texel
// In the height dimension, where we have one output:
// tex_unit:0  1  2  3  4  5  6  7  8
// Input:   |--|--|--|--|--|--|--|--|
// texcoord:|-----------*-----------|
// samples: |--*--|--*--|--*--|--*--|
//
// In the width dimension, where we have two *outputs*:
// tex_unit:0     1     2     3     4....8
// Input:   |RG|BA|RG|BA|RG|BA|RG|BA|....|
// texcoord:|-----------------------*....|
// samples: |-----*-----|-----*-----|....|
// Output:  |          RG           | BA |
//
// And now just as in phase 1, we want to do the above thing twice,
// store one result in RG and the other in BA
#define BALLTRACK_FSHADER_SOURCE_2 \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    vec2 tbase = texcoord - 3.0 * tex_unit;\n" \
    "    vec2 tdif  = 2.0 * tex_unit;\n" \
    "    vec4 avg1 = vec4(0,0,0,0);\n" \
    "    vec4 avg2 = vec4(0,0,0,0);\n" \
    "    for (int i = 0; i < 2; ++i) {\n" \
    "    for (int j = 0; j < 4; ++j) {\n" \
    "       avg1 += texture2D(tex, tbase + vec2(i,j) * tdif);\n" \
    "    }\n" \
    "    }\n" \
    "    for (int i = 2; i < 4; ++i) {\n" \
    "    for (int j = 0; j < 4; ++j) {\n" \
    "       avg2 += texture2D(tex, tbase + vec2(i,j) * tdif);\n" \
    "    }\n" \
    "    }\n" \
    "    gl_FragColor.rg = (1.0/16.0) * (avg1.rg + avg1.ba);\n" \
    "    gl_FragColor.ba = (1.0/16.0) * (avg2.rg + avg2.ba);\n" \
    "}\n"

// Balltrack shader third phase
char BALLTRACK_FSHADER_SOURCE_3[] =  \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex_camera;\n" \
    "uniform vec2 tex_unit;\n" \
    "uniform sampler2D tex_filter;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col = texture2D(tex_camera, texcoord);\n" \
    "    vec4 fil = texture2D(tex_filter, texcoord);\n" \
    "    // fil.rg is red/green filter for `left pixel'\n" \
    "    // fil.ba is red/green filter for `right pixel'\n" \
    "    float r, g;\n" \
    "    if ( mod(texcoord.x, tex_unit.x) > 0.5 * tex_unit.x ) {\n" \
    "        r = fil.r;\n" \
    "        g = fil.g;\n" \
    "    } else {\n" \
    "        r = fil.b;\n" \
    "        g = fil.a;\n" \
    "    }\n" \
    "    if (r > (172.0 / 256.0)) {\n" \
    "        gl_FragColor = 0.3 * col + 0.7 * vec4(1.0, 0.0, 1.0, 1.0);\n" \
    "    } else if (g > 0.75) {\n" \
    "        gl_FragColor = 0.8 * col + 0.2 * vec4(0.0, 1.0, 0.0, 1.0);\n" \
    "    } else {\n" \
    "        gl_FragColor = col;\n" \
    "    }\n" \
    "}\n";


// Balltrack plain shader
#define BALLTRACK_FSHADER_SOURCE_PLAIN \
    "uniform vec4 col;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    gl_FragColor = col;\n" \
    "}\n"

static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
   -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f,
};

static GLuint quad_vbo; // vertex buffer object
static GLuint fbo;      // frame buffer object for render-to-texture
static GLuint rtt_tex1; // Texture for render-to-texture
static GLuint rtt_tex2; // Texture for render-to-texture

static uint8_t* pixelbuffer; // For reading out result

static SHADER_PROGRAM_T balltrack_shader_1 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_1,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_2 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_2,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_3 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_3,
    .uniform_names = {"tex_camera", "tex_unit", "tex_filter"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_plain =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_PLAIN,
    .uniform_names = {"col"},
    .attribute_names = {"vertex"},
};


/**
 * Initialisation of shader uniforms.
 *
 * @param width Width of the EGL image.
 * @param width Height of the EGL image.
 */
static int shader_set_uniforms(SHADER_PROGRAM_T *shader,
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

static GLuint createFilterTexture(int w, int h, GLint scaling) {
    GLuint id;
    GLCHK(glGenTextures(1, &id));
    glBindTexture(GL_TEXTURE_2D, id);
    //Scaling: GL_NEAREST no interpolation for scaling down and up.
    //Scaling: GL_LINEAR  interpolate between source pixels
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, scaling));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, scaling));
    //Wrapping: repeat. Only use (s,t) as we are using a 2D texture
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
    GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
    return id;
}

int balltrack_core_init(int externalSamplerExtension, int flipY)
{
    int rc = 0;

    if (externalSamplerExtension == 0) {
        // Replace
        // "samplerExternalOES"
        // "sampler2D         "
        char* pos = 0;
        if ((pos = strstr(BALLTRACK_FSHADER_SOURCE_1, "samplerExternalOES"))){
            memcpy(pos, "sampler2D         ", 18);
        }
        if ((pos = strstr(BALLTRACK_FSHADER_SOURCE_3, "samplerExternalOES"))){
            memcpy(pos, "sampler2D         ", 18);
        }
    }

    // Camera source is Y-flipped.
    // So flip it back in the vertex shader
    if (flipY) {
        // TODO:
        // Do not flip the render-to-texture textures ??
        //balltrack_shader_1.vertex_source = BALLTRACK_VSHADER_YFLIP_SOURCE;
        //balltrack_shader_2.vertex_source = BALLTRACK_VSHADER_YFLIP_SOURCE;
        //balltrack_shader_3.vertex_source = BALLTRACK_VSHADER_YFLIP_SOURCE;
    }

    printf("Building shader 1\n");
    rc = balltrack_build_shader_program(&balltrack_shader_1);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_1, width0, height0, 1, 0);
    if (rc != 0)
        goto end;

    printf("Building shader 2\n");
    rc = balltrack_build_shader_program(&balltrack_shader_2);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_2, width1, height1, 1, 0);
    if (rc != 0)
        goto end;

    printf("Building shader 3\n");
    rc = balltrack_build_shader_program(&balltrack_shader_3);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_3, width2, height2, 1, 1);
    if (rc != 0)
        goto end;

    printf("Building shader 4\n");
    rc = balltrack_build_shader_program(&balltrack_shader_plain);
    if (rc != 0)
        goto end;

    // Buffer to read out pixels from last texture
    uint32_t buffer_size = width2 * height2 * 4;
    pixelbuffer = calloc(buffer_size, 1);
    if (!pixelbuffer) {
        rc = -1;
        goto end;
    }

    printf("Generating framebuffer object\n");
    // Create frame buffer object for render-to-texture
    GLCHK(glGenFramebuffersOES(1, &fbo));
    GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo));
    GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0)); // unbind it

    printf("Creating render-to-texture targets\n");
    rtt_tex1 = createFilterTexture(width1, height1, GL_LINEAR);
    rtt_tex2 = createFilterTexture(width2, height2, GL_NEAREST);

    printf("Creating vertex-buffer object\n");
    GLCHK(glGenBuffers(1, &quad_vbo));
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));
    //GLCHK(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));
    GLCHK(glClearColor(0.15f, 0.25f, 0.35f, 1.0f));

    GLCHK(glDisable(GL_BLEND));
end:
    return rc;
}

// x,y are coordinates in [-1,1]x[-1,1] range
void draw_line_strip(GLfloat* xys, int count, uint32_t color) {
    float r, g, b, a;
    r = (1.0/255.0) * ((color      ) & 0xff);
    g = (1.0/255.0) * ((color >>  8) & 0xff);
    b = (1.0/255.0) * ((color >> 16) & 0xff);
    a = (1.0/255.0) * ((color >> 24) & 0xff);

    SHADER_PROGRAM_T* shader = &balltrack_shader_plain;
    GLCHK(glUseProgram(shader->program));
    GLCHK(glUniform4f(shader->uniform_locations[0], r, g, b, a));

    // Unbind the vertex buffer --> use client memory
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, 0));
    GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
    GLCHK(glVertexAttribPointer(shader->attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, xys));
    // Draw
    GLCHK(glDrawArrays(GL_LINE_STRIP, 0, count));
}

// x,y are coordinates in [-1,1]x[-1,1] range
void draw_square(float xmin, float xmax, float ymin, float ymax, uint32_t color) {
    // Draw a square
    GLfloat vertexBuffer[5][2];
    vertexBuffer[0][0]= xmin;
    vertexBuffer[0][1]= ymin;
    vertexBuffer[1][0]= xmax;
    vertexBuffer[1][1]= ymin;
    vertexBuffer[2][0]= xmax;
    vertexBuffer[2][1]= ymax;
    vertexBuffer[3][0]= xmin;
    vertexBuffer[3][1]= ymax;
    vertexBuffer[4][0]= xmin;
    vertexBuffer[4][1]= ymin;
    draw_line_strip(vertexBuffer, 5, color);
}

// If target_tex is zero, then target is the screen
static int render_pass(SHADER_PROGRAM_T* shader, GLuint source_type, GLuint source_tex, GLuint target_tex, int targetWidth, int targetHeight) {
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

// All [-1,1]x[-1,1] coordinates
static int historyCount = 10;
GLfloat ballXYs[60][2];
static int ballCur = 0;
float greenxmin, greenxmax, greenymin, greenymax;
static int ballGone = 0;

static int balltrack_readout(int width, int height) {
    // Read texture
    // It packs two pixels into one:
    // RGBA is red,green,red,green filter values for neighbouring pixels
    if (pixelbuffer) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixelbuffer);
        if (glGetError() == GL_NO_ERROR) {
            // pixelbuffer[i*height + j] is i pixels from bottom and j from left
            uint32_t avgx = 0, avgy = 0;
            uint32_t count = 0;
            uint32_t gxmin = (uint32_t)(0.45f * 2.0f * width);
            uint32_t gxmax = (uint32_t)(0.55f * 2.0f * width);
            uint32_t gymin = (uint32_t)(0.45f * height);
            uint32_t gymax = (uint32_t)(0.55f * height);

            uint32_t* ptr = (uint32_t*)pixelbuffer;
            for (int i = 0; i < height; ++i) {
                for (int j = 0; j < width; ++j) {
                    // R,G,B,A = FF, 00, FF, FF
                    uint32_t rgba = *ptr++;
                    //int R1 = (rgba      ) & 0xff;
                    int G1 = (rgba >>  8) & 0xff;
                    //int R2 = (rgba >> 16) & 0xff;
                    int G2 = (rgba >> 24) & 0xff;

                    int y = i;
                    int x1 = 2*j;
                    int x2 = 2*j + 1;
                    // This part could be optimized...
                    // - y min/max check only once
                    // - if x1<gxmin then x2 does not need to be checked
                    // - ...
                    if (G1 > 128) {
                        if (x1 < gxmin) gxmin = x1;
                        if (x1 > gxmax) gxmax = x1;
                        if (y < gymin) gymin = y;
                        if (y > gymax) gymax = y;
                    }
                    if (G2 > 128) {
                        if (x2 < gxmin) gxmin = x2;
                        if (x2 > gxmax) gxmax = x2;
                        if (y < gymin) gymin = y;
                        if (y > gymax) gymax = y;
                    }
                }
            }
            ptr = (uint32_t*)pixelbuffer;
            for (int i = 0; i < height; ++i) {
                for (int j = 0; j < width; ++j) {
                    // R,G,B,A = FF, 00, FF, FF
                    uint32_t rgba = *ptr++;
                    int R1 = (rgba      ) & 0xff;
                    //int G1 = (rgba >>  8) & 0xff;
                    int R2 = (rgba >> 16) & 0xff;
                    //int G2 = (rgba >> 24) & 0xff;

                    int y = i;
                    int x1 = 2*j;
                    int x2 = 2*j + 1;
                    if ( y < gymin || y > gymax ) continue;
                    if ( x1 < gxmin || x2 > gxmax ) continue;
                    if ( R1 >= 128 ) {
                        avgx += x1;
                        avgy += y;
                        count++;
                    }
                    if ( R2 >= 128 ) {
                        avgx += x2;
                        avgy += y;
                        count++;
                    }
                }
            }
            // Map to [-1,1]
            greenxmin = gxmin / ((float)width) - 1.0f;
            greenxmax = gxmax / ((float)width) - 1.0f;
            greenymin = (2.0f * gymin) / ((float)height) - 1.0f;
            greenymax = (2.0f * gymax) / ((float)height) - 1.0f;
            if (count) {
		ballGone = 0;
                avgx /= count;
                avgy /= count;
                ballXYs[ballCur][0] = ((float)avgx) / ((float)width) - 1.0f;
                ballXYs[ballCur][1] = (2.0f * ((float)avgy)) / ((float)height) - 1.0f;
                ++ballCur;
                if(ballCur >= historyCount)
                    ballCur = 0;
                //printf("Average x,y is (%u, %u)!", avgx, avgy);
                return 1;
            } else {
		if (ballGone++ == 30) {
			printf("Ball has gone for 30 frames!\n");
			int i = ballCur - 1;
			if (i < 0) i = historyCount - 1;
			if (ballXYs[i][0] < greenxmin + (greenxmax - greenxmin) / 3.0f) {
				printf("Goal for red!\n");
			}
			if (ballXYs[i][0] > greenxmin + 2.0f * (greenxmax - greenxmin) / 3.0f) {
				printf("Goal for blue!\n");
			}
		}	
	    }
        } else {
            printf("glReadPixels failed!");
        }
    }
    return 0;
}

// Same but called from video player version
int balltrack_core_redraw(int width, int height, GLuint srctex, GLuint srctype)
{
    // Width,height is the size of the preview window
    // The source image might be much larger

#if 0
    GLCHK(glViewport(0, 0, width, height));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // Bind the input texture
    GLCHK(glUseProgram(balltrack_shader_1.program));
    GLCHK(glActiveTexture(GL_TEXTURE0));
    GLCHK(glBindTexture(srctype, srctex));
    // Bind the vertex buffer
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glEnableVertexAttribArray(balltrack_shader_1.attribute_locations[0]));
    GLCHK(glVertexAttribPointer(balltrack_shader_1.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
    // Draw
    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));
#endif

#if 1
    // First pass: hue filter into small texture
    render_pass(&balltrack_shader_1, srctype, srctex, rtt_tex1, width1, height1);
    // Second pass: erode and downsample further
    render_pass(&balltrack_shader_2, GL_TEXTURE_2D, rtt_tex1, rtt_tex2, width2, height2);
    // Readout result
    balltrack_readout(width2, height2);
    // Third pass: dilate and render to screen
    //render_pass(&balltrack_shader_3, GL_TEXTURE_2D, rtt_tex2, 0, width, height);
    GLCHK(glActiveTexture(GL_TEXTURE1));
    GLCHK(glBindTexture(GL_TEXTURE_2D, rtt_tex2));
    render_pass(&balltrack_shader_3, srctype, srctex, 0, width, height);
#endif

#if 1
    draw_square(greenxmin, greenxmax, greenymin, greenymax, 0xff00ff00);
#endif

#if 1
    draw_line_strip(ballXYs, historyCount, 0xffff0000);

    for (int i = 0; i < historyCount; ++i) {
        int time = (ballCur - i + historyCount) % historyCount;
        int blue = 0xff - time;
        // The bytes are R,G,B,A but little-endian so 0xAABBGGRR
        int color = 0xff000000 | (blue << 16);
        float size = 0.002f * time;
        draw_square(ballXYs[i][0] - 0.5f * size, ballXYs[i][0] + 0.5f * size, ballXYs[i][1] - size, ballXYs[i][1] + size, color);
    }
#endif

    return 0;
}

