#ifndef slic3r_SpoolsideUpload_hpp_
#define slic3r_SpoolsideUpload_hpp_

// DiscusSlicer: Bambu printers are never driven through the Bambu network plug-in. Their print button adds the
// finished, sliced .gcode.3mf of the current plate to the print library of a Spoolside server instead, together
// with the STL of each object on the plate as a Spoolside asset, and can start it on a printer Spoolside runs.

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

// The printer to start the plate on once it is in the library. Spoolside does the sending (LAN upload and start)
// and checks the bed with its camera first. An empty `printer` means library only.
struct SpoolsidePrint
{
    std::string      printer;          // Spoolside printer id (serial)
    std::string      printer_name;     // for messages
    bool             external = false; // feed from the external spool instead of the AMS
    std::vector<int> slots;            // AMS slot (0-3) for filament id i+1, -1 where the plate doesn't use it
};

// A filament the plate prints with: its 1-based filament id, as in the sliced file, and its type and colour.
struct SpoolsideFilament
{
    int         id;
    std::string type;
    std::string colour; // "#RRGGBB"
};

// Asks for the library entry name (prefilled with `name`), the library ("playcase" or "personal", prefilled from
// Preferences), per object whether to update an existing Spoolside asset, add a new one, or leave it out, and
// optionally a printer to start the plate on, with the spool each of the plate's `filaments` is fed from.
// An empty `scope` is a test print: nothing is added to the library and a printer is required.
// Returns false if cancelled.
bool spoolside_add_dialog(wxWindow *parent, wxString &name, std::string &scope, std::vector<SpoolsideAsset> &assets,
                          const std::vector<SpoolsideFilament> &filaments, SpoolsidePrint &print);

// Uploads the chosen assets, then the plate to the Spoolside library `scope` as `name`, then links the plate to
// those assets, then (if `print.printer` is set) starts it on that printer through Spoolside. With an empty `scope`
// (a test print) only the plate is sent, straight to the printer, and Spoolside doesn't keep it. Shows progress and
// errors, then confirms what was added, updated or started, with an "Open in Spoolside" button.
void spoolside_upload(wxWindow *parent, const boost::filesystem::path &plate_file, const std::string &name,
                      const std::string &scope, const std::vector<SpoolsideAsset> &assets, const SpoolsidePrint &print);

}} // namespace Slic3r::GUI

#endif
