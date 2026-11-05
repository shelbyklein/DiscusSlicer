#include "SpoolsideUpload.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GUI_Utils.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
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

// Runs one request on a worker thread while `progress` stays responsive, moving it from `from` to `to` percent.
// With `cancel`, the dialog's Cancel button aborts the request; without, the request always runs to the end.
Reply perform_with_progress(wxProgressDialog &progress, Http &http, const std::string &key, const wxString &message,
                            int from, int to, std::atomic<bool> *cancel)
{
    Reply            reply;
    std::atomic<int> percent{0};
    prepare(http, key, reply).on_progress([&](Http::Progress p, bool &cancel_request) {
        if (p.ultotal > 0)
            percent = int(p.ulnow * 100 / p.ultotal);
        cancel_request = cancel && *cancel;
    });
    std::atomic<bool> finished{false};
    std::thread       worker([&] {
        http.perform_sync();
        finished = true;
    });
    while (!finished) {
        if (!progress.Update(from + (to - from) * std::min(percent.load(), 99) / 100, message) && cancel)
            *cancel = true;
        wxMilliSleep(50);
    }
    worker.join();
    return reply;
}

// A printer Spoolside will start a print on now (its own rule: idle, finished or failed, and connected).
bool startable(const json &machine)
{
    const std::string state = str(machine, "state");
    return machine.value("connected", false) && !machine.value("stale", false) && (state == "Ready" || state == "Error");
}

// A spool a printer can feed from: an AMS slot (0-3) or the external spool.
constexpr int EXTERNAL_SPOOL = 254;
struct Spool
{
    int         slot;
    std::string type, colour;
};

std::vector<Spool> loaded_spools(const json &machine)
{
    std::vector<Spool> spools;
    if (machine.contains("trays") && machine["trays"].is_array())
        for (const json &t : machine["trays"])
            if (t.is_object() && !str(t, "type").empty())
                spools.push_back({t.value("slot", 0), str(t, "type"), str(t, "color")});
    if (machine.contains("external") && machine["external"].is_object() && !str(machine["external"], "type").empty())
        spools.push_back({EXTERNAL_SPOOL, str(machine["external"], "type"), str(machine["external"], "color")});
    return spools;
}

wxString spool_label(const Spool &s)
{
    const wxString what = from_u8(s.type) + (s.colour.empty() ? wxString() : " " + from_u8(s.colour));
    return s.slot == EXTERNAL_SPOOL ? wxString::Format(_L("External spool: %s"), what) : wxString::Format(_L("AMS slot %d: %s"), s.slot + 1, what);
}

// Spoolside's own default (mcp/server.mjs defaultMapping): the loaded spool of the same material family
// ("TPU-AMS" counts as TPU) with the nearest colour, else the nearest colour of any.
std::string material_family(std::string type)
{
    if (boost::algorithm::iends_with(type, "-AMS"))
        type.resize(type.size() - 4);
    return boost::algorithm::to_upper_copy(type.substr(0, type.find_first_of(" -")));
}

long colour_distance(const std::string &a, const std::string &b)
{
    auto channel = [](const std::string &hex, size_t i) {
        try {
            return hex.size() >= i + 2 ? std::stoi(hex.substr(i, 2), nullptr, 16) : 0;
        } catch (...) {
            return 0;
        }
    };
    long d = 0;
    for (size_t i : {1, 3, 5}) {
        const long v = channel(a, i) - channel(b, i);
        d += v * v;
    }
    return d;
}

