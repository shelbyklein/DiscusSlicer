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
#include <set>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/scrolwin.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

static const wxString     KEYCHAIN_SERVICE = "DiscusSlicer/Spoolside";
static const wxString     KEYCHAIN_USER    = "api-key";
static constexpr uintmax_t MAX_UPLOAD_SIZE = 95ull * 1024 * 1024; // Spoolside's request limit for prints
static constexpr uintmax_t MAX_ASSET_SIZE  = 50ull * 1024 * 1024; // ... and for asset STLs

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

using json = nlohmann::json;

namespace {

// One finished request to Spoolside.
struct Reply
{
    unsigned    status = 0;
    std::string body, error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    // Spoolside's {"error": ...} message, else curl's, else the HTTP status.
    wxString reason() const
    {
        try {
            auto j = json::parse(body);
            if (j.is_object() && j.contains("error") && j["error"].is_string())
                return from_u8(j["error"].get<std::string>());
        } catch (...) {}
        return error.empty() ? wxString::Format("HTTP %u", status) : from_u8(error);
    }
};

Http &prepare(Http &http, const std::string &key, Reply &reply)
{
    return http.header("Authorization", "Bearer " + key)
        .timeout_connect(15)
        .on_complete([&reply](std::string body, unsigned status) {
            reply.body   = std::move(body);
            reply.status = status;
        })
        .on_error([&reply](std::string body, std::string error, unsigned status) {
            reply.body   = std::move(body);
            reply.error  = std::move(error);
            reply.status = status;
        });
}

// Asks for the key if none is saved. Returns the key, or empty if the user cancelled.
std::string api_key(wxWindow *parent)
{
    std::string key = load_api_key();
    if (key.empty() && show_spoolside_settings(parent))
        key = load_api_key();
    return key;
}

void offer_new_key(wxWindow *parent, const wxString &title)
{
    MessageDialog dlg(parent, _L("Spoolside did not accept the API key. Do you want to enter a new one?"), title, wxYES_NO | wxICON_WARNING);
    if (dlg.ShowModal() == wxID_YES)
        show_spoolside_settings(parent);
}

std::string lower(std::string s)
{
    boost::algorithm::to_lower(s);
    return s;
}

// Object and file names compared without case or ".stl".
std::string match_key(std::string s)
{
    boost::algorithm::trim(s);
    if (boost::algorithm::iends_with(s, ".stl"))
        s.resize(s.size() - 4);
    return lower(s);
}

std::string str(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Assets imported by Spoolside's Dropbox script (scripts/import-assets.mjs) have the file's path inside the Dropbox
// print-file folder as their source; uploads from Spoolside's web page use "upload/...", DiscusSlicer "discusslicer/...".
bool from_dropbox(const std::string &source)
{
    return !source.empty() && !boost::algorithm::starts_with(source, "upload/") && !boost::algorithm::starts_with(source, "discusslicer/");
}

std::string new_asset_source(const std::string &name) { return "discusslicer/" + name + ".stl"; }

const char *ASSET_TYPES[] = {"Case", "Faceplate", "Sleeve", "Part"};

} // namespace

class SpoolsideAddDialog : public DPIDialog
{
    enum Action { UPDATE, ADD, SKIP };
    struct Row
    {
        ComboBox     *action;
        wxPanel      *update_panel;
        ComboBox     *asset;
        wxStaticText *note;
        wxPanel      *new_panel;
        TextInput    *name;
        ComboBox     *type;
        ComboBox     *generation;
    };

public:
    SpoolsideAddDialog(wxWindow *parent, const wxString &name, const std::string &scope, const std::vector<SpoolsideAsset> &objects, const json &assets)
        : DPIDialog(parent, wxID_ANY, _L("Add to Spoolside library"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_objects(objects)
    {
        // Every asset that can hold a printable STL (phones are reference models), by name.
        for (const json &a : assets)
            if (a.is_object() && str(a, "type") != "Phone" && !str(a, "id").empty())
                m_candidates.push_back(a);
        std::sort(m_candidates.begin(), m_candidates.end(), [](const json &a, const json &b) { return lower(str(a, "name")) < lower(str(b, "name")); });
        for (const json &a : assets)
            if (a.is_object())
                m_sources.insert(str(a, "source"));

        SetBackgroundColour(*wxWHITE);
        auto *main = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(0, 2, FromDIP(10), FromDIP(12));
        grid->AddGrowableCol(1);
        auto label = [](wxWindow *parent, const wxString &text, const wxFont &font) {
            auto *t = new wxStaticText(parent, wxID_ANY, text);
            t->SetFont(font);
            return t;
        };
        const int field_width = FromDIP(360);

        m_name = new TextInput(this, name, "", "", wxDefaultPosition, wxSize(field_width, -1), wxTE_PROCESS_ENTER);
        m_name->GetTextCtrl()->SetMaxLength(80); // Spoolside's name limit
        m_library = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(field_width, -1), 0, nullptr, wxCB_READONLY);
        m_library->Append(library_label("playcase"));
        m_library->Append(library_label("personal"));
        m_library->SetSelection(scope == "personal" ? 1 : 0);

        grid->Add(label(this, _L("Library name"), Label::Body_14), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_name, 1, wxEXPAND);
        grid->Add(label(this, _L("Library"), Label::Body_14), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_library, 1, wxEXPAND);
        main->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));

