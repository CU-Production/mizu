// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// Adapted by Kent Hall for mizu on Horizon Linux.

#version 460 core

layout (location = 0) in vec2 vert_position;
layout (location = 1) in vec2 vert_tex_coord;

layout (location = 0) out vec2 frag_tex_coord;

layout (set = 0, binding = 0) uniform MatrixBlock {
    mat4 modelview_matrix;
};

void main() {
    gl_Position = modelview_matrix * vec4(vert_position, 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
}