int closest_spool(const SpoolsideFilament &f, const std::vector<Spool> &spools)
{
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass)
        for (size_t i = 0; i < spools.size(); ++i)
            if ((pass == 1 || material_family(spools[i].type) == material_family(f.type)) &&
                (best < 0 || colour_distance(spools[i].colour, f.colour) < colour_distance(spools[best].colour, f.colour)))
                best = int(i);
    return best;
}

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
    SpoolsideAddDialog(wxWindow *parent, const wxString &name, const std::string &scope, const std::vector<SpoolsideAsset> &objects, const json &assets,
                       const std::vector<SpoolsideFilament> &filaments, const json &machines)
        : DPIDialog(parent, wxID_ANY, _L("Add to Spoolside library"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_objects(objects)
        , m_filaments(filaments)
    {
        for (const json &m : machines)
            if (m.is_object() && !str(m, "id").empty())
                m_machines.push_back(m);
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
        m_library->Append(_L("None (test print, not saved)"));
        m_library->SetSelection(scope == "personal" ? 1 : 0);

        grid->Add(label(this, _L("Library name"), Label::Body_14), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_name, 1, wxEXPAND);
        grid->Add(label(this, _L("Library"), Label::Body_14), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_library, 1, wxEXPAND);

        // Print on: a printer Spoolside runs, or library only. The last printer used is chosen again.
        m_printer = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(field_width, -1), 0, nullptr, wxCB_READONLY);
        m_printer->Append(_L("Don't print (library only)"));
        const std::string last = wxGetApp().app_config->get("spoolside_printer");
        int               selected = 0;
        for (size_t i = 0; i < m_machines.size(); ++i) {
            const json &m = m_machines[i];
            m_printer->Append(from_u8(str(m, "name")) + "  (" + from_u8(str(m, "state")) + ")");
            if (str(m, "id") == last && startable(m))
                selected = int(i) + 1;
        }
        m_printer->SetSelection(selected);
        grid->Add(label(this, _L("Print on"), Label::Body_14), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_printer, 1, wxEXPAND);
        grid->AddSpacer(0);
        m_spools = new wxPanel(this);
        m_spools->SetBackgroundColour(*wxWHITE);
        m_spools->SetSizer(new wxFlexGridSizer(0, 2, FromDIP(6), FromDIP(8)));
        grid->Add(m_spools, 1, wxEXPAND);
        main->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));

        if (!m_objects.empty()) {
            m_models_label = label(this, _L("Models on this plate"), Label::Head_14);
            main->Add(m_models_label, 0, wxLEFT | wxRIGHT, FromDIP(20));
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
        m_add = add;
        add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { finish(); });
        m_printer->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
            build_spools();
            relayout();
        });
        m_library->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
            build_spools();
            relayout();
        });
        build_spools();
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
    // "" for a test print, which isn't added to the library.
    std::string                        get_scope() const { return test_print() ? "" : m_library->GetSelection() == 1 ? "personal" : "playcase"; }
    const std::vector<SpoolsideAsset> &get_assets() const { return m_objects; }
    const SpoolsidePrint              &get_print() const { return m_print; }

protected:
    void on_dpi_changed(const wxRect &) override { relayout(); }

