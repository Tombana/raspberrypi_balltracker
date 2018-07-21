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

// For writing to the FIFO python thing
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// Divisions by 2 of 720p with correct aspect ratio
// 1280,720
//  640,360
//  320,180
//  160, 90
//   80, 45

// Every step maintains 720p aspect ratio
// -- Source is 720p
static int width0  = 1280;
static int height0 = 720;
// -- From source to phase 1: 2x2 sampler-average, then hue filter
// -- 2x2 pixels to 1 pixel
static int width1  = 640 / 2; // RGBA packs two pairs
static int height1 = 360;
// -- From phase 1 to phase 2: dilate 'red' filter, do not erode 'orange filter' but rescale?
// -- 4x4 pixels to 1 pixel (but sample 12x12 pixels to 1 pixel)
static int width2  = 160 / 2; // RGBA packs to pairs
static int height2 = 90;
// -- From phase 2 to phase 3: average for easier cpu handling ???
// -- 2x2 pixels to 1 pixel
static int width3  = 80 / 2; // RGBA packs to pairs
static int height3 = 45;
// -- From phase 3 to screen
// screenwidth,screenheight


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

// Ball hue lower bound is important to distinguish from red players
// Ball sat lower bound is important to distinguish it from pure white
//
// These (Hue,Saturation,Value) tuples are all in range [0-360]x[0,100]x[0,100]
// This is all from video. Not always the same as from camera.
// Normal ball:             (16-20, 50-70, 70-80)
// Fast blur ball in light: (?-60 , 15+  , 50+)
// Ball in goal:            (17-30, 60-80, 30-50)
// Ball in goal dark:       (16-24, 75-95, 14-21) low Value!
// Ball on white corner:    ( 9-12, 65-85, 40-55)
// Red player edges:        (?-15 , 50-70, 35-54)
//                          (47   , 42   , 31)
// Red player spinning:     ( 25  , 50   , 50)   :(
//
// For the replay video, seperating the Hue as  0.18 < neutral < 0.25 is fine.
// For the camera, the bound has to be much lower. Like  0.06 < neutral < 0.14
//
// Field: (125-175, 15-75, 13-70)
// Rescaling table
// Hue [0-360] : 4     6    7     8     9     10    11    12   14    15    16    17    18
// Hue [0-6]   : 0.067 0.10 0.117 0.133 0.150 0.167 0.183 0.20 0.233 0.250 0.267 0.283 0.30
char BALLTRACK_FSHADER_SOURCE_1[] =  \
    "#extension GL_OES_EGL_image_external : require\n" \
    "\n" \
    "vec2 getFilter(vec4 col) {\n" \
    "    // We use a piecewise definition for Hue.\n" \
    "    // We only compute two of the three parts.\n" \
    "    // The `red piece' lies in [-1,1]. The green in [1,3]. The blue in [3,5].\n" \
    "    // Multiply by 60 to get degrees.\n" \
    "    float value = max(col.r, max(col.g, col.b));\n" \
    "    float chroma= value - min(col.r, min(col.g, col.b));\n" \
    "    float sat = (value > 0.0 ? (chroma / value) : 0.0); \n" \
    "    float redfilter = 0.8;\n" \
    "    float greenfilter = 0.0;\n" \
    "    if (col.r == value) {\n" \
    "        if (sat > 0.30 && value > 0.30 && value < 0.99 ) {\n" \
    "            float hue = (col.g - col.b) / chroma;\n" \
    "            // Hue upper bound of 1.0 is automatic.\n" \
    "            if (hue > 0.14) {\n" \
    "                redfilter = 1.0;\n" \
    "            } else if (hue < 0.06) {\n" \
    "                redfilter = 0.0;\n" \
    "            }\n" \
    "        }\n" \
    "    } else if (col.g == value) {\n" \
    "        float hue = (col.b - col.r) / chroma;\n" \
    "        if (hue > 0.0 && hue < 0.9 && sat > 0.15 && value > 0.15 && value < 0.75 ) {\n" \
    "           greenfilter = 1.0;\n" \
    "        }\n" \
    "    }\n" \
    "    return vec2(redfilter, greenfilter);\n" \
    "}\n" \
    "\n" \
    "uniform samplerExternalOES tex;\n" \
    "uniform vec2 tex_unit;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col1 = texture2D(tex, texcoord - vec2(1,0) * tex_unit);\n" \
    "    vec4 col2 = texture2D(tex, texcoord + vec2(1,0) * tex_unit);\n" \
    "    gl_FragColor.rg = getFilter(col1);\n" \
    "    gl_FragColor.ba = getFilter(col2);\n" \
    "}\n";

