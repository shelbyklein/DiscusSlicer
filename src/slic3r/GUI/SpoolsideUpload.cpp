#include "SpoolsideUpload.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GUI_Utils.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Http.hpp"

#include <atomic>
#include <thread>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <wx/dialog.h>
#include <wx/progdlg.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

static const wxString     KEYCHAIN_SERVICE = "DiscusSlicer/Spoolside";
static const wxString     KEYCHAIN_USER    = "api-key";
static constexpr uintmax_t MAX_UPLOAD_SIZE = 95ull * 1024 * 1024; // Spoolside's request limit

static std::string load_api_key()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      user;
    wxSecretValue secret;
    if (!store.IsOk() || !store.Load(KEYCHAIN_SERVICE, user, secret) || !secret.IsOk())
        return {};
    return std::string(static_cast<const char *>(secret.GetData()), secret.GetSize());
}

static bool save_api_key(const std::string &key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;
    if (key.empty())
        return store.Delete(KEYCHAIN_SERVICE) || load_api_key().empty();
    return store.Save(KEYCHAIN_SERVICE, KEYCHAIN_USER, wxSecretValue(key.size(), key.data()));
}

static std::string server_url()
{
    std::string url = wxGetApp().app_config->get("spoolside_url");
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

bool show_spoolside_settings(wxWindow *parent)
{
    wxDialog dlg(parent, wxID_ANY, _L("Spoolside"));
    auto    *sizer = new wxBoxSizer(wxVERTICAL);
    auto    *grid  = new wxFlexGridSizer(2, wxSize(dlg.FromDIP(8), dlg.FromDIP(8)));
    grid->AddGrowableCol(1);

    auto *url = new wxTextCtrl(&dlg, wxID_ANY, from_u8(server_url()), wxDefaultPosition, wxSize(dlg.FromDIP(360), -1));
    // The stored key is never shown; leaving the field empty keeps it.
    const bool has_key = !load_api_key().empty();
    auto *key = new wxTextCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(dlg.FromDIP(360), -1), wxTE_PASSWORD);
    key->SetHint(has_key ? _L("Saved. Type a new key to replace it.") : _L("Paste the API key"));

    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Server URL")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(url, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("API key")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(key, 1, wxEXPAND);

    sizer->Add(new wxStaticText(&dlg, wxID_ANY, _L("Sliced plates are uploaded to this Spoolside print library. The API key is stored in the system keychain.")),
               0, wxALL, dlg.FromDIP(12));
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP(12));
    sizer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, dlg.FromDIP(12));
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();

    if (dlg.ShowModal() != wxID_OK)
        return false;

    wxGetApp().app_config->set("spoolside_url", into_u8(url->GetValue().Trim().Trim(false)));
    wxGetApp().app_config->save();
    const std::string new_key = into_u8(key->GetValue().Trim().Trim(false));
    if (!new_key.empty() && !save_api_key(new_key)) {
        show_error(parent, _L("Could not save the API key to the system keychain."));
        return false;
    }
    return true;
}

static wxString library_label(const std::string &scope) { return scope == "personal" ? _L("Personal") : _L("PlayCase"); }

