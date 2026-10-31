/*
 * The FreeType modules fbengine's text uses, in place of FreeType's own list: TrueType and
 * OpenType (CFF) fonts and their collections, their hinting, and the smooth renderer, so .fon,
 * BDF, PCF, Type 1, and PFR fonts, which HarfBuzz can't shape, aren't read. FreeType's SVG
 * renderer is kept, as loading an OpenType-SVG glyph in color takes it for granted; with no hooks
 * given it, it refuses to draw one, and the glyph's outline is drawn instead.
 */

FT_USE_MODULE(FT_Module_Class, autofit_module_class)
FT_USE_MODULE(FT_Driver_ClassRec, tt_driver_class)
FT_USE_MODULE(FT_Driver_ClassRec, cff_driver_class)
FT_USE_MODULE(FT_Module_Class, psaux_module_class)
FT_USE_MODULE(FT_Module_Class, psnames_module_class)
FT_USE_MODULE(FT_Module_Class, pshinter_module_class)
FT_USE_MODULE(FT_Module_Class, sfnt_module_class)
FT_USE_MODULE(FT_Renderer_Class, ft_smooth_renderer_class)
FT_USE_MODULE(FT_Renderer_Class, ft_svg_renderer_class)