// Balltrack shader second phase
// Ouput is 8X smaller in both directions!
// We will use GL_LINEAR so that the GPU samples 4 texels at once.
// The center of the output pixel (=texcoord) is at the intersection
// of four input pixels.
// tex_unit is size of input texel
// NOTE: Trying to sample 16x16 pixels yields an out-of-memory error! 12x12 still works!
// In the height dimension, where we have one output:
// tex_unit:             0  1  2  3  4  5  6  7  8
//          -8 -7 -6 -5 -4 -3 -2 -1  0  1  2  3  4  5  6  7  8
// Input:    |--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|
// texcoord:             |-----------*-----------|
// samples1:             |--*--|--*--|--*--|--*--|
// samples2:       |--*--|--*--|--*--|--*--|--*--|--*--|
//
// In the width dimension, where we have two *outputs*:
// tex_unit:             0     1     2     3     4     5     6     7     8
//          -6    -5    -4    -3    -2    -1     0     1     2     3     4     5     6
// Input:    |RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|
// texcoord:             |-----------------------*-----------------------|
// samples1:             |-----*-----|-----*-----|-----*-----|-----*-----|
// samples2:       |-----*-----|-----*-----|-----*-----|-----*-----|-----*-----|
// Output:         |<--             R G             -->|
// Output:                                 |<--             B A             -->|
// One of the x-sample points is used in both results!
// OOM:      |-----*-----|-----*-----|-----*-----|-----*-----|-----*-----|-----*-----| (out-of-memory)
char BALLTRACK_FSHADER_SOURCE_2[] =  \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    vec2 tbase = texcoord - vec2(4.0,5.0) * tex_unit;\n" \
    "    vec2 tdif  = 2.0 * tex_unit;\n" \
    "    vec4 avg1   = vec4(0,0,0,0);\n" \
    "    vec4 avg2   = vec4(0,0,0,0);\n" \
    "    vec4 avgboth= vec4(0,0,0,0);\n" \
    "    for (int j = 0; j < 6; ++j) {\n" \
    "       avgboth += texture2D(tex, tbase + vec2(2,j) * tdif);\n" \
    "    }\n" \
    "    for (int i = 0; i < 2; ++i) {\n" \
    "    for (int j = 0; j < 6; ++j) {\n" \
    "       avg1 += texture2D(tex, tbase + vec2(i,j) * tdif);\n" \
    "    }\n" \
    "    }\n" \
    "    for (int i = 3; i < 5; ++i) {\n" \
    "    for (int j = 0; j < 6; ++j) {\n" \
    "       avg2 += texture2D(tex, tbase + vec2(i,j) * tdif);\n" \
    "    }\n" \
    "    }\n" \
    "    avg1 += avgboth;\n" \
    "    avg2 += avgboth;\n" \
    "    gl_FragColor.rg = (1.0/36.0) * (avg1.rg + avg1.ba);\n" \
    "    gl_FragColor.ba = (1.0/36.0) * (avg2.rg + avg2.ba);\n" \
    "}\n";

