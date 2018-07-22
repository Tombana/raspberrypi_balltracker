// // Balltrack shader second phase
// // Ouput is 8X smaller in both directions!
// // We will use GL_LINEAR so that the GPU samples 4 texels at once.
// // The center of the output pixel (=texcoord) is at the intersection
// // of four input pixels.
// // tex_unit is size of input texel
// // NOTE: Trying to sample 16x16 pixels yields an out-of-memory error! 12x12 still works!
// // In the height dimension, where we have one output:
// // tex_unit:             0  1  2  3  4  5  6  7  8
// //          -8 -7 -6 -5 -4 -3 -2 -1  0  1  2  3  4  5  6  7  8
// // Input:    |--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|--|
// // texcoord:             |-----------*-----------|
// // samples1:             |--*--|--*--|--*--|--*--|
// // samples2:       |--*--|--*--|--*--|--*--|--*--|--*--|
// //
// // In the width dimension, where we have two *outputs*:
// // tex_unit:             0     1     2     3     4     5     6     7     8
// //          -6    -5    -4    -3    -2    -1     0     1     2     3     4     5     6
// // Input:    |RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|RG|BA|
// // texcoord:             |-----------------------*-----------------------|
// // samples1:             |-----*-----|-----*-----|-----*-----|-----*-----|
// // samples2:       |-----*-----|-----*-----|-----*-----|-----*-----|-----*-----|
// // Output:         |<--             R G             -->|
// // Output:                                 |<--             B A             -->|
// // One of the x-sample points is used in both results!
// // OOM:      |-----*-----|-----*-----|-----*-----|-----*-----|-----*-----|-----*-----| (out-of-memory)
// uniform sampler2D tex;
// varying vec2 texcoord;
// uniform vec2 tex_unit;
// void main(void) {
//     vec2 tbase = texcoord - vec2(4.0,5.0) * tex_unit;
//     vec2 tdif  = 2.0 * tex_unit;
//     vec4 avg1   = vec4(0,0,0,0);
//     vec4 avg2   = vec4(0,0,0,0);
//     vec4 avgboth= vec4(0,0,0,0);
//     for (int j = 0; j < 6; ++j) {
//        avgboth += texture2D(tex, tbase + vec2(2,j) * tdif);
//     }
//     for (int i = 0; i < 2; ++i) {
//     for (int j = 0; j < 6; ++j) {
//        avg1 += texture2D(tex, tbase + vec2(i,j) * tdif);
//     }
//     }
//     for (int i = 3; i < 5; ++i) {
//     for (int j = 0; j < 6; ++j) {
//        avg2 += texture2D(tex, tbase + vec2(i,j) * tdif);
//     }
//     }
//     avg1 += avgboth;
//     avg2 += avgboth;
//     gl_FragColor.rg = (1.0/36.0) * (avg1.rg + avg1.ba);
//     gl_FragColor.ba = (1.0/36.0) * (avg2.rg + avg2.ba);
// }


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
uniform sampler2D tex;
varying vec2 texcoord;
uniform vec2 tex_unit;
void main(void) {
    bool foundRed = false;
    vec4 avg1 = vec4(0,0,0,0);
    vec4 avg2 = vec4(0,0,0,0);
    for (int i = -3; i <= 3; i += 2) {
    for (int j = -5; j <= 5; j += 2) {
       vec4 col = texture2D(tex, texcoord + vec2(i,j) * tex_unit);
       if (min(col.r,col.b) < 0.78) {
           foundRed = true;
       }
       // I *really* hope it unrolls this loop
       if (j == -1 || j == 1) {
           if (i == -1) {
               avg1 += col;
           } else if (i == 1) {
               avg2 += col;
           }
       }
    }
    }
    //gl_FragColor.rg = (1.0/4.0) * (avg1.rg + avg1.ba);
    //gl_FragColor.ba = (1.0/4.0) * (avg2.rg + avg2.ba);
    gl_FragColor.g = (1.0/4.0) * (avg1.g + avg1.a);
    gl_FragColor.a = (1.0/4.0) * (avg2.g + avg2.a);
    if (foundRed) {
        gl_FragColor.r = 0.0;
        gl_FragColor.b = 0.0;
    } else {
        //// Scale [0.8,1] to [0.5,1]
        //// 2.0 * r - 1.0 
        //// where r = (avg.r + avg.b) / 4.0 
        //gl_FragColor.r = (2.0/4.0) * (avg1.r + avg1.b) - 1.0;
        //gl_FragColor.b = (2.0/4.0) * (avg2.r + avg2.b) - 1.0;
        gl_FragColor.r = (1.0/4.0) * (avg1.r + avg1.b);
        gl_FragColor.b = (1.0/4.0) * (avg2.r + avg2.b);
    }
}

