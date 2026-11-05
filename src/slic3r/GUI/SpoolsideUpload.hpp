#ifndef slic3r_SpoolsideUpload_hpp_
#define slic3r_SpoolsideUpload_hpp_

// DiscusSlicer: Bambu printers are never driven through the Bambu network plug-in. Their print button adds the
// finished, sliced .gcode.3mf to the print library of a Spoolside server instead; Spoolside decides which printer runs it.

#include <string>
#include <vector>
#include <boost/filesystem/path.hpp>

class wxWindow;
class wxString;

namespace Slic3r { namespace GUI {

// Edits the Spoolside server URL (app config) and API key (system keychain). Returns false if cancelled.
bool show_spoolside_settings(wxWindow *parent);

// Asks for the library entry name (prefilled with `name`) and the library ("playcase" or "personal", prefilled from
// Preferences). `plate_count` > 1 notes that every plate becomes its own entry. Returns false if cancelled.
bool spoolside_add_dialog(wxWindow *parent, int plate_count, wxString &name, std::string &scope);

struct SpoolsidePrint
{
    boost::filesystem::path file; // single-plate .gcode.3mf
    std::string             name; // library entry name (UTF-8)
};

// Uploads the prints to the Spoolside library `scope`, with progress and error dialogs, then confirms what was
// added or updated, with an "Open in Spoolside" button.
void spoolside_upload(wxWindow *parent, const std::vector<SpoolsidePrint> &prints, const std::string &scope);

}} // namespace Slic3r::GUI

#endif
