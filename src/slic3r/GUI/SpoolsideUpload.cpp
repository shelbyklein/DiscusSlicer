#include "SpoolsideUpload.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
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

void spoolside_upload(wxWindow *parent, const boost::filesystem::path &file, const std::string &name)
{
    std::string key = load_api_key();
    if (key.empty()) {
        if (!show_spoolside_settings(parent) || (key = load_api_key()).empty())
            return;
    }

    boost::system::error_code ec;
    const uintmax_t           size = boost::filesystem::file_size(file, ec);
    if (ec || size == 0) {
        show_error(parent, _L("The sliced file could not be created."));
        return;
    }
    if (size > MAX_UPLOAD_SIZE) {
        show_error(parent, _L("The sliced file is larger than Spoolside's 95 MB upload limit."));
        return;
    }

    const std::string base = server_url();
    std::atomic<int>  percent{0};
    std::atomic<bool> cancel{false}, done{false};
    std::string       body, error;
    unsigned          status = 0;

    auto http = Http::post(base + "/api/library");
    http.header("Content-Type", "application/octet-stream")
        .header("x-file-name", Http::url_encode(name))
        .header("x-library-scope", wxGetApp().app_config->get("spoolside_library"))
        .header("Authorization", "Bearer " + key)
        .timeout_connect(15)
        .set_post_body(file)
        .on_progress([&](Http::Progress progress, bool &cancel_request) {
            if (progress.ultotal > 0)
                percent = int(progress.ulnow * 100 / progress.ultotal);
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

    {
        wxProgressDialog progress(_L("Send to Spoolside"), wxString::Format(_L("Uploading %s"), from_u8(name)), 100, parent,
                                  wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
        std::thread worker([&] {
            http.perform_sync();
            done = true;
        });
        while (!done) {
            if (!progress.Update(std::min(percent.load(), 99)))
                cancel = true;
            wxMilliSleep(50);
        }
        worker.join();
    }

    if (cancel)
        return;

    if (!error.empty() || status < 200 || status >= 300) {
        std::string reason = error;
        try {
            auto j = nlohmann::json::parse(body);
            if (j.is_object() && j.contains("error"))
                reason = j["error"].get<std::string>();
        } catch (...) {}
        BOOST_LOG_TRIVIAL(error) << "Spoolside upload failed: HTTP " << status << ": " << reason;
        if (status == 401) {
            MessageDialog dlg(parent, _L("Spoolside did not accept the API key. Do you want to enter a new one?"), _L("Send to Spoolside"),
                              wxYES_NO | wxICON_WARNING);
            if (dlg.ShowModal() == wxID_YES)
                show_spoolside_settings(parent);
            return;
        }
        show_error(parent, wxString::Format(_L("Upload to Spoolside failed: %s"),
                                            reason.empty() ? wxString::Format("HTTP %u", status) : from_u8(reason)));
        return;
    }

    // Spoolside returns one library entry per plate; open the print when there is just one.
    std::string url = base + "/library";
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_array() && j.size() == 1 && j[0].contains("id"))
            url += "/" + j[0]["id"].get<std::string>();
    } catch (...) {}
    BOOST_LOG_TRIVIAL(info) << "Spoolside upload done: " << url;
    wxLaunchDefaultBrowser(from_u8(url));
}

}} // namespace Slic3r::GUI
