#include "backend.hpp"
#include "session.hpp"
#include "target_pty.hpp"

#include <gtksourceview/gtksource.h>
#include <gtkmm.h>

#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace {

const std::regex kFrameLldb(R"(^\s*\*? *frame #(\d+):\s+(\S+)\s+(.*)$)");
const std::regex kFrameGdb(R"(^\s*\*? *#(\d+)\s+(\S+)\s+(.*)$)");
const std::regex kLoc(R"(at ([^:\s]+):(\d+))");

std::vector<std::string> clean_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = line;
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (t.empty() || t == "(lldb)" || t == "(gdb)" || t.rfind("(lldb)", 0) == 0 ||
            t.rfind("command source", 0) == 0)
            continue;
        out.push_back(line);
    }
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void apply_source_scheme(GtkSourceBuffer* buf, bool light) {
    if (!buf) return;
    auto* sm = gtk_source_style_scheme_manager_get_default();
    const char* ids[] = {light ? "Adwaita" : "Adwaita-dark",
                         light ? "classic" : "oblivion",
                         light ? "solarized-light" : "solarized-dark",
                         "tango"};
    GtkSourceStyleScheme* scheme = nullptr;
    for (auto id : ids) {
        scheme = gtk_source_style_scheme_manager_get_scheme(sm, id);
        if (scheme) break;
    }
    if (scheme) gtk_source_buffer_set_style_scheme(buf, scheme);
}

Gtk::ScrolledWindow* make_source_page(const std::string& path, const std::string& text,
                                      GtkSourceView** out_view, GtkSourceBuffer** out_buf,
                                      bool light = false) {
    auto* view = GTK_SOURCE_VIEW(gtk_source_view_new());
    auto* buf = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)));
    gtk_source_view_set_show_line_numbers(view, TRUE);
    gtk_source_view_set_highlight_current_line(view, TRUE);
    gtk_source_view_set_show_line_marks(view, TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    auto* bp_attr = gtk_source_mark_attributes_new();
    GdkRGBA bp_rgba;
    gdk_rgba_parse(&bp_rgba, "#e74c3c");
    gtk_source_mark_attributes_set_background(bp_attr, &bp_rgba);
    gtk_source_mark_attributes_set_icon_name(bp_attr, "media-record-symbolic");
    gtk_source_view_set_mark_attributes(view, "breakpoint", bp_attr, 10);
    auto* cur_attr = gtk_source_mark_attributes_new();
    GdkRGBA cur_rgba;
    gdk_rgba_parse(&cur_rgba, "#f1c40f");
    gtk_source_mark_attributes_set_background(cur_attr, &cur_rgba);
    gtk_source_view_set_mark_attributes(view, "current", cur_attr, 20);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buf), text.c_str(), -1);
    auto* lm = gtk_source_language_manager_get_default();
    auto* lang = gtk_source_language_manager_guess_language(lm, path.c_str(), nullptr);
    if (lang) gtk_source_buffer_set_language(buf, lang);
    apply_source_scheme(buf, light);
    gtk_widget_add_css_class(GTK_WIDGET(view), "source-editor");
    if (out_view) *out_view = view;
    if (out_buf) *out_buf = buf;
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_expand(true);
    gtk_scrolled_window_set_child(scroll->gobj(), GTK_WIDGET(view));
    return scroll;
}

}  // namespace

