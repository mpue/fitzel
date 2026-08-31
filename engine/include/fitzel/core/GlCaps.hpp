#pragma once

// What the CURRENT context turned out to be able to do.
//
// The engine draws in OpenGL 3.3 and means it: every shader it ships is
// `#version 330 core`, and a machine that can only give it 3.3 loses nothing.
// The window asks for 4.3 first all the same, because two things cannot be
// written below it at all -- compute shaders and shader storage buffers -- and
// one feature wants both (the GPU path tracer). Asking costs nothing: a 4.3
// core context runs every 330 shader unchanged.
//
// So this is not a version check to gate the engine on. It is the one question
// a caller with a compute kernel in its hand has to ask before it tries.
namespace fitzel::glcaps {

// True when the context that is current reached 4.3, i.e. compute shaders,
// shader storage buffers and image load/store are all there. Answers for the
// context glad was loaded against, so it is meaningless before that -- and,
// like everything else in GL, it is about the calling thread's current context.
bool compute();

// The version glad actually resolved, for a message that tells the author what
// they have rather than only what they lack.
int majorVersion();
int minorVersion();

} // namespace fitzel::glcaps
