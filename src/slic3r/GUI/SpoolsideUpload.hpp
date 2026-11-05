#ifndef slic3r_SpoolsideUpload_hpp_
#define slic3r_SpoolsideUpload_hpp_

// DiscusSlicer: Bambu printers are never driven through the Bambu network plug-in. "Print" and "Send"
// upload the sliced .gcode.3mf to the print library of a Spoolside server instead, and Spoolside starts the print.

#include <string>
#include <boost/filesystem/path.hpp>

class wxWindow;

namespace Slic3r { namespace GUI {

// Edits the Spoolside server URL (app config) and API key (system keychain). Returns false if cancelled.
bool show_spoolside_settings(wxWindow *parent);

// Uploads a sliced .gcode.3mf to the Spoolside print library under `name`, with progress and error dialogs,
// then opens the uploaded print in the browser.
void spoolside_upload(wxWindow *parent, const boost::filesystem::path &file, const std::string &name);

}} // namespace Slic3r::GUI

#endif