class DebuggerApp : public Gtk::Application {
public:
    explicit DebuggerApp(Config cfg)
        : Gtk::Application("dev.local.sdbg", Gio::Application::Flags::DEFAULT_FLAGS),
          cfg_(std::move(cfg)) {
        backend_ = cfg_.debugger == "gdb" ? make_gdb(cfg_.debugger_path)
                                          : make_lldb(cfg_.debugger_path);
    }

protected:
    void on_activate() override {
        window_ = std::make_unique<Gtk::ApplicationWindow>();
        add_window(*window_);
        window_->set_title("sdbg");
        window_->set_resizable(true);
        window_->set_decorated(true);
        window_->set_size_request(480, 320);
        window_->set_default_size(cfg_.win_w > 400 ? cfg_.win_w : 1280,
                                  cfg_.win_h > 300 ? cfg_.win_h : 820);
        window_->signal_close_request().connect(
            [this]() {
                if (session_) {
                    try {
                        session_->command("quit", 1.5);
                    } catch (...) {
                    }
                    session_->stop();
                }
                if (target_pty_) target_pty_->stop();
                cfg_.win_w = window_->get_width();
                cfg_.win_h = window_->get_height();
                persist();
                return false;
            },
            false);

        css_ = Gtk::CssProvider::create();
        Gtk::StyleProvider::add_provider_for_display(
            Gdk::Display::get_default(), css_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        auto root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        root->set_hexpand(true);
        root->set_vexpand(true);
        window_->set_child(*root);

        auto header = Gtk::make_managed<Gtk::HeaderBar>();
        window_->set_titlebar(*header);
        auto title = Gtk::make_managed<Gtk::Label>("sdbg");
        header->set_title_widget(*title);

        engine_drop_.set_model(Gtk::StringList::create({"lldb", "gdb"}));
        engine_drop_.set_selected(backend_.name == "gdb" ? 1 : 0);
        engine_drop_.property_selected().signal_changed().connect([this] { on_engine_changed(); });
        auto ebox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        ebox->append(*Gtk::make_managed<Gtk::Label>("Engine"));
        ebox->append(engine_drop_);
        theme_drop_.set_model(Gtk::StringList::create({"dark", "light"}));
        theme_drop_.set_selected(cfg_.theme == "light" ? 1 : 0);
        theme_drop_.property_selected().signal_changed().connect([this] { apply_theme(); });
        font_size_.set_range(9, 22);
        font_size_.set_value(cfg_.font_size > 0 ? cfg_.font_size : 12);
        font_size_.set_digits(0);
        font_size_.signal_value_changed().connect([this] { apply_theme(); });
        ebox->append(*Gtk::make_managed<Gtk::Label>("Theme"));
        ebox->append(theme_drop_);
        ebox->append(*Gtk::make_managed<Gtk::Label>("Font"));
        ebox->append(font_size_);
        header->pack_end(*ebox);
        apply_theme();

        auto toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        toolbar->set_margin(8);
        root->append(*toolbar);
        toolbar->append(*Gtk::make_managed<Gtk::Label>("Target"));
        exe_.set_hexpand(true);
        exe_.set_placeholder_text("Target executable or core");
        exe_.set_text(cfg_.target);
        toolbar->append(exe_);
        auto browse = Gtk::make_managed<Gtk::Button>("Browse…");
        browse->signal_clicked().connect([this] { browse_exe(); });
        auto load = Gtk::make_managed<Gtk::Button>("Load target");
        load->signal_clicked().connect([this] { load_target(); });
        auto apply = Gtk::make_managed<Gtk::Button>("Apply params");
        apply->signal_clicked().connect([this] { apply_params(); });
        toolbar->append(*browse);
        toolbar->append(*load);
        toolbar->append(*apply);
        attach_pid_.set_placeholder_text("PID");
        attach_pid_.set_width_chars(8);
        if (cfg_.attach) attach_pid_.set_text(std::to_string(cfg_.attach));
        auto attach_btn = Gtk::make_managed<Gtk::Button>("Attach");
        attach_btn->signal_clicked().connect([this] { attach_process(); });
        toolbar->append(attach_pid_);
        toolbar->append(*attach_btn);
        auto cfg_btn = Gtk::make_managed<Gtk::Button>("Configure…");
        cfg_btn->signal_clicked().connect([this] { show_config_dialog(); });
        toolbar->append(*cfg_btn);
        auto hdr_cfg = Gtk::make_managed<Gtk::Button>("Configure");
        hdr_cfg->signal_clicked().connect([this] { show_config_dialog(); });
        header->pack_start(*hdr_cfg);

        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(8);
        grid->set_row_spacing(6);
        grid->set_margin(12);

        args_.set_placeholder_text("argv after argv0");
        args_.set_text(cfg_.args);
        cwd_.set_placeholder_text("Working directory");
        cwd_.set_text(cfg_.cwd);
        env_.set_placeholder_text("KEY=value");
        env_.set_text(cfg_.env);
        extra_.set_monospace(true);
        extra_.get_buffer()->set_text(cfg_.extra);
        stop_at_entry_.set_label("Stop at entry (before main)");
        stop_at_entry_.set_active(cfg_.stop_at_entry);

        auto add_row = [&](int row, const char* lab, Gtk::Widget& w) {
            auto* l = Gtk::make_managed<Gtk::Label>(lab);
            l->set_xalign(0);
            w.set_hexpand(true);
            grid->attach(*l, 0, row);
            grid->attach(w, 1, row);
        };
        add_row(0, "Arguments", args_);
        add_row(1, "Working dir", cwd_);
        add_row(2, "Environment", env_);
        extra_scroll_.set_child(extra_);
        extra_scroll_.set_min_content_height(52);
        add_row(3, "Setup commands", extra_scroll_);
        stdin_path_.set_placeholder_text("redirect stdin from file");
        stdin_path_.set_text(cfg_.stdin_path);
        stdout_path_.set_placeholder_text("redirect stdout to file");
        stdout_path_.set_text(cfg_.stdout_path);
        stderr_path_.set_placeholder_text("redirect stderr to file");
        stderr_path_.set_text(cfg_.stderr_path);
        tty_path_.set_placeholder_text("/dev/pts/N");
        tty_path_.set_text(cfg_.tty);
        add_row(4, "Stdin file", stdin_path_);
        add_row(5, "Stdout file", stdout_path_);
        add_row(6, "Stderr file", stderr_path_);
        add_row(7, "TTY", tty_path_);
        remote_.set_placeholder_text("host:port  (gdbserver)");
        remote_.set_text(cfg_.remote);
        add_row(8, "Remote", remote_);
        follow_fork_.set_model(Gtk::StringList::create({"parent", "child", "ask", "both"}));
        {
            int sel = 0;
            if (cfg_.follow_fork == "child") sel = 1;
            else if (cfg_.follow_fork == "ask") sel = 2;
            else if (cfg_.follow_fork == "both") sel = 3;
            follow_fork_.set_selected(sel);
        }
        add_row(9, "Follow fork", follow_fork_);
        detach_fork_.set_label("Detach other fork");
        detach_fork_.set_active(cfg_.detach_on_fork);
        grid->attach(detach_fork_, 1, 10);
        tgt_console_on_.set_label("Enable target console (inferior PTY)");
        tgt_console_on_.set_active(cfg_.target_console);
        tgt_bind_in_.set_label("Bind stdin");
        tgt_bind_in_.set_active(cfg_.target_bind_stdin);
        tgt_bind_out_.set_label("Bind stdout");
        tgt_bind_out_.set_active(cfg_.target_bind_stdout);
        tgt_bind_err_.set_label("Bind stderr");
        tgt_bind_err_.set_active(cfg_.target_bind_stderr);
        tgt_echo_.set_label("Local echo");
        tgt_echo_.set_active(cfg_.target_echo);
        tgt_crlf_.set_label("Send CR+LF");
        tgt_crlf_.set_active(cfg_.target_crlf);
        auto tbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        tbox->append(tgt_console_on_);
        tbox->append(tgt_bind_in_);
        tbox->append(tgt_bind_out_);
        tbox->append(tgt_bind_err_);
        tbox->append(tgt_echo_);
        tbox->append(tgt_crlf_);
        grid->attach(*tbox, 0, 11, 2);
        core_path_.set_placeholder_text("core dump path");
        core_path_.set_text(cfg_.core);
        add_row(12, "Core dump", core_path_);
        source_map_.set_monospace(true);
        source_map_.get_buffer()->set_text(cfg_.source_map);
        source_map_scroll_.set_child(source_map_);
        source_map_scroll_.set_min_content_height(48);
        add_row(13, "Source map", source_map_scroll_);
        grid->attach(stop_at_entry_, 1, 14);

        settings_win_ = std::make_unique<Gtk::Window>();
        settings_win_->set_title("Target configuration");
        settings_win_->set_transient_for(*window_);
        settings_win_->set_hide_on_close(true);
        settings_win_->set_resizable(true);
        settings_win_->set_default_size(720, 640);
        auto sbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto sscroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        sscroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sscroll->set_child(*grid);
        sscroll->set_vexpand(true);
        sbox->append(*sscroll);
        auto sbtn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        sbtn->set_margin(10);
        auto apply_dlg = Gtk::make_managed<Gtk::Button>("Apply");
        apply_dlg->signal_clicked().connect([this] { apply_params(); });
        auto close_dlg = Gtk::make_managed<Gtk::Button>("Close");
        close_dlg->signal_clicked().connect([this] { settings_win_->set_visible(false); });
        sbtn->append(*apply_dlg);
        sbtn->append(*close_dlg);
        sbox->append(*sbtn);
        settings_win_->set_child(*sbox);

        auto controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        controls->set_margin_start(8);
        controls->set_margin_end(8);
        root->append(*controls);
        auto mk = [&](const char* lab, std::function<void()> cb) {
            auto* b = Gtk::make_managed<Gtk::Button>(lab);
            b->signal_clicked().connect(std::move(cb));
            controls->append(*b);
            return b;
        };
        mk("Start debugger", [this] { start_session(); });
        mk("Run target", [this] { run_target(); });
        mk("Reload target", [this] { reload_target(); });
        mk("Load core", [this] { load_core(); });
        mk("Connect remote", [this] { connect_remote(); });
        mk("Continue", [this] { dbg_async(backend_.continue_cmd); });
        mk("Pause", [this] {
            if (session_) session_->interrupt();
            Glib::signal_timeout().connect_once([this] { refresh_state(); }, 250);
        });
        mk("Step in", [this] { dbg(backend_.step_in); });
        mk("Step over", [this] { dbg(backend_.step_over); });
        mk("Step out", [this] { dbg(backend_.step_out); });
        mk("Kill", [this] { dbg(backend_.kill_cmd); });
        bp_.set_placeholder_text("Breakpoint: main or file.c:12");
        bp_.set_hexpand(true);
        controls->append(bp_);
        bp_cond_.set_placeholder_text("condition e.g. i==3");
        bp_thread_.set_placeholder_text("thread #");
        bp_thread_.set_width_chars(8);
        controls->append(bp_cond_);
        controls->append(bp_thread_);
        mk("Add BP", [this] {
            auto spec = std::string(bp_.get_text());
            if (!spec.empty())
                dbg(backend_.breakpoint(spec, bp_cond_.get_text(), bp_thread_.get_text()));
        });
        mk("Clear BPs", [this] { dbg(backend_.delete_breaks); });
        mk("Delete BP", [this] { delete_selected_bp(); });
        mk("Toggle BP", [this] { toggle_selected_bp(); });

        status_.set_xalign(0);
        status_.set_markup("<span foreground='#9aa0a6'>Idle</span>");
        status_.set_margin_start(10);
        root->append(status_);

        auto panes = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
        panes->set_expand(true);
        panes->set_hexpand(true);
        panes->set_vexpand(true);
        panes->set_wide_handle(true);
        root->append(*panes);

        auto source_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        source_box->set_hexpand(true);
        source_box->set_vexpand(true);
        auto src_header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto sl = Gtk::make_managed<Gtk::Label>("SOURCE");
        sl->set_xalign(0);
        sl->set_hexpand(true);
        src_header->append(*sl);
        auto openf = Gtk::make_managed<Gtk::Button>("Open file…");
        openf->signal_clicked().connect([this] { browse_source(); });
        auto closet = Gtk::make_managed<Gtk::Button>("Close tab");
        closet->signal_clicked().connect([this] { close_current_tab(); });
        src_header->append(*openf);
        src_header->append(*closet);
        auto reloadf = Gtk::make_managed<Gtk::Button>("Reload file");
        reloadf->signal_clicked().connect([this] { reload_current_file(); });
        src_header->append(*reloadf);
        find_entry_.set_placeholder_text("Find");
        find_entry_.set_width_chars(16);
        find_entry_.signal_activate().connect([this] { find_in_file(true); });
        src_header->append(find_entry_);
        auto fnext = Gtk::make_managed<Gtk::Button>("Next");
        fnext->signal_clicked().connect([this] { find_in_file(true); });
        auto fprev = Gtk::make_managed<Gtk::Button>("Prev");
        fprev->signal_clicked().connect([this] { find_in_file(false); });
        src_header->append(*fprev);
        src_header->append(*fnext);
        goto_entry_.set_placeholder_text("Line");
        goto_entry_.set_width_chars(5);
        goto_entry_.signal_activate().connect([this] { goto_line_ui(); });
        src_header->append(goto_entry_);
        auto gbtn = Gtk::make_managed<Gtk::Button>("Go");
        gbtn->signal_clicked().connect([this] { goto_line_ui(); });
        src_header->append(*gbtn);
        source_box->append(*src_header);
        notebook_.set_scrollable(true);
        notebook_.set_expand(true);
        source_box->append(notebook_);
        panes->set_start_child(*source_box);
        panes->set_resize_start_child(true);

        auto right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        right->set_size_request(220, -1);
        right->set_hexpand(true);
        right->set_vexpand(true);
        panes->set_end_child(*right);
        panes->set_resize_start_child(true);
        panes->set_resize_end_child(true);
        panes->set_shrink_start_child(true);
        panes->set_shrink_end_child(true);
        panes->set_position(cfg_.win_w > 800 ? cfg_.win_w * 2 / 3 : 720);

        auto add_list = [&](const char* title, Gtk::ListBox& list) {
            auto* lab = Gtk::make_managed<Gtk::Label>(title);
            lab->set_xalign(0);
            auto* sc = Gtk::make_managed<Gtk::ScrolledWindow>();
            sc->set_min_content_height(110);
            sc->set_vexpand(true);
            sc->set_child(list);
            right->append(*lab);
            right->append(*sc);
        };
        add_list("THREADS", threads_);
        // markers below patched after all add_list calls
        add_list("CALL STACK", frames_);
        add_list("VARIABLES", vars_);
        hex_locals_.set_label("Hex locals");
        hex_locals_.set_active(cfg_.hex_locals);
        hex_locals_.signal_toggled().connect([this] {
            cfg_.hex_locals = hex_locals_.get_active();
            if (session_ && session_->alive()) dbg(backend_.radix_cmd(cfg_.hex_locals));
        });
        right->append(hex_locals_);
        add_list("REGISTERS", regs_);
        auto rrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        reg_name_.set_placeholder_text("reg");
        reg_name_.set_width_chars(8);
        reg_val_.set_placeholder_text("value");
        reg_val_.set_hexpand(true);
        auto rset = Gtk::make_managed<Gtk::Button>("Set reg");
        rset->signal_clicked().connect([this] {
            auto n = std::string(reg_name_.get_text());
            auto v = std::string(reg_val_.get_text());
            if (!n.empty() && !v.empty()) dbg(backend_.write_reg_cmd(n, v));
        });
        rrow->append(reg_name_);
        rrow->append(reg_val_);
        rrow->append(*rset);
        right->append(*rrow);
        add_list("BREAKPOINTS", bps_);
        auto wlab = Gtk::make_managed<Gtk::Label>("WATCHES");
        wlab->set_xalign(0);
        right->append(*wlab);
        auto wrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        watch_entry_.set_placeholder_text("expression");
        watch_entry_.set_hexpand(true);
        watch_entry_.signal_activate().connect([this] { add_watch(); });
        auto wadd = Gtk::make_managed<Gtk::Button>("Add");
        wadd->signal_clicked().connect([this] { add_watch(); });
        auto wdel = Gtk::make_managed<Gtk::Button>("Remove");
        wdel->signal_clicked().connect([this] { remove_watch(); });
        wrow->append(watch_entry_);
        wrow->append(*wadd);
        wrow->append(*wdel);
        right->append(*wrow);
        auto wsc = Gtk::make_managed<Gtk::ScrolledWindow>();
        wsc->set_min_content_height(90);
        wsc->set_vexpand(true);
        wsc->set_child(watches_);
        right->append(*wsc);

        auto mlab = Gtk::make_managed<Gtk::Label>("MEMORY");
        mlab->set_xalign(0);
        right->append(*mlab);
        auto mrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        mem_addr_.set_placeholder_text("address");
        mem_addr_.set_hexpand(true);
        mem_count_.set_placeholder_text("64");
        mem_count_.set_width_chars(5);
        mem_count_.set_text("64");
        auto mgo = Gtk::make_managed<Gtk::Button>("Read");
        mgo->signal_clicked().connect([this] { read_memory(); });
        mrow->append(mem_addr_);
        mrow->append(mem_count_);
        mrow->append(*mgo);
        right->append(*mrow);
        mem_view_.set_editable(false);
        mem_view_.set_monospace(true);
        mem_view_.add_css_class("mono-pane");
        mem_scroll_.set_child(mem_view_);
        mem_scroll_.set_min_content_height(100);
        right->append(mem_scroll_);

        frames_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            if (row) dbg(backend_.select_frame(row->get_index()));
        });
        vars_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            if (!row) return;
            if (auto* lab = dynamic_cast<Gtk::Label*>(row->get_child())) {
                auto t = std::string(lab->get_text());
                auto eq = t.find('=');
                auto name = eq == std::string::npos ? t : t.substr(0, eq);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                auto b = name.find_first_not_of(" \t*");
                if (b != std::string::npos) name = name.substr(b);
                if (!name.empty()) dbg(backend_.expand_local_cmd(name));
            }
        });
        regs_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            if (!row) return;
            if (auto* lab = dynamic_cast<Gtk::Label*>(row->get_child())) {
                auto t = std::string(lab->get_text());
                auto sp = t.find_first_of(" \t");
                reg_name_.set_text(sp == std::string::npos ? t : t.substr(0, sp));
            }
        });
        threads_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            if (!row) return;
            if (auto* lab = dynamic_cast<Gtk::Label*>(row->get_child())) {
                auto id = first_number(lab->get_text());
                if (!id.empty()) dbg(backend_.select_thread(std::stoi(id)));
            }
        });

        auto consoles = Gtk::make_managed<Gtk::Notebook>();
        consoles->set_scrollable(true);

        auto dbg_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        console_lab_.set_text("DEBUGGER CONSOLE");
        console_lab_.set_xalign(0);
        dbg_page->append(console_lab_);
        console_.set_editable(false);
        console_.set_monospace(true);
        console_.add_css_class("mono-pane");
        console_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        console_scroll_.set_child(console_);
        console_scroll_.set_min_content_height(160);
        console_scroll_.set_vexpand(true);
        dbg_page->append(console_scroll_);
        auto cmdrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        cmdrow->set_margin(6);
        cmd_.set_placeholder_text("Debugger command…");
        cmd_.set_hexpand(true);
        cmd_.signal_activate().connect([this] { send_console(); });
        auto cmd_keys = Gtk::EventControllerKey::create();
        cmd_keys->signal_key_pressed().connect(
            [this](guint key, guint, Gdk::ModifierType) {
                if (key == GDK_KEY_Up) {
                    history_up();
                    return true;
                }
                if (key == GDK_KEY_Down) {
                    history_down();
                    return true;
                }
                return false;
            },
            false);
        cmd_.add_controller(cmd_keys);
        auto send = Gtk::make_managed<Gtk::Button>("Send");
        send->signal_clicked().connect([this] { send_console(); });
        cmdrow->append(cmd_);
        cmdrow->append(*send);
        dbg_page->append(*cmdrow);
        consoles->append_page(*dbg_page, *Gtk::make_managed<Gtk::Label>("Debugger"));

        auto tgt_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto tgt_hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        target_lab_.set_text("TARGET CONSOLE");
        target_lab_.set_xalign(0);
        target_lab_.set_hexpand(true);
        tgt_hdr->append(target_lab_);
        auto tclear = Gtk::make_managed<Gtk::Button>("Clear");
        tclear->signal_clicked().connect([this] { target_view_.get_buffer()->set_text(""); });
        tgt_hdr->append(*tclear);
        tgt_page->append(*tgt_hdr);
        target_view_.set_editable(false);
        target_view_.set_monospace(true);
        target_view_.add_css_class("mono-pane");
        target_view_.set_wrap_mode(Gtk::WrapMode::CHAR);
        target_scroll_.set_child(target_view_);
        target_scroll_.set_min_content_height(160);
        target_scroll_.set_vexpand(true);
        tgt_page->append(target_scroll_);
        auto tin = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        tin->set_margin(6);
        target_in_.set_placeholder_text("Input to inferior…");
        target_in_.set_hexpand(true);
        target_in_.signal_activate().connect([this] { send_target_input(); });
        auto tsend = Gtk::make_managed<Gtk::Button>("Send to target");
        tsend->signal_clicked().connect([this] { send_target_input(); });
        tin->append(target_in_);
        tin->append(*tsend);
        tgt_page->append(*tin);
        consoles->append_page(*tgt_page, *Gtk::make_managed<Gtk::Label>("Target"));
        consoles->set_vexpand(true);
        root->append(*consoles);

        target_dispatch_.connect([this] {
            std::string chunk;
            {
                std::lock_guard<std::mutex> g(tgt_mu_);
                chunk.swap(tgt_queue_);
            }
            auto buf = target_view_.get_buffer();
            buf->insert(buf->end(), chunk);
            auto mark = buf->create_mark(buf->end());
            target_view_.scroll_to(mark);
        });

        output_dispatch_.connect([this] {
            std::string chunk;
            {
                std::lock_guard<std::mutex> g(out_mu_);
                chunk.swap(out_queue_);
            }
            auto buf = console_.get_buffer();
            buf->insert(buf->end(), chunk);
            auto mark = buf->create_mark(buf->end());
            console_.scroll_to(mark);
        });

        auto keys = Gtk::EventControllerKey::create();
        keys->signal_key_pressed().connect(
            [this](guint key, guint, Gdk::ModifierType mods) {
                const auto raw = static_cast<unsigned>(mods);
                const bool ctrl = raw & GDK_CONTROL_MASK;
                const bool shift = raw & GDK_SHIFT_MASK;
                if (key == GDK_KEY_F5) {
                    if (ctrl)
                        run_target();
                    else
                        dbg(backend_.continue_cmd);
                    return true;
                }
                if (key == GDK_KEY_F10) {
                    dbg(backend_.step_over);
                    return true;
                }
                if (key == GDK_KEY_F11 && shift) {
                    dbg(backend_.step_out);
                    return true;
                }
                if (key == GDK_KEY_F11) {
                    dbg(backend_.step_in);
                    return true;
                }
                if (ctrl && (key == GDK_KEY_b || key == GDK_KEY_B)) {
                    auto spec = std::string(bp_.get_text());
                    if (!spec.empty()) dbg(backend_.breakpoint(spec));
                    return true;
                }
                if (ctrl && (key == GDK_KEY_p || key == GDK_KEY_P)) {
                    cmd_.grab_focus();
                    return true;
                }
                if (ctrl && (key == GDK_KEY_d || key == GDK_KEY_D)) {
                    show_disassembly();
                    return true;
                }
                if (ctrl && (key == GDK_KEY_f || key == GDK_KEY_F)) {
                    find_entry_.grab_focus();
                    return true;
                }
                if (ctrl && (key == GDK_KEY_g || key == GDK_KEY_G)) {
                    goto_entry_.grab_focus();
                    return true;
                }
                if (ctrl && (key == GDK_KEY_r || key == GDK_KEY_R)) {
                    reload_current_file();
                    return true;
                }
                return false;
            },
            false);
        window_->add_controller(keys);

        stop_dispatch_.connect([this] { refresh_state(); });

        window_->present();
        for (auto& s : cfg_.sources) show_source(s, 1, false);
        Glib::signal_idle().connect_once([this] { start_session(); });
    }

