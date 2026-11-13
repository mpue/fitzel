#pragma once

#include <string>

#include "Export3mf.hpp"

struct EditorContext;

// File > Export Selection as 3MF: the dialog between the menu and the file.
// It asks the one thing a print needs that the scene does not say -- how big --
// showing the size it will come out at, then the save dialog. Editor only.
namespace export3mf {

struct Dialog {
    bool        opening = false;   // begin() asks for the modal; draw() opens it
    Shape       shape;             // the selection as it was when it opened
    int         objects = 0;
    bool        fit = false;       // false: as in the scene; true: longest side = fitMm
    float       fitMm = 100.0f;
    std::string name;              // the file name offered
    std::string dir;               // where the last one went
};

// Is there anything selected that would come out as a surface?
bool canExport(const EditorContext& ed);

// Gather the selection and open the dialog. Nothing to export: the status line
// says so and no dialog opens.
void begin(Dialog& d, EditorContext& ed);

// The modal, every frame. `projectDir` is where the save dialog starts until a
// file has been written somewhere else.
void draw(Dialog& d, EditorContext& ed, const std::string& projectDir);

// The size the dialog has chosen, as write() takes it.
Options options(const Dialog& d);

} // namespace export3mf