private:
    bool test_print() const { return m_library->GetSelection() == 2; }

    const json *chosen_machine() const
    {
        const int sel = m_printer->GetSelection();
        return sel > 0 && sel <= int(m_machines.size()) ? &m_machines[sel - 1] : nullptr;
    }

    // One row per filament of the plate: the spool it prints from, preset to the closest loaded spool.
    // The external spool is offered only for single-filament plates (it can't be mixed with AMS slots).
    void build_spools()
    {
        m_spools->DestroyChildren();
        m_spool_choices.clear();
        m_spool_options.clear();
        const json *machine = chosen_machine();
        if (machine) {
            std::vector<Spool> spools = loaded_spools(*machine);
            if (m_filaments.size() != 1)
                spools.erase(std::remove_if(spools.begin(), spools.end(), [](const Spool &s) { return s.slot == EXTERNAL_SPOOL; }), spools.end());
            for (const SpoolsideFilament &f : m_filaments) {
                auto *title = new wxStaticText(m_spools, wxID_ANY,
                                               wxString::Format(_L("Filament %d: %s"), f.id, from_u8(f.type + (f.colour.empty() ? "" : " " + f.colour))));
                title->SetFont(Label::Body_12);
                auto *choice = new ComboBox(m_spools, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(220), -1), 0, nullptr, wxCB_READONLY);
                for (const Spool &s : spools)
                    choice->Append(spool_label(s));
                choice->SetSelection(closest_spool(f, spools));
                m_spools->GetSizer()->Add(title, 0, wxALIGN_CENTER_VERTICAL);
                m_spools->GetSizer()->Add(choice, 1, wxEXPAND);
                m_spool_choices.push_back(choice);
                m_spool_options.push_back(spools);
            }
            if (spools.empty())
                m_spools->GetSizer()->Add(new wxStaticText(m_spools, wxID_ANY, _L("No filament is loaded on this printer.")), 0);
        }
        m_spools->Show(machine != nullptr);
        m_add->SetLabel(test_print() ? _L("Print") : machine ? _L("Add and print") : _L("Add"));
    }

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
        // A test print sends only the plate: no library entry, so no models to upload or link.
        if (m_models_label)
            m_models_label->Show(!test_print());
        if (m_list)
            m_list->Show(!test_print());
        for (Row &row : m_rows) {
            const int action = row.action->GetSelection();
            const int asset  = row.asset->GetSelection();
            row.update_panel->Show(action == UPDATE);
            row.note->Show(asset >= 0 && from_dropbox(str(m_candidates[asset], "source")));
            row.new_panel->Show(action == ADD);
        }
        m_spools->Layout();
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
        if (test_print() && !chosen_machine()) {
            show_error(this, _L("Choose a printer under Print on for a test print."));
            return;
        }
        std::set<std::string> ids, sources;
        for (size_t i = 0; i < m_rows.size() && !test_print(); ++i) {
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

        m_print = {};
        if (const json *machine = chosen_machine()) {
            const wxString printer = from_u8(str(*machine, "name"));
            if (!startable(*machine)) {
                show_error(this, wxString::Format(_L("%s can't start a print now (%s). Choose another printer, or add the plate to the library only."),
                                                  printer, from_u8(lower(str(*machine, "state")))));
                return;
            }
            m_print.printer      = str(*machine, "id");
            m_print.printer_name = str(*machine, "name");
            int max_id           = 0;
            for (const SpoolsideFilament &f : m_filaments)
                max_id = std::max(max_id, f.id);
            m_print.slots.assign(max_id, -1);
            for (size_t i = 0; i < m_filaments.size(); ++i) {
                const int sel = m_spool_choices[i]->GetSelection();
                if (sel < 0) {
                    show_error(this, wxString::Format(_L("Choose the spool on %s for filament %d."), printer, m_filaments[i].id));
                    return;
                }
                const Spool &spool = m_spool_options[i][sel];
                if (spool.slot == EXTERNAL_SPOOL)
                    m_print.external = true;
                else
                    m_print.slots[m_filaments[i].id - 1] = spool.slot;
            }
            if (m_print.external)
                m_print.slots.clear();
        }
        wxGetApp().app_config->set("spoolside_printer", m_print.printer);
        EndModal(wxID_OK);
    }

    TextInput                  *m_name;
    ComboBox                   *m_library;
    wxStaticText               *m_models_label = nullptr;
    ComboBox                   *m_printer;
    wxPanel                    *m_spools;
    Button                     *m_add = nullptr;
    wxScrolledWindow           *m_list = nullptr;
    std::vector<Row>            m_rows;
    std::vector<json>           m_candidates;
    std::set<std::string>       m_sources;
    std::vector<SpoolsideAsset> m_objects;
    std::vector<SpoolsideFilament> m_filaments;
    std::vector<json>              m_machines;
    std::vector<ComboBox *>        m_spool_choices;
    std::vector<std::vector<Spool>> m_spool_options; // the spools listed in each of m_spool_choices
    SpoolsidePrint                 m_print;
};