        if (!m_objects.empty()) {
            main->Add(label(this, _L("Models on this plate"), Label::Head_14), 0, wxLEFT | wxRIGHT, FromDIP(20));
            m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
            m_list->SetBackgroundColour(*wxWHITE);
            m_list->SetScrollRate(0, FromDIP(10));
            auto *rows = new wxFlexGridSizer(0, 2, FromDIP(6), FromDIP(12));
            rows->AddGrowableCol(1);
            for (const SpoolsideAsset &object : m_objects)
                add_row(rows, object.object, field_width);
            m_list->SetSizer(rows);
            main->Add(m_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
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

        SetSizer(main);
        wxGetApp().UpdateDlgDarkUI(this);
        relayout();
        CenterOnParent();
        m_name->GetTextCtrl()->SetFocus();
        m_name->GetTextCtrl()->SelectAll();
    }

    wxString                           get_name() const { return wxString(m_name->GetTextCtrl()->GetValue()).Trim().Trim(false); }
    std::string                        get_scope() const { return m_library->GetSelection() == 1 ? "personal" : "playcase"; }
    const std::vector<SpoolsideAsset> &get_assets() const { return m_objects; }

protected:
    void on_dpi_changed(const wxRect &) override { relayout(); }

private:
    void add_row(wxFlexGridSizer *rows, const std::string &object, int width)
    {
        Row row;
        auto *title = new wxStaticText(m_list, wxID_ANY, from_u8(object), wxDefaultPosition, wxSize(FromDIP(170), -1), wxST_ELLIPSIZE_END);
        title->SetFont(Label::Body_14);
        title->SetToolTip(from_u8(object));
        row.action = new ComboBox(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(width, -1), 0, nullptr, wxCB_READONLY);
        row.action->Append(_L("Update existing asset"));
        row.action->Append(_L("Add as new asset"));
        row.action->Append(_L("Don't upload"));

        // Update: an existing asset, preselected by name (or by the file name of its source).
        row.update_panel = new wxPanel(m_list);
        auto *update_sizer = new wxBoxSizer(wxVERTICAL);
        row.asset = new ComboBox(row.update_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(width, -1), 0, nullptr, wxCB_READONLY);
        const std::string key = match_key(object);
        int match = -1;
        for (size_t i = 0; i < m_candidates.size(); ++i) {
            const json &a = m_candidates[i];
            wxString text = from_u8(str(a, "name")) + "  (" + from_u8(str(a, "type"));
            if (a.value("generation", 3) == 4)
                text += ", Gen 4";
            row.asset->Append(text + ")");
            const std::string source = str(a, "source");
            if (match < 0 && (match_key(str(a, "name")) == key ||
                              (!source.empty() && match_key(boost::filesystem::path(source).filename().string()) == key)))
                match = int(i);
        }
        row.note = new wxStaticText(row.update_panel, wxID_ANY, _L("Re-running Spoolside's Dropbox import would put the Dropbox file back."));
        row.note->SetFont(Label::Body_12);
        row.note->SetForegroundColour(wxColour(144, 144, 144));
        row.note->Wrap(width);
        update_sizer->Add(row.asset, 0, wxEXPAND);
        update_sizer->Add(row.note, 0, wxTOP, FromDIP(4));
        row.update_panel->SetSizer(update_sizer);

        // Add: name, type and generation of a new asset.
        row.new_panel = new wxPanel(m_list);
        auto *new_sizer = new wxBoxSizer(wxHORIZONTAL);
        std::string name = object;
        if (boost::algorithm::iends_with(name, ".stl"))
            name.resize(name.size() - 4);
        row.name = new TextInput(row.new_panel, from_u8(name), "", "", wxDefaultPosition, wxSize(-1, -1));
        row.name->GetTextCtrl()->SetMaxLength(100);
        row.type = new ComboBox(row.new_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), 0, nullptr, wxCB_READONLY);
        int type = 3; // Part
        for (int i = 0; i < 4; ++i) {
            row.type->Append(_(ASSET_TYPES[i]));
            if (type == 3 && lower(name).find(lower(ASSET_TYPES[i])) != std::string::npos)
                type = i;
        }
        row.type->SetSelection(type);
        row.generation = new ComboBox(row.new_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1), 0, nullptr, wxCB_READONLY);
        row.generation->Append(_L("Gen 3"));
        row.generation->Append(_L("Gen 4"));
        row.generation->SetSelection(0);
        new_sizer->Add(row.name, 1, wxEXPAND);
        new_sizer->Add(row.type, 0, wxLEFT, FromDIP(6));
        new_sizer->Add(row.generation, 0, wxLEFT, FromDIP(6));
        row.new_panel->SetSizer(new_sizer);