class SpoolsideAddDialog : public DPIDialog
{
public:
    SpoolsideAddDialog(wxWindow *parent, int plate_count, const wxString &name, const std::string &scope)
        : DPIDialog(parent, wxID_ANY, plate_count > 1 ? _L("Add all plates to Spoolside library") : _L("Add to Spoolside library"),
                    wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        SetBackgroundColour(*wxWHITE);
        auto *main = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(0, 2, FromDIP(10), FromDIP(12));
        grid->AddGrowableCol(1);
        auto label = [this](const wxString &text) {
            auto *t = new wxStaticText(this, wxID_ANY, text);
            t->SetFont(Label::Body_14);
            return t;
        };

        m_name = new TextInput(this, name, "", "", wxDefaultPosition, wxSize(FromDIP(320), -1), wxTE_PROCESS_ENTER);
        m_name->GetTextCtrl()->SetMaxLength(80); // Spoolside's name limit
        m_library = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(320), -1), 0, nullptr, wxCB_READONLY);
        m_library->Append(library_label("playcase"));
        m_library->Append(library_label("personal"));
        m_library->SetSelection(scope == "personal" ? 1 : 0);

        grid->Add(label(_L("Library name")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_name, 1, wxEXPAND);
        grid->Add(label(_L("Library")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_library, 1, wxEXPAND);
        main->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));

        if (plate_count > 1) {
            auto *note = new wxStaticText(this, wxID_ANY,
                wxString::Format(_L("Each of the %d plates becomes its own library entry, named \"<name> – <plate name>\"."), plate_count));
            note->SetFont(Label::Body_12);
            note->SetForegroundColour(wxColour(144, 144, 144));
            note->Wrap(FromDIP(420));
            main->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        }

        auto *buttons = new DialogButtons(this, {"Add", "Cancel"}, _L("Add"));
        Button *add = buttons->GetButtonFromID(wxID_ADD);
        add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        m_name->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { finish(); });
        m_name->GetTextCtrl()->Bind(wxEVT_TEXT, [this, add](wxCommandEvent &e) {
            add->Enable(!get_name().empty());
            e.Skip();
        });
        add->Enable(!get_name().empty());
        main->Add(buttons, 0, wxEXPAND);

        SetSizerAndFit(main);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        m_name->GetTextCtrl()->SetFocus();
        m_name->GetTextCtrl()->SelectAll();
    }

    wxString    get_name() const { return wxString(m_name->GetTextCtrl()->GetValue()).Trim().Trim(false); }
    std::string get_scope() const { return m_library->GetSelection() == 1 ? "personal" : "playcase"; }

protected:
    void on_dpi_changed(const wxRect &) override
    {
        Fit();
        Refresh();
    }

private:
    void finish()
    {
        if (!get_name().empty())
            EndModal(wxID_OK);
    }

    TextInput *m_name;
    ComboBox  *m_library;
};

bool spoolside_add_dialog(wxWindow *parent, int plate_count, wxString &name, std::string &scope)
{
    SpoolsideAddDialog dlg(parent, plate_count, name, wxGetApp().app_config->get("spoolside_library"));
    if (dlg.ShowModal() != wxID_OK)
        return false;
    name  = dlg.get_name();
    scope = dlg.get_scope();
    return true;
}

namespace {
struct Added
{
    std::string name;
    bool        replaced = false;
};
} // namespace

