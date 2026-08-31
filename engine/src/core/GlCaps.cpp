#include "fitzel/core/GlCaps.hpp"

#include <glad/gl.h>

namespace fitzel::glcaps {

// glad fills these when the loader runs, one per version it managed to resolve
// against the driver. Reading them rather than parsing glGetString(GL_VERSION)
// on purpose: the string is the DRIVER's version, and what a caller needs to
// know is whether the entry points it is about to call are non-null.
bool compute() { return GLAD_GL_VERSION_4_3 != 0; }

// Asked of the context itself (core since 3.0), so it reports what is really
// current rather than what this build was generated for.
int majorVersion() {
    GLint v = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &v);
    return v;
}
int minorVersion() {
    GLint v = 0;
    glGetIntegerv(GL_MINOR_VERSION, &v);
    return v;
}

} // namespace fitzel::glcaps