        // Nothing is uploaded unless it matches an existing asset or is chosen here.
        row.action->SetSelection(match >= 0 ? UPDATE : SKIP);
        row.asset->SetSelection(match);

        rows->Add(title, 0, wxALIGN_CENTER_VERTICAL);
        rows->Add(row.action, 1, wxEXPAND);
        rows->AddSpacer(0);
        auto *details = new wxBoxSizer(wxVERTICAL);
        details->Add(row.update_panel, 0, wxEXPAND);
        details->Add(row.new_panel, 0, wxEXPAND);
        rows->Add(details, 1, wxEXPAND | wxBOTTOM, FromDIP(6));

        row.action->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { relayout(); });
        row.asset->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { relayout(); });
        m_rows.push_back(row);
    }

    void relayout()
    {
        for (Row &row : m_rows) {
            const int action = row.action->GetSelection();
            const int asset  = row.asset->GetSelection();
            row.update_panel->Show(action == UPDATE);
            row.note->Show(asset >= 0 && from_dropbox(str(m_candidates[asset], "source")));
            row.new_panel->Show(action == ADD);
        }
        if (m_list) {
            m_list->GetSizer()->Layout();
            const wxSize best = m_list->GetSizer()->GetMinSize();
            m_list->SetMinSize(wxSize(best.x + FromDIP(16), std::min(best.y, FromDIP(340))));
            m_list->FitInside();
        }
        Layout();
        Fit();
        Refresh();
    }

    void finish()
    {
        if (get_name().empty())
            return;
        std::set<std::string> ids, sources;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row      &row    = m_rows[i];
            SpoolsideAsset &out    = m_objects[i];
            const wxString  object = from_u8(out.object);
            json            meta;
            out.upload = row.action->GetSelection() != SKIP;
            if (row.action->GetSelection() == UPDATE) {
                const int asset = row.asset->GetSelection();
                if (asset < 0) {
                    show_error(this, wxString::Format(_L("Choose the Spoolside asset to update with \"%s\"."), object));
                    return;
                }
                // Spoolside replaces the STL of the asset with the same source and keeps its name, status and notes.
                const json &a = m_candidates[asset];
                if (str(a, "source").empty()) {
                    show_error(this, wxString::Format(_L("\"%s\" has no source in Spoolside, so its STL can't be replaced."), from_u8(str(a, "name"))));
                    return;
                }
                for (const char *key : {"name", "type", "generation", "status", "note", "fit", "source", "category"})
                    if (a.contains(key))
                        meta[key] = a[key];
                out.replaces = true;
                if (!ids.insert(str(a, "id")).second) {
                    show_error(this, wxString::Format(_L("\"%s\" is chosen for more than one model."), from_u8(str(a, "name"))));
                    return;
                }
            } else if (row.action->GetSelection() == ADD) {
                const std::string name = into_u8(wxString(row.name->GetTextCtrl()->GetValue()).Trim().Trim(false));
                if (name.empty()) {
                    show_error(this, wxString::Format(_L("Enter a name for the new asset from \"%s\"."), object));
                    return;
                }
                meta = {{"name", name},
                        {"type", ASSET_TYPES[std::max(0, row.type->GetSelection())]},
                        {"generation", row.generation->GetSelection() == 1 ? 4 : 3},
                        {"status", "Up to date"},
                        {"source", new_asset_source(name)}};
                // Adding the same name again later updates that asset rather than making a second one.
                out.replaces = m_sources.count(new_asset_source(name)) > 0;
            } else
                continue;
            out.name = meta.value("name", out.object);
            out.meta = meta.dump();
            if (!sources.insert(meta.value("source", "")).second) {
                show_error(this, wxString::Format(_L("\"%s\" is chosen for more than one model."), from_u8(out.name)));
                return;
            }
        }
        EndModal(wxID_OK);
    }

    TextInput                  *m_name;
    ComboBox                   *m_library;
    wxScrolledWindow           *m_list = nullptr;
    std::vector<Row>            m_rows;
    std::vector<json>           m_candidates;
    std::set<std::string>       m_sources;
    std::vector<SpoolsideAsset> m_objects;
};

