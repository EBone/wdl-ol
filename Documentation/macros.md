# About the project level preprocessor macros
@todo improve this!

IPlug uses preprocessor macros to select certain APIs and functionality at compile time. The following macros can be defined at project level (in the visual studio project/.props file, xcode project/xconfig file, or CMake script). 

##IPlug
* VST_API 
* VST3_API
* AU_API
* AAX_API
* SA_API
* WAM_API
* USE_IDLE_CALLS: if this is enabled as a preprocessor macro IPlug::OnIdle() will be called in VST2 plug-ins
 
##IGraphics
* GRAPHICS_SCALING: enables hi DPI graphics
* NO_IGRAPHICS: define this to builder plug-in without IGraphics UI functionality
* IGRAPHICS_LICE: (default) define this in order to build your plug-in using LICE as the drawing API
* IGRAPHICS_CAIRO: define this in order to build your plug-in using CAIRO as the drawing API
* IGRAPHICS_NANOVG: define this in order to build your plug-in using 9 avg as the drawing API
* IGRAPHICS_AGG: define this in order to build your plug-in using Anti-Grain Geometry as the drawing API
* CONTROL_BOUNDS_COLOR=COLOR_WHITE : you can define this 2 change the color of control bounds when IGraphics::ShowControlBounds(true) is set (debug mode only). The default color is green. 
* USE_IDLE_CALLS: if this is enabled as a preprocessor macro IGraphics::OnGUIIdle() will be called