private:
    Config cfg_;
    Backend backend_;
    std::unique_ptr<DebuggerSession> session_;
    std::unique_ptr<Gtk::ApplicationWindow> window_;
    std::unique_ptr<Gtk::Window> settings_win_;
    Gtk::DropDown engine_drop_, theme_drop_;
    Gtk::SpinButton font_size_;
    Glib::RefPtr<Gtk::CssProvider> css_;
    Gtk::Entry exe_, args_, cwd_, env_, bp_, cmd_, attach_pid_, watch_entry_;
    Gtk::Entry stdin_path_, stdout_path_, stderr_path_, tty_path_, remote_, core_path_;
    Gtk::Entry find_entry_, goto_entry_, bp_cond_, bp_thread_, mem_addr_, mem_count_, reg_name_,
        reg_val_;
    Gtk::CheckButton hex_locals_;
    Gtk::TextView mem_view_;
    Gtk::ScrolledWindow mem_scroll_;
    std::vector<std::string> history_;
    int history_idx_ = 0;
    Gtk::TextView source_map_;
    Gtk::ScrolledWindow source_map_scroll_;
    std::map<std::string, std::set<int>> bp_lines_;
    std::string current_src_;
    int current_line_ = 0;
    std::map<std::string, Glib::RefPtr<Gio::FileMonitor>> file_monitors_;
    Gtk::DropDown follow_fork_;
    Gtk::CheckButton detach_fork_;
    Gtk::CheckButton tgt_console_on_, tgt_bind_in_, tgt_bind_out_, tgt_bind_err_, tgt_echo_, tgt_crlf_;
    Gtk::TextView extra_, console_, target_view_;
    Gtk::ScrolledWindow extra_scroll_, console_scroll_, target_scroll_;
    Gtk::CheckButton stop_at_entry_;
    Gtk::Label status_, console_lab_, target_lab_;
    Gtk::Entry target_in_;
    std::unique_ptr<TargetPty> target_pty_;
    Glib::Dispatcher target_dispatch_;
    std::mutex tgt_mu_;
    std::string tgt_queue_;
    Gtk::Notebook notebook_;
    Gtk::ListBox threads_, frames_, vars_, regs_, bps_, watches_;
    Glib::Dispatcher output_dispatch_;
    Glib::Dispatcher stop_dispatch_;
    std::mutex out_mu_;
    std::string out_queue_;
    bool busy_ = false;
    std::unordered_map<std::string, Gtk::Widget*> tabs_;
    std::unordered_map<std::string, GtkSourceView*> views_;
    std::unordered_map<std::string, GtkSourceBuffer*> buffers_;
    std::vector<std::string> watch_exprs_;
    std::string disasm_key_ = "[disassembly]";
    gint64 last_stop_ms_ = 0;

    void set_status(const std::string& text, const char* color = "#9aa0a6") {
        auto safe = Glib::Markup::escape_text(text);
        status_.set_markup("<span foreground='" + std::string(color) + "'>" + safe + "</span>");
    }

    void append_console(const std::string& text) {
        {
            std::lock_guard<std::mutex> g(out_mu_);
            out_queue_ += text;
        }
        output_dispatch_.emit();
    }

    void persist() {
        cfg_.target = exe_.get_text();
        cfg_.args = args_.get_text();
        cfg_.cwd = cwd_.get_text();
        cfg_.env = env_.get_text();
        cfg_.extra = extra_.get_buffer()->get_text();
        cfg_.stop_at_entry = stop_at_entry_.get_active();
        cfg_.stdin_path = stdin_path_.get_text();
        cfg_.stdout_path = stdout_path_.get_text();
        cfg_.stderr_path = stderr_path_.get_text();
        cfg_.tty = tty_path_.get_text();
        cfg_.remote = remote_.get_text();
        cfg_.core = core_path_.get_text();
        {
            auto idx = follow_fork_.get_selected();
            const char* modes[] = {"parent", "child", "ask", "both"};
            cfg_.follow_fork = modes[idx > 3 ? 0 : idx];
        }
        cfg_.detach_on_fork = detach_fork_.get_active();
        cfg_.target_console = tgt_console_on_.get_active();
        cfg_.target_bind_stdin = tgt_bind_in_.get_active();
        cfg_.target_bind_stdout = tgt_bind_out_.get_active();
        cfg_.target_bind_stderr = tgt_bind_err_.get_active();
        cfg_.target_echo = tgt_echo_.get_active();
        cfg_.target_crlf = tgt_crlf_.get_active();
        cfg_.source_map = source_map_.get_buffer()->get_text();
        cfg_.hex_locals = hex_locals_.get_active();
        cfg_.font_size = static_cast<int>(font_size_.get_value());
        cfg_.theme = theme_drop_.get_selected() == 1 ? "light" : "dark";
        cfg_.debugger = backend_.name;
        auto path = cfg_.config_path.empty() ? resolve_config_path() : cfg_.config_path;
        cfg_.config_path = path;
        save_config_file(cfg_, path);
    }

    void on_engine_changed() {
        auto idx = engine_drop_.get_selected();
        std::string name = idx == 1 ? "gdb" : "lldb";
        if (name == backend_.name && session_ && session_->alive()) return;
        backend_ = name == "gdb" ? make_gdb(cfg_.debugger_path) : make_lldb(cfg_.debugger_path);
        persist();
        start_session();
    }

    void start_session() {
        session_ = std::make_unique<DebuggerSession>(
            backend_,
            [this](const std::string& t) { append_console(t); },
            [this](const std::string&) {
                auto now = g_get_monotonic_time();
                if (now - last_stop_ms_ < 250000) return;
                last_stop_ms_ = now;
                stop_dispatch_.emit();
            });
        if (!session_->start()) {
            set_status("Failed to start " + backend_.name, "#f28b82");
            return;
        }
        set_status(backend_.name + " ready (" + backend_.binary + ")", "#8ab4f8");
        window_->set_title("sdbg — " + backend_.name);
        console_lab_.set_text(backend_.name + " CONSOLE");
        Glib::signal_timeout().connect_once([this] { startup_actions(); }, 350);
    }

    void startup_actions() {
        if (!session_ || !session_->alive()) return;
        bool auto_act = !cfg_.target.empty() || cfg_.load || cfg_.run || cfg_.attach ||
                        !cfg_.core.empty() || !cfg_.breakpoints.empty();
        if (!auto_act) return;
        run_bg([this] {
            auto exe = exe_.get_text();
            if (!exe.empty()) {
                for (auto& c : backend_.load_target(exe)) session_->command(c);
                if (!cfg_.core.empty()) {
                    if (backend_.name == "gdb")
                        session_->command("core-file \"" + cfg_.core + "\"");
                    else
                        session_->command("target create --core \"" + cfg_.core + "\" \"" + exe +
                                          "\"");
                }
                for (auto& c : backend_.apply_params(args_.get_text(), cwd_.get_text(),
                                                     env_.get_text(), extra_.get_buffer()->get_text(),
                                                     stop_at_entry_.get_active()))
                    session_->command(c);
                apply_extended();
            }
            if (!cfg_.remote.empty()) session_->command(backend_.remote_cmd(cfg_.remote));
            if (!cfg_.core.empty())
                session_->command(backend_.core_cmd(cfg_.core, exe_.get_text()));
            if (cfg_.attach) {
                session_->command(backend_.attach_cmd(cfg_.attach));
            }
            for (auto& spec : cfg_.breakpoints) session_->command(backend_.breakpoint(spec));
            if (cfg_.run && !exe.empty() && !cfg_.attach) {
                session_->command(backend_.run_cmd(stop_at_entry_.get_active()));
                Glib::signal_idle().connect_once([this] { refresh_state(); });
            }
        });
    }

    void run_bg(std::function<void()> fn) {
        if (busy_) return;
        busy_ = true;
        set_status("Working…", "#fdd663");
        std::thread([this, fn = std::move(fn)] {
            try {
                fn();
            } catch (const std::exception& ex) {
                Glib::signal_idle().connect_once([this, msg = std::string(ex.what())] {
                    set_status(std::string("Error: ") + msg, "#f28b82");
                });
            }
            Glib::signal_idle().connect_once([this] { busy_ = false; });
        }).detach();
    }

    void show_config_dialog() {
        if (!settings_win_) return;
        settings_win_->present();
    }

    void apply_theme() {
        if (!css_) return;
        int px = static_cast<int>(font_size_.get_value());
        if (px < 9) px = 12;
        bool light = theme_drop_.get_selected() == 1;
        std::string bg = light ? "#f4f6f8" : "#1b1e24";
        std::string fg = light ? "#1b1e24" : "#e8eaed";
        std::string panel = light ? "#ffffff" : "#12151a";
        css_->load_from_data(
            "window { background: " + bg + "; color: " + fg + "; }"
            "entry, textview { font-family: monospace; font-size: " + std::to_string(px) + "px; }"
            "textview.mono-pane { background: " + panel + "; color: " + fg + "; }"
            "textview.source-editor { font-family: monospace; font-size: " +
            std::to_string(px) + "px; }");
        cfg_.font_size = px;
        cfg_.theme = light ? "light" : "dark";
        for (auto& kv : buffers_) apply_source_scheme(kv.second, light);
    }

    void dbg_async(const std::string& cmd) {
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        set_status(cmd + " (async)", "#fdd663");
        session_->command_async(cmd);
    }

    void history_up() {
        if (history_.empty()) return;
        if (history_idx_ > 0) --history_idx_;
        cmd_.set_text(history_[history_idx_]);
    }

    void history_down() {
        if (history_.empty()) return;
        if (history_idx_ + 1 < static_cast<int>(history_.size())) {
            ++history_idx_;
            cmd_.set_text(history_[history_idx_]);
        } else {
            history_idx_ = static_cast<int>(history_.size());
            cmd_.set_text("");
        }
    }

    void read_memory() {
        auto addr = std::string(mem_addr_.get_text());
        if (addr.empty()) {
            set_status("Enter a memory address", "#f28b82");
            return;
        }
        int n = std::atoi(std::string(mem_count_.get_text()).c_str());
        if (n <= 0) n = 64;
        if (!session_ || !session_->alive()) return;
        run_bg([this, addr, n] {
            auto out = session_->command(backend_.mem_cmd(addr, n));
            Glib::signal_idle().connect_once([this, out] { mem_view_.get_buffer()->set_text(out); });
        });
    }

    void dbg(const std::string& cmd) {
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        run_bg([this, cmd] {
            auto out = session_->command(cmd);
            Glib::signal_idle().connect_once([this] { refresh_state(); });
            Glib::signal_idle().connect_once([this, cmd] { set_status(cmd, "#81c995"); });
            (void)out;
        });
    }

    void apply_extended() {
        auto follow = cfg_.follow_fork;
        std::string in = stdin_path_.get_text();
        std::string out = stdout_path_.get_text();
        std::string err = stderr_path_.get_text();
        std::string tty = tty_path_.get_text();
        if (tgt_console_on_.get_active()) {
            if (!target_pty_ || !target_pty_->alive()) {
                target_pty_ = std::make_unique<TargetPty>([this](const std::string& t) {
                    std::lock_guard<std::mutex> g(tgt_mu_);
                    tgt_queue_ += t;
                    target_dispatch_.emit();
                });
                if (!target_pty_->start()) {
                    Glib::signal_idle().connect_once(
                        [this] { set_status("Failed to open target PTY", "#f28b82"); });
                }
            }
            if (target_pty_ && target_pty_->alive()) {
                Glib::signal_idle().connect_once([this] {
                    target_lab_.set_text("TARGET CONSOLE — " + target_pty_->slave_name());
                });
                if (tty.empty()) tty = target_pty_->slave_name();
                if (tgt_bind_in_.get_active() && in.empty()) in = target_pty_->slave_name();
                if (tgt_bind_out_.get_active() && out.empty()) out = target_pty_->slave_name();
                if (tgt_bind_err_.get_active() && err.empty()) err = target_pty_->slave_name();
            }
        }
        for (auto& c : backend_.io_cmds(in, out, err, tty, args_.get_text()))
            session_->command(c);
        for (auto& c : backend_.fork_cmds(follow, detach_fork_.get_active()))
            session_->command(c);
        for (auto& pair : parse_source_maps())
            session_->command(backend_.source_map_cmd(pair.first, pair.second));
    }

    void send_target_input() {
        auto line = std::string(target_in_.get_text());
        target_in_.set_text("");
        if (line.empty()) return;
        if (tgt_crlf_.get_active())
            line += "\r\n";
        else
            line += "\n";
        if (tgt_echo_.get_active()) {
            auto buf = target_view_.get_buffer();
            buf->insert(buf->end(), line);
        }
        if (target_pty_ && target_pty_->alive())
            target_pty_->write(line);
        else
            set_status("Target console PTY is not open — Load/Run first", "#f28b82");
    }

    void load_target() {
        persist();
        auto exe = exe_.get_text();
        if (exe.empty()) {
            set_status("Choose a target executable", "#f28b82");
            return;
        }
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        run_bg([this, exe] {
            for (auto& c : backend_.load_target(exe)) session_->command(c);
            for (auto& c : backend_.apply_params(args_.get_text(), cwd_.get_text(), env_.get_text(),
                                                 extra_.get_buffer()->get_text(),
                                                 stop_at_entry_.get_active()))
                session_->command(c);
            apply_extended();
            Glib::signal_idle().connect_once(
                [this, exe] { set_status("Loaded target " + exe, "#81c995"); });
        });
    }

    void apply_params() {
        persist();
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        run_bg([this] {
            for (auto& c : backend_.apply_params(args_.get_text(), cwd_.get_text(), env_.get_text(),
                                                 extra_.get_buffer()->get_text(),
                                                 stop_at_entry_.get_active()))
                session_->command(c);
            apply_extended();
            Glib::signal_idle().connect_once([this] { set_status("Applied target parameters", "#81c995"); });
        });
    }

    void run_target() {
        persist();
        auto exe = exe_.get_text();
        if (exe.empty()) {
            set_status("Choose a target executable", "#f28b82");
            return;
        }
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        run_bg([this, exe] {
            for (auto& c : backend_.load_target(exe)) session_->command(c);
            for (auto& c : backend_.apply_params(args_.get_text(), cwd_.get_text(), env_.get_text(),
                                                 extra_.get_buffer()->get_text(),
                                                 stop_at_entry_.get_active()))
                session_->command(c);
            apply_extended();
            session_->command_async(backend_.run_cmd(stop_at_entry_.get_active()));
            Glib::signal_idle().connect_once([this, exe] { set_status("Running " + exe, "#81c995"); });
        });
    }

    void reload_target() {
        persist();
        auto exe = exe_.get_text();
        if (exe.empty()) {
            set_status("Choose a target executable", "#f28b82");
            return;
        }
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        run_bg([this, exe] {
            for (auto& c : backend_.reload_cmds(exe)) session_->command(c);
            apply_extended();
            Glib::signal_idle().connect_once(
                [this, exe] { set_status("Reloaded " + exe, "#81c995"); });
        });
    }

    void load_core() {
        persist();
        auto core = std::string(core_path_.get_text());
        if (core.empty()) {
            set_status("Set a core dump path", "#f28b82");
            return;
        }
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        auto exe = exe_.get_text();
        run_bg([this, core, exe] {
            if (!exe.empty()) {
                for (auto& c : backend_.load_target(exe)) session_->command(c);
            }
            session_->command(backend_.core_cmd(core, exe));
            Glib::signal_idle().connect_once([this] { refresh_state(); });
            Glib::signal_idle().connect_once(
                [this, core] { set_status("Loaded core " + core, "#81c995"); });
        });
    }

    void connect_remote() {
        persist();
        auto spec = std::string(remote_.get_text());
        if (spec.empty()) {
            set_status("Set remote host:port", "#f28b82");
            return;
        }
        if (!session_ || !session_->alive()) {
            set_status("Start the debugger first", "#f28b82");
            return;
        }
        auto exe = exe_.get_text();
        run_bg([this, spec, exe] {
            if (!exe.empty()) {
                for (auto& c : backend_.load_target(exe)) session_->command(c);
            }
            session_->command(backend_.remote_cmd(spec));
            Glib::signal_idle().connect_once([this] { refresh_state(); });
            Glib::signal_idle().connect_once(
                [this, spec] { set_status("Connected " + spec, "#81c995"); });
        });
    }

    void send_console() {
        auto t = std::string(cmd_.get_text());
        cmd_.set_text("");
        if (t.empty()) return;
        history_.push_back(t);
        history_idx_ = static_cast<int>(history_.size());
        dbg(t);
    }

    void fill_list(Gtk::ListBox& box, const std::vector<std::string>& rows) {
        while (auto* row = box.get_row_at_index(0)) box.remove(*row);
        for (auto& r : rows) {
            auto* lab = Gtk::make_managed<Gtk::Label>(r);
            lab->set_xalign(0);
            lab->set_selectable(true);
            box.append(*lab);
        }
    }

    void refresh_state() {
        if (!session_ || !session_->alive()) return;
        std::thread([this] {
            try {
                auto th = session_->command(backend_.threads_cmd);
                auto bt = session_->command("bt");
                auto vars = session_->command(backend_.locals_cmd);
                auto regs = session_->command(backend_.regs_cmd);
                auto bps = session_->command(backend_.breaks_cmd);
                auto fi = session_->command(backend_.frame_info_cmd);
                Glib::signal_idle().connect_once([this, th, bt, vars, regs, bps, fi] {
                    fill_list(threads_, clean_lines(th));
                    fill_list(frames_, clean_lines(bt));
                    fill_list(vars_, clean_lines(vars));
                    auto r = clean_lines(regs);
                    if (r.size() > 80) r.resize(80);
                    fill_list(regs_, r);
                    fill_list(bps_, clean_lines(bps));
                    parse_bp_locations(bps);
                    std::smatch m;
                    bool have_src = std::regex_search(fi, m, kLoc) || std::regex_search(bt, m, kLoc);
                    if (have_src)
                        show_source(m[1].str(), std::stoi(m[2].str()), true);
                    else
                        show_disassembly();
                    refresh_all_marks();
                });
                refresh_watches();
            } catch (...) {
            }
        }).detach();
    }

    std::vector<std::pair<std::string, std::string>> parse_source_maps() {
        std::vector<std::pair<std::string, std::string>> out;
        auto text = std::string(source_map_.get_buffer()->get_text());
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            auto a = line.find_first_not_of(" \t");
            if (a == std::string::npos || line[a] == '#') continue;
            line = line.substr(a);
            std::string from, to;
            auto eq = line.find('=');
            auto arrow = line.find("=>");
            if (arrow != std::string::npos) {
                from = line.substr(0, arrow);
                to = line.substr(arrow + 2);
            } else if (eq != std::string::npos) {
                from = line.substr(0, eq);
                to = line.substr(eq + 1);
            } else {
                auto sp = line.find(' ');
                if (sp == std::string::npos) continue;
                from = line.substr(0, sp);
                to = line.substr(sp + 1);
            }
            auto trim = [](std::string s) {
                auto b = s.find_first_not_of(" \t");
                auto e = s.find_last_not_of(" \t");
                if (b == std::string::npos) return std::string();
                return s.substr(b, e - b + 1);
            };
            from = trim(from);
            to = trim(to);
            if (!from.empty() && !to.empty()) out.emplace_back(from, to);
        }
        return out;
    }

    std::string remap_path(std::string path) {
        for (auto& p : parse_source_maps()) {
            if (path.compare(0, p.first.size(), p.first) == 0)
                path = p.second + path.substr(p.first.size());
        }
        return path;
    }

    std::string resolve_source(const std::string& path) {
        auto mapped = remap_path(path);
        if (access(mapped.c_str(), R_OK) == 0) return mapped;
        if (access(path.c_str(), R_OK) == 0) return path;
        std::string a = std::string(cwd_.get_text()) + "/" + mapped;
        if (access(a.c_str(), R_OK) == 0) return a;
        a = std::string(cwd_.get_text()) + "/" + path;
        if (access(a.c_str(), R_OK) == 0) return a;
        return {};
    }

    void show_source(const std::string& path, int line, bool focus) {
        auto real = resolve_source(path);
        if (real.empty()) return;
        if (!tabs_.count(real)) {
            GtkSourceView* view = nullptr;
            GtkSourceBuffer* buf = nullptr;
            auto* page = make_source_page(real, read_file(real), &view, &buf,
                                          theme_drop_.get_selected() == 1);
            page->set_name(real);
            auto* label = Gtk::make_managed<Gtk::Label>(Glib::path_get_basename(real));
            notebook_.append_page(*page, *label);
            tabs_[real] = page;
            views_[real] = view;
            buffers_[real] = buf;
            hook_gutter(view, real);
            watch_file(real);
        }
        if (focus) {
            current_src_ = real;
            current_line_ = line;
            auto* page = tabs_[real];
            notebook_.set_current_page(notebook_.page_num(*page));
            window_->set_title("sdbg — " + Glib::path_get_basename(real));
            apply_marks(real);
            if (line > 0 && views_[real]) {
                GtkTextIter it;
                gtk_text_buffer_get_iter_at_line(
                    gtk_text_view_get_buffer(GTK_TEXT_VIEW(views_[real])), &it, line - 1);
                gtk_text_buffer_place_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(views_[real])),
                                             &it);
                gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(views_[real]), &it, 0.2, TRUE, 0.0, 0.35);
            }
        }
    }

    void close_current_tab() {
        int n = notebook_.get_current_page();
        if (n < 0) return;
        auto* w = notebook_.get_nth_page(n);
        if (!w) return;
        for (auto it = tabs_.begin(); it != tabs_.end();) {
            if (it->second->gobj() == w->gobj()) {
                views_.erase(it->first);
                buffers_.erase(it->first);
                file_monitors_.erase(it->first);
                it = tabs_.erase(it);
            } else
                ++it;
        }
        notebook_.remove_page(n);
    }

    void browse_exe() {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Select executable");
        dlg->open(*window_, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                auto f = dlg->open_finish(res);
                if (f) exe_.set_text(f->get_path());
            } catch (...) {
            }
        });
    }

    static std::string first_number(const Glib::ustring& text) {
        static const std::regex re(R"((\d+))");
        std::smatch m;
        std::string s = text.raw();
        if (std::regex_search(s, m, re)) return m[1];
        return {};
    }

    Gtk::Label* selected_label(Gtk::ListBox& box) {
        auto* row = box.get_selected_row();
        if (!row) return nullptr;
        return dynamic_cast<Gtk::Label*>(row->get_child());
    }

    void delete_selected_bp() {
        if (auto* lab = selected_label(bps_)) {
            auto id = first_number(lab->get_text());
            if (!id.empty()) dbg(backend_.delete_bp_id(id));
        }
    }

    void toggle_selected_bp() {
        if (auto* lab = selected_label(bps_)) {
            auto id = first_number(lab->get_text());
            if (id.empty()) return;
            auto t = std::string(lab->get_text());
            bool disabled = t.find("disabled") != std::string::npos ||
                            t.find("inactive") != std::string::npos;
            dbg(backend_.enable_bp_id(id, disabled));
        }
    }

    void add_watch() {
        auto e = std::string(watch_entry_.get_text());
        if (e.empty()) return;
        watch_exprs_.push_back(e);
        watch_entry_.set_text("");
        refresh_watches();
    }

    void remove_watch() {
        auto* row = watches_.get_selected_row();
        if (!row) return;
        int i = row->get_index();
        if (i >= 0 && i < static_cast<int>(watch_exprs_.size()))
            watch_exprs_.erase(watch_exprs_.begin() + i);
        refresh_watches();
    }

    void refresh_watches() {
        if (!session_ || !session_->alive() || watch_exprs_.empty()) {
            Glib::signal_idle().connect_once([this] {
                std::vector<std::string> rows = watch_exprs_;
                fill_list(watches_, rows);
            });
            return;
        }
        std::vector<std::string> exprs = watch_exprs_;
        std::thread([this, exprs] {
            std::vector<std::string> rows;
            for (auto& e : exprs) {
                try {
                    auto out = session_->command(backend_.eval_cmd(e), 3.0);
                    auto lines = clean_lines(out);
                    std::string val = e + " = ";
                    if (!lines.empty()) val += lines.back();
                    rows.push_back(val);
                } catch (...) {
                    rows.push_back(e + " = <error>");
                }
            }
            Glib::signal_idle().connect_once([this, rows] { fill_list(watches_, rows); });
        }).detach();
    }

    void attach_process() {
        auto t = std::string(attach_pid_.get_text());
        if (t.empty()) {
            set_status("Enter a PID", "#f28b82");
            return;
        }
        int pid = std::atoi(t.c_str());
        if (pid <= 0) {
            set_status("Invalid PID", "#f28b82");
            return;
        }
        dbg(backend_.attach_cmd(pid));
    }

    void show_disassembly() {
        if (!session_ || !session_->alive()) return;
        std::thread([this] {
            try {
                auto text = session_->command(backend_.disasm_cmd, 4.0);
                Glib::signal_idle().connect_once([this, text] { set_disasm_tab(text); });
            } catch (...) {
            }
        }).detach();
    }

    void set_disasm_tab(const std::string& text) {
        if (!tabs_.count(disasm_key_)) {
            GtkSourceView* view = nullptr;
            GtkSourceBuffer* buf = nullptr;
            auto* page = make_source_page("disasm.s", text.empty() ? "; no disassembly\n" : text,
                                          &view, &buf, theme_drop_.get_selected() == 1);
            auto* label = Gtk::make_managed<Gtk::Label>("DISASSEMBLY");
            notebook_.append_page(*page, *label);
            tabs_[disasm_key_] = page;
            views_[disasm_key_] = view;
        } else if (views_[disasm_key_]) {
            gtk_text_buffer_set_text(
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(views_[disasm_key_])),
                text.empty() ? "; no disassembly\n" : text.c_str(), -1);
        }
        notebook_.set_current_page(notebook_.page_num(*tabs_[disasm_key_]));
        set_status("Disassembly", "#8ab4f8");
    }

    struct GutterData {
        DebuggerApp* app;
        std::string path;
    };

    static void on_gutter_click(GtkGestureClick*, gint, gdouble x, gdouble y, gpointer data) {
        auto* gd = static_cast<GutterData*>(data);
        if (!gd || !gd->app) return;
        auto* view = gd->app->views_[gd->path];
        if (!view) return;
        if (x > 56) return;
        GtkTextIter it;
        gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view), &it, 0, static_cast<int>(y));
        int line = gtk_text_iter_get_line(&it) + 1;
        auto spec = gd->path + ":" + std::to_string(line);
        gd->app->bp_.set_text(spec);
        gd->app->dbg(gd->app->backend_.breakpoint(spec));
    }

    void parse_bp_locations(const std::string& text) {
        bp_lines_.clear();
        static const std::regex file_line(
            R"(file\s*=\s*'([^']+)'\s*,\s*line\s*=\s*(\d+))", std::regex::icase);
        static const std::regex colon(R"((?:in\s+)?(\S+\.\w+):(\d+))");
        std::smatch m;
        auto s = text;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), file_line);
             it != std::sregex_iterator(); ++it) {
            auto real = resolve_source((*it)[1].str());
            if (!real.empty()) bp_lines_[real].insert(std::stoi((*it)[2]));
        }
        for (auto it = std::sregex_iterator(s.begin(), s.end(), colon);
             it != std::sregex_iterator(); ++it) {
            auto real = resolve_source((*it)[1].str());
            if (!real.empty()) bp_lines_[real].insert(std::stoi((*it)[2]));
        }
    }

    void clear_marks(GtkSourceBuffer* buf) {
        if (!buf) return;
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buf), &start, &end);
        gtk_source_buffer_remove_source_marks(buf, &start, &end, "breakpoint");
        gtk_source_buffer_remove_source_marks(buf, &start, &end, "current");
    }

    void apply_marks(const std::string& path) {
        auto bit = buffers_.find(path);
        if (bit == buffers_.end() || !bit->second) return;
        clear_marks(bit->second);
        auto add = [&](int line, const char* cat) {
            if (line < 1) return;
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(bit->second), &it, line - 1);
            gtk_source_buffer_create_source_mark(bit->second, nullptr, cat, &it);
        };
        auto bps = bp_lines_.find(path);
        if (bps != bp_lines_.end())
            for (int ln : bps->second) add(ln, "breakpoint");
        if (path == current_src_) add(current_line_, "current");
    }

    void refresh_all_marks() {
        for (auto& kv : buffers_) apply_marks(kv.first);
    }

    GtkSourceView* current_view() {
        int n = notebook_.get_current_page();
        if (n < 0) return nullptr;
        auto* w = notebook_.get_nth_page(n);
        if (!w) return nullptr;
        for (auto& kv : tabs_) {
            if (kv.second->gobj() == w->gobj()) {
                auto it = views_.find(kv.first);
                if (it != views_.end()) return it->second;
            }
        }
        return nullptr;
    }

    std::string current_tab_path() {
        int n = notebook_.get_current_page();
        if (n < 0) return {};
        auto* w = notebook_.get_nth_page(n);
        if (!w) return {};
        for (auto& kv : tabs_)
            if (kv.second->gobj() == w->gobj()) return kv.first;
        return {};
    }

    void find_in_file(bool forward) {
        auto* view = current_view();
        if (!view) return;
        auto needle = std::string(find_entry_.get_text());
        if (needle.empty()) return;
        auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
        GtkTextIter start, match_s, match_e;
        gtk_text_buffer_get_iter_at_mark(buf, &start, gtk_text_buffer_get_insert(buf));
        gboolean ok;
        if (forward)
            ok = gtk_text_iter_forward_search(&start, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY,
                                              &match_s, &match_e, nullptr);
        else {
            ok = gtk_text_iter_backward_search(&start, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY,
                                               &match_s, &match_e, nullptr);
        }
        if (!ok) {
            if (forward)
                gtk_text_buffer_get_start_iter(buf, &start);
            else
                gtk_text_buffer_get_end_iter(buf, &start);
            if (forward)
                ok = gtk_text_iter_forward_search(&start, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY,
                                                  &match_s, &match_e, nullptr);
            else
                ok = gtk_text_iter_backward_search(&start, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY,
                                                   &match_s, &match_e, nullptr);
        }
        if (ok) {
            gtk_text_buffer_select_range(buf, &match_s, &match_e);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view), &match_s, 0.2, TRUE, 0.0, 0.35);
        } else {
            set_status("Not found: " + needle, "#fdd663");
        }
    }

    void goto_line_ui() {
        auto t = std::string(goto_entry_.get_text());
        int line = std::atoi(t.c_str());
        auto path = current_tab_path();
        if (path.empty() || line < 1) return;
        show_source(path, line, true);
    }

    void reload_current_file() {
        auto path = current_tab_path();
        if (path.empty() || path == disasm_key_) return;
        auto bit = buffers_.find(path);
        if (bit == buffers_.end() || !bit->second) return;
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(bit->second), read_file(path).c_str(), -1);
        apply_marks(path);
        set_status("Reloaded " + path, "#81c995");
    }

    void watch_file(const std::string& path) {
        if (file_monitors_.count(path)) return;
        try {
            auto f = Gio::File::create_for_path(path);
            auto mon = f->monitor_file(Gio::FileMonitorFlags::NONE);
            mon->signal_changed().connect(
                [this, path](const Glib::RefPtr<Gio::File>&, const Glib::RefPtr<Gio::File>&,
                             Gio::FileMonitor::Event ev) {
                    if (ev == Gio::FileMonitor::Event::CHANGES_DONE_HINT ||
                        ev == Gio::FileMonitor::Event::CHANGED)
                        Glib::signal_timeout().connect_once(
                            [this, path] {
                                auto bit = buffers_.find(path);
                                if (bit == buffers_.end() || !bit->second) return;
                                gtk_text_buffer_set_text(GTK_TEXT_BUFFER(bit->second),
                                                         read_file(path).c_str(), -1);
                                apply_marks(path);
                                set_status("File changed on disk: " + path, "#8ab4f8");
                            },
                            200);
                });
            file_monitors_[path] = mon;
        } catch (...) {
        }
    }

    void hook_gutter(GtkSourceView* view, const std::string& path) {
        auto* gest = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gest), 1);
        auto* gd = new GutterData{this, path};
        g_signal_connect(gest, "pressed", G_CALLBACK(on_gutter_click), gd);
        gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(gest));
    }

    void browse_source() {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Open source file");
        dlg->open(*window_, [this, dlg](const Glib::RefPtr<Gio::AsyncResult>& res) {
            try {
                auto f = dlg->open_finish(res);
                if (f) show_source(f->get_path(), 1, true);
            } catch (...) {
            }
        });
    }
};

int main(int argc, char** argv) {
    auto cfg = parse_cli(argc, argv);
    auto path = resolve_config_path(cfg.config_path);
    cfg.config_path = path;
    load_config_file(cfg, path);
    // CLI wins over file for explicit target already parsed first; reload file then re-apply
    // debugger from env if present.
    const char* envd = std::getenv("DEBUGGER_BACKEND");
    if (envd && (std::string(envd) == "gdb" || std::string(envd) == "lldb"))
        cfg.debugger = envd;
    auto app = Glib::make_refptr_for_instance(new DebuggerApp(std::move(cfg)));
    return app->run(0, nullptr);
}