bool spoolside_add_dialog(wxWindow *parent, wxString &name, std::string &scope, std::vector<SpoolsideAsset> &assets)
{
    const std::string key = api_key(parent);
    if (key.empty())
        return false;

    json list = json::array();
    if (!assets.empty()) {
        wxBusyCursor busy;
        Reply        reply;
        auto         http = Http::get(server_url() + "/api/assets");
        prepare(http, key, reply).timeout_max(30).perform_sync();
        if (reply.status == 401) {
            offer_new_key(parent, _L("Add to Spoolside library"));
            return false;
        }
        try {
            if (reply.ok())
                list = json::parse(reply.body);
        } catch (...) {}
        if (!reply.ok() || !list.is_array()) {
            show_error(parent, wxString::Format(_L("Could not load the Spoolside assets: %s"), reply.ok() ? _L("unexpected reply") : reply.reason()));
            return false;
        }
    }

    SpoolsideAddDialog dlg(parent, name, wxGetApp().app_config->get("spoolside_library"), assets, list);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    name   = dlg.get_name();
    scope  = dlg.get_scope();
    assets = dlg.get_assets();
    return true;
}

void spoolside_upload(wxWindow *parent, const boost::filesystem::path &plate_file, const std::string &name,
                      const std::string &scope, const std::vector<SpoolsideAsset> &assets)
{
    const wxString title = _L("Add to Spoolside library");
    const std::string key = api_key(parent);
    if (key.empty())
        return;

    std::vector<const SpoolsideAsset *> uploads;
    for (const SpoolsideAsset &a : assets)
        if (a.upload)
            uploads.push_back(&a);
    auto size_ok = [parent](const boost::filesystem::path &file, uintmax_t limit, const wxString &what, int mb) {
        boost::system::error_code ec;
        const uintmax_t           size = boost::filesystem::file_size(file, ec);
        if (ec || size == 0)
            show_error(parent, wxString::Format(_L("\"%s\" could not be written."), what));
        else if (size > limit)
            show_error(parent, wxString::Format(_L("\"%s\" is larger than Spoolside's %d MB upload limit."), what, mb));
        return !ec && size > 0 && size <= limit;
    };
    for (const SpoolsideAsset *a : uploads)
        if (!size_ok(a->stl, MAX_ASSET_SIZE, from_u8(a->name), 50))
            return;
    if (!size_ok(plate_file, MAX_UPLOAD_SIZE, from_u8(name), 95))
        return;

    const std::string base  = server_url();
    const int         steps = int(uploads.size()) + (uploads.empty() ? 1 : 2);
    std::atomic<int>  percent{0};
    std::atomic<bool> cancel{false};
    wxArrayString     done;      // what has been added or updated so far
    bool              failed = false;
    Reply             failure;   // the request that failed, if any
    wxString          entry_line;
    std::vector<std::string> asset_ids;
    {
        wxProgressDialog progress(title, wxString::Format(_L("Uploading %s"), from_u8(name)), 100, parent,
                                  wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
        int  step = 0;
        // Runs one request on a worker thread while the progress dialog stays responsive.
        auto run  = [&](Http &http, const wxString &message) {
            Reply reply;
            percent = 0;
            prepare(http, key, reply).on_progress([&](Http::Progress p, bool &cancel_request) {
                if (p.ultotal > 0)
                    percent = int(p.ulnow * 100 / p.ultotal);
                cancel_request = cancel;
            });
            std::atomic<bool> finished{false};
            std::thread       worker([&] {
                http.perform_sync();
                finished = true;
            });
            while (!finished) {
                if (!progress.Update((step * 100 + std::min(percent.load(), 99)) / steps, message))
                    cancel = true;
                wxMilliSleep(50);
            }
            worker.join();
            ++step;
            return reply;
        };

        // 1. The models. Sending an asset's existing source replaces that asset's STL.
        for (const SpoolsideAsset *a : uploads) {
            auto http = Http::post(base + "/api/assets");
            http.header("Content-Type", "application/octet-stream").header("x-asset", Http::url_encode(a->meta)).set_post_body(a->stl);
            Reply reply = run(http, wxString::Format(_L("Uploading %s"), from_u8(a->name)));
            std::string id, asset_name = a->name;
            try {
                auto j     = json::parse(reply.body);
                id         = str(j, "id");
                asset_name = j.value("name", asset_name);
            } catch (...) {}
            if (cancel || !reply.ok() || id.empty()) {
                failed  = true;
                failure = reply;
                break;
            }
            asset_ids.push_back(id);
            done.Add(wxString::Format(a->replaces ? _L("Updated asset \"%s\"") : _L("Added asset \"%s\""), from_u8(asset_name)));
        }

        // 2. The plate. Spoolside names a single-plate entry after x-file-name minus ".gcode.3mf", and replaces the
        // entry with that name in the same library. The suffix keeps a name that itself ends in ".3mf" intact.
        std::string entry_id;
        int         plate = -1;
        if (!failed) {
            auto http = Http::post(base + "/api/library");
            http.header("Content-Type", "application/octet-stream")
                .header("x-file-name", Http::url_encode(name + ".gcode.3mf"))
                .header("x-library-scope", scope)
                .set_post_body(plate_file);
            Reply       reply = run(http, wxString::Format(_L("Uploading %s"), from_u8(name)));
            std::string entry_name = name;
            bool        replaced   = false;
            try {
                auto j = json::parse(reply.body);
                if (j.is_array() && !j.empty() && j[0].is_object()) {
                    entry_id   = str(j[0], "id");
                    entry_name = j[0].value("name", entry_name);
                    replaced   = j[0].value("replaced", false);
                    const json &plates = j[0]["plates"];
                    if (plates.is_array() && !plates.empty() && plates[0].is_object())
                        plate = plates[0].value("index", -1);
                }
            } catch (...) {}
            if (cancel || !reply.ok() || entry_id.empty()) {
                failed  = true;
                failure = reply;
            } else
                entry_line = wxString::Format(replaced ? _L("Updated \"%s\" in the %s library.") : _L("Added \"%s\" to the %s library."),
                                              from_u8(entry_name), library_label(scope));
        }

        // 3. Link the plate to the models it prints (Spoolside records each asset's current STL hash).
        if (!failed && !asset_ids.empty()) {
            auto http = Http::patch(base + "/api/library/" + entry_id);
            http.header("Content-Type", "application/json").set_post_body(json{{"plate", plate}, {"assetIds", asset_ids}}.dump());
            Reply reply = run(http, _L("Linking the plate to its models"));
            if (!reply.ok()) {
                failed  = true;
                failure = reply;
            }
        }
    }

    if (failed) {
        if (cancel && done.empty() && entry_line.empty())
            return;
        BOOST_LOG_TRIVIAL(error) << "Spoolside upload failed: HTTP " << failure.status << ": " << into_u8(failure.reason());
        if (failure.status == 401) {
            offer_new_key(parent, title);
            return;
        }
        wxString msg = cancel && failure.ok() ? _L("The upload to Spoolside was cancelled.")
                              : wxString::Format(_L("Upload to Spoolside failed: %s"), failure.reason());
        if (!entry_line.empty() || !done.empty()) {
            msg += "\n\n" + _L("Done before that:");
            if (!entry_line.empty())
                msg += "\n" + entry_line;
            for (const wxString &line : done)
                msg += "\n" + line;
        }
        show_error(parent, msg);
        return;
    }

    wxString msg = entry_line;
    if (!done.empty()) {
        msg += "\n\n" + wxString::Format(_L("The plate is linked to %d models:"), int(done.size()));
        for (const wxString &line : done)
            msg += "\n" + line;
    }

    // Spoolside has no address for a single sliced print; open the library it went to.
    const std::string url = base + (scope == "personal" ? "/personal" : "/library/sliced");
    BOOST_LOG_TRIVIAL(info) << "Spoolside upload done: " << asset_ids.size() << " assets, " << url;

    MessageDialog dlg(parent, msg, _L("Spoolside"), wxYES_NO | wxICON_INFORMATION);
    dlg.SetButtonLabel(wxID_YES, _L("Open in Spoolside"));
    dlg.SetButtonLabel(wxID_NO, _L("OK"), true);
    if (dlg.ShowModal() == wxID_YES)
        wxLaunchDefaultBrowser(from_u8(url));
}

}} // namespace Slic3r::GUI
