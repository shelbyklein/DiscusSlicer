#ifndef slic3r_SpoolsideUpload_hpp_
#define slic3r_SpoolsideUpload_hpp_

// DiscusSlicer: Bambu printers are never driven through the Bambu network plug-in. Their print button adds the
// finished, sliced .gcode.3mf of the current plate to the print library of a Spoolside server instead (Spoolside
// decides which printer runs it), together with the STL of each object on the plate as a Spoolside asset.

#include <string>
#include <vector>
#include <boost/filesystem/path.hpp>

class wxWindow;
class wxString;

namespace Slic3r { namespace GUI {

// Edits the Spoolside server URL (app config) and API key (system keychain). Returns false if cancelled.
bool show_spoolside_settings(wxWindow *parent);

// One object on the plate and what to do with its STL in Spoolside's asset library.
struct SpoolsideAsset
{
    std::string             object;          // object name (UTF-8), set by the caller
    bool                    upload = false;  // set by the dialog
    bool                    replaces = false; // updates an asset that already has this source
    std::string             name;            // asset name, for the confirmation
    std::string             meta;            // x-asset JSON
    boost::filesystem::path stl;             // binary STL, written by the caller for uploads
};

// Asks for the library entry name (prefilled with `name`), the library ("playcase" or "personal", prefilled from
// Preferences) and, per object, whether to update an existing Spoolside asset, add a new one, or leave it out.
// Returns false if cancelled.
bool spoolside_add_dialog(wxWindow *parent, wxString &name, std::string &scope, std::vector<SpoolsideAsset> &assets);

// Uploads the chosen assets, then the plate to the Spoolside library `scope` as `name`, then links the plate to
// those assets. Shows progress and errors, then confirms what was added or updated, with an "Open in Spoolside" button.
void spoolside_upload(wxWindow *parent, const boost::filesystem::path &plate_file, const std::string &name,
                      const std::string &scope, const std::vector<SpoolsideAsset> &assets);

}} // namespace Slic3r::GUI

#endif
