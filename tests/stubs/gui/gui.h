#pragma once

// Host viewport callbacks and minimal GUI service placeholders

#include "canvas.h"
#include <input/input.h>
#define RECORD_GUI "gui"
typedef struct Gui {
    int unused;
} Gui;
typedef struct ViewPort {
    void (*draw)(Canvas*, void*);
    void (*input)(InputEvent*, void*);
    void* context;
} ViewPort;
typedef enum {
    GuiLayerFullscreen
} GuiLayer;
// Allocate the callbacks and context needed to render through the host Canvas
ViewPort* view_port_alloc(void);
// Capture final UI state for assertions before freeing the viewport
void view_port_free(ViewPort* p);
// Retain the real drawing callback and its application context
void view_port_draw_callback_set(ViewPort* p, void (*fn)(Canvas*, void*), void* context);
// Retain the input callback and its application context for host callers
void view_port_input_callback_set(ViewPort* p, void (*fn)(InputEvent*, void*), void* context);
// Count a redraw and invoke the real callback immediately on a fresh host Canvas
void view_port_update(ViewPort* p);
// Expose the active viewport to scripted input observations
void gui_add_view_port(Gui* gui, ViewPort* p, GuiLayer layer);
// Accept GUI detachment; viewport disposal releases the host allocation separately
void gui_remove_view_port(Gui* gui, ViewPort* p);