// Balltrack shader new second phase
// Ouput is 4X smaller in both directions
// We will use GL_LINEAR so that the GPU samples 4 texels at once.
// The center of the output pixel (=texcoord) is at the intersection
// of four input pixels.
// tex_unit is size of input texel
// In the height dimension, where we have one output:
// tex_unit:-8 -7 -6 -5 -4 -3 -2 -1  0  1  2  3  4  5  6  7  8
// Input:    |--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|
// texcoord:                   |-----*-----|
// samples:        |--*--|--*--|--*--|--*--|--*--|--*--|
//
// unused:
// samples:                 |--*--|--*--|--*--|
// samples:              |--*--|--*--|--*--|--*--|
// samples:           |--*--|--*--|--*--|--*--|--*--|
//
// In the width dimension, where we have two *outputs*:
// tex_unit:-6    -5    -4    -3    -2    -1     0     1     2     3     4     5     6
// Input:    |RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|
// texcoord:                         |----R-G----*----B-A----|
// Output:               |<--             R G             -->|
// Output:                           |<--             B A             -->|
// samples:              |-----*-----|-----*-----|-----*-----|-----*-----|
// 
// unused
// samples:                          |-----*-----|-----*-----|
// samples:                    |-----*-----|-----*-----|-----*-----|
// samples:        |-----*-----|-----*-----|-----*-----|-----*-----|-----*-----|
char BALLTRACK_FSHADER_SOURCE_2_NEW[] =  \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    bool foundRed = false;\n" \
    "    vec4 avg1 = vec4(0,0,0,0);\n" \
    "    vec4 avg2 = vec4(0,0,0,0);\n" \
    "    for (int i = -3; i <= 3; i += 2) {\n" \
    "    for (int j = -5; j <= 5; j += 2) {\n" \
    "       vec4 col = texture2D(tex, texcoord + vec2(i,j) * tex_unit);\n" \
    "       if (min(col.r,col.b) < 0.78) {\n" \
    "           foundRed = true;\n" \
    "       }\n" \
    "       // I *really* hope it unrolls this loop\n" \
    "       if (j == -1 || j == 1) {\n" \
    "           if (i == -1) {\n" \
    "               avg1 += col;\n" \
    "           } else if (i == 1) {\n" \
    "               avg2 += col;\n" \
    "           }\n" \
    "       }\n" \
    "    }\n" \
    "    }\n" \
    "    //gl_FragColor.rg = (1.0/4.0) * (avg1.rg + avg1.ba);\n" \
    "    //gl_FragColor.ba = (1.0/4.0) * (avg2.rg + avg2.ba);\n" \
    "    gl_FragColor.g = (1.0/4.0) * (avg1.g + avg1.a);\n" \
    "    gl_FragColor.a = (1.0/4.0) * (avg2.g + avg2.a);\n" \
    "    if (foundRed) {\n" \
    "        gl_FragColor.r = 0.0;\n" \
    "        gl_FragColor.b = 0.0;\n" \
    "    } else {\n" \
    "        //// Scale [0.8,1] to [0.5,1]\n" \
    "        //// 2.0 * r - 1.0 \n" \
    "        //// where r = (avg.r + avg.b) / 4.0 \n" \
    "        //gl_FragColor.r = (2.0/4.0) * (avg1.r + avg1.b) - 1.0;\n" \
    "        //gl_FragColor.b = (2.0/4.0) * (avg2.r + avg2.b) - 1.0;\n" \
    "        gl_FragColor.r = (1.0/4.0) * (avg1.r + avg1.b);\n" \
    "        gl_FragColor.b = (1.0/4.0) * (avg2.r + avg2.b);\n" \
    "    }\n" \
    "}\n";

// Balltrack shader new third phase
// Ouput is 2X smaller in both directions
// In the height dimension, where we have one output:
// tex_unit:-8 -7 -6 -5 -4 -3 -2 -1  0  1  2  3  4  5  6  7  8
// Input:    |--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|
// texcoord:                      |--*--|
// samples:                       |--*--|
//
// In the width dimension, where we have two *outputs*:
// tex_unit:-6    -5    -4    -3    -2    -1     0     1     2     3     4     5     6
// Input:    |RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|
// texcoord:                               |-R-G-*-B-A-|
// Output:                                 |<R G>|
// Output:                                       |<B A>|
// samples:                                |--*--|--*--|  do not use GL_LINEAR ??
char BALLTRACK_FSHADER_SOURCE_3_NEW[] =  \
    "uniform sampler2D tex;\n" \
    "varying vec2 texcoord;\n" \
    "uniform vec2 tex_unit;\n" \
    "void main(void) {\n" \
    "    bool foundRed = false;\n" \
    "    for (int i = -2; i <= 2; i += 2) {\n" \
    "       vec4 col = texture2D(tex, texcoord + vec2(i,0) * tex_unit);\n" \
    "       if (min(col.r,col.b) < 0.78) {\n" \
    "           foundRed = true;\n" \
    "       }\n" \
    "    }\n" \
    "    vec4 col1 = texture2D(tex, texcoord - vec2(0.5,0) * tex_unit );\n" \
    "    vec4 col2 = texture2D(tex, texcoord + vec2(0.5,0) * tex_unit );\n" \
    "    gl_FragColor.rg = 0.5 * (col1.rg + col1.ba);\n" \
    "    gl_FragColor.ba = 0.5 * (col2.rg + col2.ba);\n" \
    "    if (foundRed) {\n" \
    "        gl_FragColor.r = 0.0;\n" \
    "        gl_FragColor.b = 0.0;\n" \
    "    } else {\n" \
    "        // Rescale from [0.8,1] to [0,1] \n" \
    "        gl_FragColor.r = 5.0 * gl_FragColor.r - 4.0;\n" \
    "        gl_FragColor.b = 5.0 * gl_FragColor.b - 4.0;\n" \
    "    }\n" \
     "}\n";


#define DEBUG 0

// Balltrack shader display phase
char BALLTRACK_FSHADER_SOURCE_DISPLAY[] =  \
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
    "    if ( mod(texcoord.x, tex_unit.x) < 0.5 * tex_unit.x ) {\n" \
    "        r = fil.r;\n" \
    "        g = fil.g;\n" \
    "    } else {\n" \
    "        r = fil.b;\n" \
    "        g = fil.a;\n" \
    "    }\n" \
    "    if (r > 0.3) {\n" \
    "        gl_FragColor = 0.7 * col + 0.3 * vec4(1.0, 0.0, 1.0, 1.0);\n" \
    "    } else if (g > 0.75) {\n" \
    "        gl_FragColor = 0.9 * col + 0.1 * vec4(0.0, 1.0, 0.0, 1.0);\n" \
    "    } else {\n" \
    "        gl_FragColor = col;\n" \
    "    }\n" \
    "    //gl_FragColor = 0.2 * col + 0.8 * vec4(r,r,r,1.0);\n" \
    "}\n";


// Balltrack fixed color shader
char BALLTRACK_FSHADER_SOURCE_FIXEDCOLOR[] =  \
    "uniform vec4 col;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    gl_FragColor = col;\n" \
    "}\n";

// Plain copy of the input
char BALLTRACK_FSHADER_SOURCE_PLAIN[] =  \
    "#extension GL_OES_EGL_image_external : require\n" \
    "uniform samplerExternalOES tex_rgb;\n" \
    "uniform samplerExternalOES tex_y;\n" \
    "uniform samplerExternalOES tex_u;\n" \
    "uniform samplerExternalOES tex_v;\n" \
    "varying vec2 texcoord;\n" \
    "void main(void) {\n" \
    "    vec4 col   = texture2D(tex_rgb, texcoord);\n" \
    "    vec4 col_y = texture2D(tex_y, texcoord);\n" \
    "    vec4 col_u = texture2D(tex_u, texcoord);\n" \
    "    vec4 col_v = texture2D(tex_v, texcoord);\n" \
    "    float Y = col_y.r;\n" \
    "    float U = col_u.r;\n" \
    "    float V = col_v.r;\n" \
    "    float B = clamp(1.164 * (Y - 0.0625) + 2.018 * (U - 0.5)  , 0.0,1.0);\n" \
    "    float G = clamp(1.164 * (Y - 0.0625) - 0.813 * (V - 0.5) - 0.391 * (U - 0.5) , 0.0,1.0);\n" \
    "    float R = clamp(1.164 * (Y - 0.0625) + 1.596 * (V - 0.5)  , 0.0,1.0);\n" \
    "    //gl_FragColor = vec4(abs(R - col.r), abs(G - col.g), abs(B - col.b), 1.0);\n" \
    "    gl_FragColor = vec4(R, G, B, 1.0);\n" \
    "    //gl_FragColor = col;\n" \
    "}\n";



static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
   -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f,
};

static GLuint quad_vbo; // vertex buffer object
static GLuint fbo;      // frame buffer object for render-to-texture
static GLuint rtt_tex1; // Texture for render-to-texture
static GLuint rtt_tex2; // Texture for render-to-texture
static GLuint rtt_tex3; // Texture for render-to-texture

static uint8_t* pixelbuffer; // For reading out result

int timeseriesfile;


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
    .fragment_source = BALLTRACK_FSHADER_SOURCE_2_NEW,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_3 =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_3_NEW,
    .uniform_names = {"tex", "tex_unit"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_display =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_DISPLAY,
    .uniform_names = {"tex_camera", "tex_unit", "tex_filter"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_fixedcolor =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_FIXEDCOLOR,
    .uniform_names = {"col"},
    .attribute_names = {"vertex"},
};

static SHADER_PROGRAM_T balltrack_shader_plain =
{
    .vertex_source = BALLTRACK_VSHADER_SOURCE,
    .fragment_source = BALLTRACK_FSHADER_SOURCE_PLAIN,
    .uniform_names = {"tex_rgb", "tex_y", "tex_u", "tex_v"},
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
    //Wrapping: clamp. Only use (s,t) as we are using a 2D texture
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL));
    return id;
}