void spoolside_upload(wxWindow *parent, const std::vector<SpoolsidePrint> &prints, const std::string &scope)
{
    const wxString title = prints.size() > 1 ? _L("Add all plates to Spoolside library") : _L("Add to Spoolside library");
    std::string key = load_api_key();
    if (key.empty()) {
        if (!show_spoolside_settings(parent) || (key = load_api_key()).empty())
            return;
    }

    for (const SpoolsidePrint &print : prints) {
        boost::system::error_code ec;
        const uintmax_t           size = boost::filesystem::file_size(print.file, ec);
        if (ec || size == 0) {
            show_error(parent, _L("The sliced file could not be created."));
            return;
        }
        if (size > MAX_UPLOAD_SIZE) {
            show_error(parent, wxString::Format(_L("\"%s\" is larger than Spoolside's 95 MB upload limit."), from_u8(print.name)));
            return;
        }
    }

    const std::string  base = server_url();
    std::vector<Added> added;
    std::atomic<int>   percent{0};
    std::atomic<bool>  cancel{false};
    std::string        body, error;
    unsigned           status = 0;
    {
        wxProgressDialog progress(title, wxString::Format(_L("Uploading %s"), from_u8(prints.front().name)), 100, parent,
                                  wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
        for (size_t i = 0; i < prints.size() && !cancel; ++i) {
            const SpoolsidePrint &print = prints[i];
            body.clear();
            error.clear();
            status  = 0;
            percent = 0;
            // Spoolside names a single-plate entry after x-file-name minus ".gcode.3mf", and replaces the entry with
            // that name in the same library. The suffix keeps a name that itself ends in ".3mf" intact.
            auto http = Http::post(base + "/api/library");
            http.header("Content-Type", "application/octet-stream")
                .header("x-file-name", Http::url_encode(print.name + ".gcode.3mf"))
                .header("x-library-scope", scope)
                .header("Authorization", "Bearer " + key)
                .timeout_connect(15)
                .set_post_body(print.file)
                .on_progress([&](Http::Progress p, bool &cancel_request) {
                    if (p.ultotal > 0)
                        percent = int(p.ulnow * 100 / p.ultotal);
                    cancel_request = cancel;
                })
                .on_complete([&](std::string response, unsigned http_status) {
                    body   = std::move(response);
                    status = http_status;
                })
                .on_error([&](std::string response, std::string message, unsigned http_status) {
                    body   = std::move(response);
                    error  = std::move(message);
                    status = http_status;
                });

            std::atomic<bool> done{false};
            std::thread       worker([&] {
                http.perform_sync();
                done = true;
            });
            const wxString message = wxString::Format(_L("Uploading %s"), from_u8(print.name));
            while (!done) {
                const int overall = int((i * 100 + std::min(percent.load(), 99)) / prints.size());
                if (!progress.Update(overall, message))
                    cancel = true;
                wxMilliSleep(50);
            }
            worker.join();
            if (cancel || !error.empty() || status < 200 || status >= 300)
                break;

            Added entry{print.name};
            try {
                auto j = nlohmann::json::parse(body);
                if (j.is_array() && !j.empty() && j[0].is_object()) {
                    entry.name     = j[0].value("name", entry.name);
                    entry.replaced = j[0].value("replaced", false);
                }
            } catch (...) {}
            added.push_back(std::move(entry));
        }
    }

    const wxString library = library_label(scope);
    const bool     failed  = added.size() < prints.size();
    if (failed && !cancel) {
        std::string reason = error;
        try {
            auto j = nlohmann::json::parse(body);
            if (j.is_object() && j.contains("error"))
                reason = j["error"].get<std::string>();
        } catch (...) {}
        BOOST_LOG_TRIVIAL(error) << "Spoolside upload failed: HTTP " << status << ": " << reason;
        if (status == 401) {
            MessageDialog dlg(parent, _L("Spoolside did not accept the API key. Do you want to enter a new one?"), title,
                              wxYES_NO | wxICON_WARNING);
            if (dlg.ShowModal() == wxID_YES)
                show_spoolside_settings(parent);
            return;
        }
        wxString msg = wxString::Format(_L("Upload to Spoolside failed: %s"),
                                        reason.empty() ? wxString::Format("HTTP %u", status) : from_u8(reason));
        if (!added.empty())
            msg += "\n\n" + wxString::Format(_L("%d of %d plates were added before the error."), int(added.size()), int(prints.size()));
        show_error(parent, msg);
        return;
    }
    if (added.empty())
        return; // cancelled before anything was added

    wxString msg;
    if (added.size() == 1)
        msg = wxString::Format(added[0].replaced ? _L("Updated \"%s\" in the %s library.") : _L("Added \"%s\" to the %s library."),
                               from_u8(added[0].name), library);
    else {
        msg = wxString::Format(_L("%d prints in the %s library:"), int(added.size()), library);
        for (const Added &a : added)
            msg += "\n" + wxString::Format(a.replaced ? _L("Updated \"%s\"") : _L("Added \"%s\""), from_u8(a.name));
    }
    if (cancel)
        msg += "\n\n" + _L("The remaining plates were cancelled.");

    // Spoolside has no address for a single sliced print; open the library it went to.
    const std::string url = base + (scope == "personal" ? "/personal" : "/library/sliced");
    BOOST_LOG_TRIVIAL(info) << "Spoolside upload done: " << added.size() << " entries, " << url;

    MessageDialog dlg(parent, msg, _L("Spoolside"), wxYES_NO | wxICON_INFORMATION);
    dlg.SetButtonLabel(wxID_YES, _L("Open in Spoolside"));
    dlg.SetButtonLabel(wxID_NO, _L("OK"), true);
    if (dlg.ShowModal() == wxID_YES)
        wxLaunchDefaultBrowser(from_u8(url));
}

}} // namespace Slic3r::GUI