bool spoolside_add_dialog(wxWindow *parent, wxString &name, std::string &scope, std::vector<SpoolsideAsset> &assets,
                          const std::vector<SpoolsideFilament> &filaments, SpoolsidePrint &print)
{
    const std::string key = api_key(parent);
    if (key.empty())
        return false;

    json list = json::array(), machines = json::array();
    {
        wxBusyCursor busy;
        if (!assets.empty()) {
            Reply reply;
            auto  http = Http::get(server_url() + "/api/assets");
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
        // The printers, for "Print on". Without them the plate can still go to the library.
        Reply reply;
        auto  http = Http::get(server_url() + "/api/workspace");
        prepare(http, key, reply).timeout_max(30).perform_sync();
        if (reply.status == 401) {
            offer_new_key(parent, _L("Add to Spoolside library"));
            return false;
        }
        try {
            if (reply.ok()) {
                json workspace = json::parse(reply.body);
                if (workspace.is_object() && workspace.contains("machines") && workspace["machines"].is_array())
                    machines = workspace["machines"];
            }
        } catch (...) {}
        if (machines.empty())
            BOOST_LOG_TRIVIAL(warning) << "Spoolside printers unavailable: " << into_u8(reply.reason());
    }

    SpoolsideAddDialog dlg(parent, name, wxGetApp().app_config->get("spoolside_library"), assets, list, filaments, machines);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    name   = dlg.get_name();
    scope  = dlg.get_scope();
    assets = dlg.get_assets();
    print  = dlg.get_print();
    return true;
}

namespace {

// Starts library entry `entry_id`, plate `plate`, on `print.printer` through Spoolside, which sends the file over
// the LAN. Unless `bed_clear`, Spoolside checks the bed with the printer's camera first and refuses (409, "bed")
// if it sees something. The request can't be cancelled halfway: Spoolside would go on sending the file.
Reply start_print(wxWindow *parent, const std::string &key, const std::string &entry_id, int plate, const SpoolsidePrint &print, bool bed_clear)
{
    wxProgressDialog progress(_L("Print on Spoolside"), wxString::Format(_L("Starting on %s"), from_u8(print.printer_name)), 100, parent,
                              wxPD_APP_MODAL | wxPD_AUTO_HIDE);
    auto http = Http::post(server_url() + "/api/printers/" + Http::url_encode(print.printer) + "/print");
    http.timeout_max(600)
        .header("Content-Type", "application/json")
        .set_post_body(json{{"fileId", entry_id},
                            {"plate", plate},
                            {"useAms", !print.external},
                            {"amsMapping", print.slots},
                            {"bedLevelling", true},
                            {"bedClear", bed_clear}}
                           .dump());
    const wxString message = bed_clear ? wxString::Format(_L("Sending to %s"), from_u8(print.printer_name))
                                       : wxString::Format(_L("Checking the bed and sending to %s"), from_u8(print.printer_name));
    return perform_with_progress(progress, http, key, message, 0, 100, nullptr);
}

// A test print: sends `plate_file` straight to `print.printer` through Spoolside, which starts it without adding it
// to the library (it keeps the file only while sending). Same bed check as start_print(); the plate is the file's only one.
Reply start_test_print(wxWindow *parent, const std::string &key, const boost::filesystem::path &plate_file, const std::string &name,
                       const SpoolsidePrint &print, bool bed_clear)
{
    wxProgressDialog progress(_L("Test print on Spoolside"), wxString::Format(_L("Starting on %s"), from_u8(print.printer_name)), 100, parent,
                              wxPD_APP_MODAL | wxPD_AUTO_HIDE);
    auto http = Http::post(server_url() + "/api/printers/" + Http::url_encode(print.printer) + "/print-file");
    http.timeout_max(600)
        .header("Content-Type", "application/octet-stream")
        .header("x-file-name", Http::url_encode(name + ".gcode.3mf"))
        .header("x-print", Http::url_encode(json{{"useAms", !print.external},
                                                  {"amsMapping", print.slots},
                                                  {"bedLevelling", true},
                                                  {"bedClear", bed_clear}}
                                                 .dump()))
        .set_post_body(plate_file);
    const wxString message = bed_clear ? wxString::Format(_L("Sending to %s"), from_u8(print.printer_name))
                                       : wxString::Format(_L("Checking the bed and sending to %s"), from_u8(print.printer_name));
    return perform_with_progress(progress, http, key, message, 0, 100, nullptr);
}

bool bed_refused(const Reply &reply)
{
    try {
        auto j = json::parse(reply.body);
        return reply.status == 409 && j.is_object() && j.value("bed", false);
    } catch (...) {
        return false;
    }
}

wxString started_line(const SpoolsidePrint &print)
{
    if (print.external)
        return wxString::Format(_L("Started on %s from the external spool."), from_u8(print.printer_name));
    wxString slots;
    for (int s : print.slots)
        if (s >= 0)
            slots += (slots.empty() ? "" : ", ") + wxString::Format("%d", s + 1);
    return wxString::Format(_L("Started on %s (AMS slot %s)."), from_u8(print.printer_name), slots);
}

} // namespace

void spoolside_upload(wxWindow *parent, const boost::filesystem::path &plate_file, const std::string &name,
                      const std::string &scope, const std::vector<SpoolsideAsset> &assets, const SpoolsidePrint &print)
{
    const wxString title = _L("Add to Spoolside library");
    const std::string key = api_key(parent);
    if (key.empty())
        return;

    // A test print: only the plate, straight to the printer, nothing kept in the library.
    if (scope.empty()) {
        boost::system::error_code ec;
        const uintmax_t           size = boost::filesystem::file_size(plate_file, ec);
        if (ec || size == 0 || size > MAX_UPLOAD_SIZE) {
            show_error(parent, ec || size == 0 ? wxString::Format(_L("\"%s\" could not be written."), from_u8(name))
                                               : wxString::Format(_L("\"%s\" is larger than Spoolside's %d MB upload limit."), from_u8(name), 95));
            return;
        }
        Reply reply = start_test_print(parent, key, plate_file, name, print, false);
        if (bed_refused(reply)) {
            MessageDialog ask(parent,
                              wxString::Format(_L("%s: %s\n\nIf the bed is clear, the print can start anyway."), from_u8(print.printer_name), reply.reason()),
                              _L("Test print on Spoolside"), wxYES_NO | wxICON_WARNING);
            ask.SetButtonLabel(wxID_YES, _L("Bed is clear, print"));
            ask.SetButtonLabel(wxID_NO, _L("Don't print"), true);
            if (ask.ShowModal() != wxID_YES)
                return;
            reply = start_test_print(parent, key, plate_file, name, print, true);
        }
        if (reply.status == 401) {
            offer_new_key(parent, _L("Test print on Spoolside"));
            return;
        }
        if (!reply.ok()) {
            BOOST_LOG_TRIVIAL(error) << "Spoolside test print failed: HTTP " << reply.status << ": " << into_u8(reply.reason());
            show_error(parent, wxString::Format(_L("Not printed on %s: %s"), from_u8(print.printer_name), reply.reason()));
            return;
        }
        MessageDialog dlg(parent, started_line(print) + "\n\n" + _L("It's a test print, so it wasn't added to the library."), _L("Spoolside"),
                          wxYES_NO | wxICON_INFORMATION);
        dlg.SetButtonLabel(wxID_YES, _L("Open in Spoolside"));
        dlg.SetButtonLabel(wxID_NO, _L("OK"), true);
        if (dlg.ShowModal() == wxID_YES)
            wxLaunchDefaultBrowser(from_u8(server_url() + "/printers"));
        return;
    }

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
    std::atomic<bool> cancel{false};
    wxArrayString     done;      // what has been added or updated so far
    bool              failed = false;
    Reply             failure;   // the request that failed, if any
    wxString          entry_line;
    std::vector<std::string> asset_ids;
    std::string              entry_id;
    int                      plate = -1;
    {
        wxProgressDialog progress(title, wxString::Format(_L("Uploading %s"), from_u8(name)), 100, parent,
                                  wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
        int  step = 0;
        auto run  = [&](Http &http, const wxString &message) {
            Reply reply = perform_with_progress(progress, http, key, message, step * 100 / steps, (step + 1) * 100 / steps, &cancel);
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

    // 4. Start it. If the camera sees something on the bed, ask before starting anyway.
    bool print_failed = false;
    if (!print.printer.empty()) {
        Reply reply = plate < 0 ? Reply{0, "", "Spoolside did not report the plate"} : start_print(parent, key, entry_id, plate, print, false);
        if (bed_refused(reply)) {
            MessageDialog ask(parent,
                              wxString::Format(_L("%s: %s\n\nIf the bed is clear, the print can start anyway."), from_u8(print.printer_name), reply.reason()),
                              _L("Print on Spoolside"), wxYES_NO | wxICON_WARNING);
            ask.SetButtonLabel(wxID_YES, _L("Bed is clear, print"));
            ask.SetButtonLabel(wxID_NO, _L("Don't print"), true);
            if (ask.ShowModal() == wxID_YES)
                reply = start_print(parent, key, entry_id, plate, print, true);
            else
                reply = Reply{0, "", into_u8(_L("not started, the bed wasn't clear"))};
        }
        print_failed = !reply.ok();
        if (print_failed) {
            BOOST_LOG_TRIVIAL(error) << "Spoolside print failed: HTTP " << reply.status << ": " << into_u8(reply.reason());
            msg += "\n\n" + wxString::Format(_L("Not printed on %s: %s"), from_u8(print.printer_name), reply.reason());
        } else
            msg += "\n\n" + started_line(print);
    }

    // Spoolside has no address for a single sliced print; open the printers page after a print, else the library.
    const std::string url = base + (!print.printer.empty() && !print_failed ? "/printers" : scope == "personal" ? "/personal" : "/library/sliced");
    BOOST_LOG_TRIVIAL(info) << "Spoolside upload done: " << asset_ids.size() << " assets, " << url;

    MessageDialog dlg(parent, msg, _L("Spoolside"), wxYES_NO | (print_failed ? wxICON_WARNING : wxICON_INFORMATION));
    dlg.SetButtonLabel(wxID_YES, _L("Open in Spoolside"));
    dlg.SetButtonLabel(wxID_NO, _L("OK"), true);
    if (dlg.ShowModal() == wxID_YES)
        wxLaunchDefaultBrowser(from_u8(url));
}

}} // namespace Slic3r::GUI