int balltrack_core_init(int externalSamplerExtension, int flipY)
{
    int rc = 0;

    const char* glRenderer = (const char*)glGetString(GL_RENDERER);
    printf("OpenGL renderer string: %s\n", glRenderer);

    if (externalSamplerExtension == 0) {
        // Replace
        // "samplerExternalOES"
        // "sampler2D         "
        char* pos = 0;
        if ((pos = strstr(BALLTRACK_FSHADER_SOURCE_1, "samplerExternalOES"))){
            memcpy(pos, "sampler2D         ", 18);
        }
        if ((pos = strstr(BALLTRACK_FSHADER_SOURCE_DISPLAY, "samplerExternalOES"))){
            memcpy(pos, "sampler2D         ", 18);
        }
        if ((pos = strstr(BALLTRACK_FSHADER_SOURCE_PLAIN, "samplerExternalOES"))){
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

    printf("Building shader `phase 1`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_1);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_1, width0, height0, 1, 0);
    if (rc != 0)
        goto end;

    printf("Building shader `phase 2`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_2);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_2, width1, height1, 1, 0);
    if (rc != 0)
        goto end;

    printf("Building shader `phase 3`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_3);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_3, width2, height2, 1, 0);
    if (rc != 0)
        goto end;

    printf("Building shader `display`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_display);
    if (rc != 0)
        goto end;
#if DEBUG == 1
    rc = shader_set_uniforms(&balltrack_shader_display, width1, height1, 1, 1);
#elif DEBUG == 2
    rc = shader_set_uniforms(&balltrack_shader_display, width2, height2, 1, 1);
#else
    rc = shader_set_uniforms(&balltrack_shader_display, width3, height3, 1, 1);
#endif
    if (rc != 0)
        goto end;

    printf("Building shader `fixedcolor`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_fixedcolor);
    if (rc != 0)
        goto end;

    printf("Building shader `plain`\n");
    rc = balltrack_build_shader_program(&balltrack_shader_plain);
    if (rc != 0)
        goto end;
    rc = shader_set_uniforms(&balltrack_shader_plain, width0, height0, 0, 0);
    if (rc != 0)
        goto end;
    GLCHK(glUniform1i(balltrack_shader_plain.uniform_locations[1], 2)); // Texture unit
    GLCHK(glUniform1i(balltrack_shader_plain.uniform_locations[2], 3)); // Texture unit
    GLCHK(glUniform1i(balltrack_shader_plain.uniform_locations[3], 4)); // Texture unit

    // Buffer to read out pixels from last texture
    uint32_t buffer_size = width3 * height3 * 4;
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
    GLint tex1scaling = GL_LINEAR;
    GLint tex2scaling = GL_LINEAR;
    GLint tex3scaling = GL_NEAREST;
#if DEBUG == 1
    tex1scaling = GL_NEAREST;
#elif DEBUG == 2
    tex2scaling = GL_NEAREST;
#endif
    rtt_tex1 = createFilterTexture(width1, height1, tex1scaling);
    rtt_tex2 = createFilterTexture(width2, height2, tex2scaling);
    rtt_tex3 = createFilterTexture(width3, height3, tex3scaling);

    printf("Creating vertex-buffer object\n");
    GLCHK(glGenBuffers(1, &quad_vbo));
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));
    GLCHK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));

    GLCHK(glDisable(GL_BLEND));
    GLCHK(glDisable(GL_DEPTH_TEST));

    timeseriesfile = open("/tmp/timeseries.txt", O_WRONLY);
    if (!timeseriesfile) {
        printf("Unable to open /tmp/timeseries.txt\n");
    }
end:
    return rc;
}

typedef struct GLPOINT {
    GLfloat x;
    GLfloat y;
} GLPOINT;

// x,y are coordinates in [-1,1]x[-1,1] range
void draw_line_strip(GLPOINT* xys, int count, uint32_t color) {
    if (count == 0)
        return;

    float r, g, b, a;
    r = (1.0/255.0) * ((color      ) & 0xff);
    g = (1.0/255.0) * ((color >>  8) & 0xff);
    b = (1.0/255.0) * ((color >> 16) & 0xff);
    a = (1.0/255.0) * ((color >> 24) & 0xff);

    SHADER_PROGRAM_T* shader = &balltrack_shader_fixedcolor;
    GLCHK(glUseProgram(shader->program));
    GLCHK(glUniform4f(shader->uniform_locations[0], r, g, b, a));

    // Unbind the vertex buffer --> use client memory
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, 0));
    GLCHK(glEnableVertexAttribArray(shader->attribute_locations[0]));
    GLCHK(glVertexAttribPointer(shader->attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, (GLfloat*)xys));
    // Draw
    GLCHK(glDrawArrays(GL_LINE_STRIP, 0, count));
}

// x,y are coordinates in [-1,1]x[-1,1] range
void draw_square(float xmin, float xmax, float ymin, float ymax, uint32_t color) {
    // Draw a square
    GLPOINT vertexBuffer[5];
    vertexBuffer[0].x = xmin;
    vertexBuffer[0].y = ymin;
    vertexBuffer[1].x = xmax;
    vertexBuffer[1].y = ymin;
    vertexBuffer[2].x = xmax;
    vertexBuffer[2].y = ymax;
    vertexBuffer[3].x = xmin;
    vertexBuffer[3].y = ymax;
    vertexBuffer[4].x = xmin;
    vertexBuffer[4].y = ymin;
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
        // According to the open source GL driver for the VC4 chip,
        // [ https://github.com/anholt/mesa/wiki/VC4-Performance-Tricks ],
        // it is faster to clear the buffer even when writing to the complete screen
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        // Unset frame buffer object. Now draw to screen
        GLCHK(glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0));
        GLCHK(glViewport(0, 0, targetWidth, targetHeight));
        glClear(GL_COLOR_BUFFER_BIT); // See above comment
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
static int historyCount = 120;
GLPOINT ballXYs[120];
int ballTimes[120];
static int frameNumber = 0;
static int ballCur = 0;
float greenxmin, greenxmax, greenymin, greenymax;
static int ballGone = 0;

static int balltrack_readout(int width, int height) {
    frameNumber++;
    // Read texture
    // It packs two pixels into one:
    // RGBA is red,green,red,green filter values for neighbouring pixels
    if (pixelbuffer) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixelbuffer);
        if (glGetError() == GL_NO_ERROR) {
            // pixelbuffer[i*height + j] is i pixels from bottom and j from left
            int gxmin = (int)(0.45f * 2.0f * width);
            int gxmax = (int)(0.55f * 2.0f * width);
            int gymin = (int)(0.45f * height);
            int gymax = (int)(0.55f * height);

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
            gxmin -= 3;
            gxmax += 3;
            gymin -= 3;
            gymax += 3;
            if (gxmin < 0) gxmin = 0;
            if (gymin < 0) gymin = 0;
            if (gxmax > 2*width-1) gxmax = 2*width - 1;
            if (gymax > height) gymax = height;

            // Find the max orange intensity
            uint32_t maxx = 0, maxy = 0;
            uint32_t maxR = 0;
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
                    if (R1 > maxR) {
                        maxx = x1;
                        maxy = y;
                        maxR = R1;
                    }
                    if (R2 > maxR) {
                        maxx = x2;
                        maxy = y;
                        maxR = R2;
                    }
                }
            }
            // Take weighted average near the maximum
            int searchImin = maxy - 4;
            int searchImax = maxy + 4;
            int searchJmin = maxx/2 - 3;
            int searchJmax = (maxx+1)/2 + 3;
            if (searchImin < 0) searchImin = 0;
            if (searchJmin < 0) searchJmin = 0;
            if (searchImax > height) searchImax = height;
            if (searchJmax > width ) searchJmax = width;

            int threshold1 = 160;
            int threshold2 = 200;
            if (maxx < gxmin + 8 || maxx > gxmax - 8) {
                threshold1 = 80;
                threshold2 = 160;
            }


            uint32_t avgx = 0, avgy = 0;
            uint32_t weight = 0;
            uint32_t count = 0;
            if (maxR > threshold1) {
                ptr = (uint32_t*)pixelbuffer;
                for (int i = searchImin; i < searchImax; ++i) {
                    for (int j = searchJmin; j < searchJmax; ++j) {
                        // R,G,B,A = FF, 00, FF, FF
                        uint32_t rgba = ptr[i * width + j];
                        int R1 = (rgba      ) & 0xff;
                        //int G1 = (rgba >>  8) & 0xff;
                        int R2 = (rgba >> 16) & 0xff;
                        //int G2 = (rgba >> 24) & 0xff;
                        int y = i;
                        int x1 = 2*j;
                        int x2 = 2*j + 1;
                        avgx += x1 * R1;
                        avgy += y  * R1;
                        weight += R1;
                        avgx += x2 * R2;
                        avgy += y  * R2;
                        weight += R2;
                        count++;
                    }
                }
            }
            // Map to [-1,1]
            greenxmin = gxmin / ((float)width) - 1.0f;
            greenxmax = gxmax / ((float)width) - 1.0f;
            greenymin = (2.0f * gymin) / ((float)height) - 1.0f;
            greenymax = (2.0f * gymax) / ((float)height) - 1.0f;
            if (weight > threshold2) {
                ballGone = 0;
                // avgx, avgy are the bottom-left corner of the macropixels
                // Shift them by half a pixel to fix
                // Then, map them to [-1,1] range
                float x = 0.5f + (((float)avgx) / ((float)weight));
                float y = 0.5f + (((float)avgy) / ((float)weight));
                ballXYs[ballCur].x = x / ((float)width) - 1.0f;
                ballXYs[ballCur].y = (2.0f * y) / ((float)height) - 1.0f;
                ballTimes[ballCur] = frameNumber;
                ++ballCur;
                if(ballCur >= historyCount) {
                    ballCur = 0;

                    if (timeseriesfile) {
                        char buffer[128];
                        for (int i = 0; i < historyCount; ++i) {
                            int len = sprintf(buffer, "{%d, %f, %f},\n", ballTimes[i], ballXYs[i].x, ballXYs[i].y);
                            write(timeseriesfile, buffer, len);
                        }
                    }
                }
                //printf("Average x,y is (%u, %u)!", avgx, avgy);
                return 1;
            } else {
                if (ballGone++ == 60) {
                    printf("Ball has gone for 60 frames!\n");
                    int i = ballCur - 1;
                    if (i < 0) i = historyCount - 1;
                    int goal = 0;
                    if (ballXYs[i].x < greenxmin + (greenxmax - greenxmin) / 4.0f) {
                        printf("Goal for red!\n");
                        goal = 1;
                    }
                    if (ballXYs[i].y > greenxmin + 3.0f * (greenxmax - greenxmin) / 4.0f) {
                        printf("Goal for blue!\n");
                        goal = 2;
                    }
                    if (goal != 0) {
                        //int fd = open("/tmp/foos-debug.in", O_WRONLY);
                        //if (fd > 0) {
                        //    write(fd, (goal == 1 ? "BG\n" : "YG\n"), 4);
                        //    close(fd);
                        //}
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
    if (frameNumber == 60) {
        render_pass(&balltrack_shader_plain, srctype, srctex, 0, width, height);
        dump_frame(width, height, "framedump.tga");
        printf("Frame dumped to framedump.tga\n");
    }

    // Width,height is the size of the preview window
    // The source image might be much larger

    // First pass: hue filter into smaller texture
    render_pass(&balltrack_shader_1, srctype,       srctex,   rtt_tex1, width1, height1);
    // Second pass: dilate red players
    render_pass(&balltrack_shader_2, GL_TEXTURE_2D, rtt_tex1, rtt_tex2, width2, height2);
    // Third pass: downsample
    render_pass(&balltrack_shader_3, GL_TEXTURE_2D, rtt_tex2, rtt_tex3, width3, height3);
    // Readout result
    balltrack_readout(width3, height3);
    // Third pass: render to screen
    GLCHK(glActiveTexture(GL_TEXTURE1));
#if DEBUG == 1
    GLCHK(glBindTexture(GL_TEXTURE_2D, rtt_tex1));
#elif DEBUG == 2
    GLCHK(glBindTexture(GL_TEXTURE_2D, rtt_tex2));
#else
    GLCHK(glBindTexture(GL_TEXTURE_2D, rtt_tex3));
#endif
    render_pass(&balltrack_shader_display, srctype, srctex, 0, width, height);

#if 1
    // Draw green bounding box
    draw_square(greenxmin, greenxmax, greenymin, greenymax, 0xff00ff00);
#endif

#if 1
    // Draw line for ball history
    // Be carefull with circular buffer
    draw_line_strip(&ballXYs[0], ballCur, 0xffff0000);
    draw_line_strip(&ballXYs[ballCur], historyCount - ballCur, 0xffff0000);

    // Draw squares on detection points
    for (int i = ballCur - 20; i < ballCur; ++i) {
        int time = ballCur - i;
        int blue = 0xff - time;
        // The bytes are R,G,B,A but little-endian so 0xAABBGGRR
        int color = 0xff000000 | (blue << 16);
        float size = 0.002f * time;

        int idx = (i < 0 ? i + historyCount : i);
        GLPOINT* pt = &ballXYs[idx];
        draw_square(pt->x - 0.5f * size, pt->x + 0.5f * size, pt->y - size, pt->y + size, color);
    }
#endif

    return 0;
}

