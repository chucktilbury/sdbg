// Minimal Debug Adapter Protocol front-end for GDB/MI.
// Speak DAP on stdin/stdout; drive gdb --interpreter=mi2.
#include "backend.hpp"
#include "session.hpp"

#include <iostream>
#include <sstream>

static std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n')
            o += "\\n";
        else
            o.push_back(c);
    }
    o.push_back('"');
    return o;
}

static void send(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

static std::string read_message() {
    std::string line, all;
    int len = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Content-Length:", 0) == 0)
            len = std::atoi(line.c_str() + 15);
        if (line.empty()) break;
    }
    if (len <= 0) return {};
    all.resize(len);
    std::cin.read(&all[0], len);
    return all;
}

static std::string field(const std::string& js, const std::string& key) {
    auto k = "\"" + key + "\"";
    auto p = js.find(k);
    if (p == std::string::npos) return {};
    p = js.find(':', p);
    p = js.find_first_not_of(" \t\"", p + 1);
    if (p == std::string::npos) return {};
    if (js[p] == '"') {
        auto e = js.find('"', p + 1);
        return js.substr(p + 1, e - p - 1);
    }
    auto e = js.find_first_of(",}", p);
    return js.substr(p, e - p);
}

int main() {
    Backend gdb = make_gdb({});
    DebuggerSession sess(gdb, [](const std::string&) {}, [](const std::string&) {});
    int seq_out = 1;
    auto ok = [&](const std::string& req, const std::string& body = "{}") {
        auto cmd = field(req, "command");
        auto seq = field(req, "seq");
        send("{\"type\":\"response\",\"request_seq\":" + (seq.empty() ? std::string("0") : seq) +
             ",\"success\":true,\"command\":" + json_str(cmd) + ",\"body\":" + body + "}");
    };
    while (true) {
        auto msg = read_message();
        if (msg.empty()) break;
        auto cmd = field(msg, "command");
        if (cmd == "initialize") {
            ok(msg,
               "{\"supportsConfigurationDoneRequest\":true,\"supportsConditionalBreakpoints\":true}");
            send("{\"type\":\"event\",\"event\":\"initialized\",\"seq\":" + std::to_string(seq_out++) +
                 "}");
        } else if (cmd == "launch") {
            auto prog = field(msg, "program");
            if (!sess.alive()) sess.start();
            if (!prog.empty()) sess.command("file \"" + prog + "\"");
            ok(msg);
        } else if (cmd == "setBreakpoints") {
            auto path = field(msg, "path");
            sess.command("breakpoint delete --force");
            // naive: look for "line":N
            std::string rest = msg;
            size_t p = 0;
            while ((p = rest.find("\"line\":", p)) != std::string::npos) {
                int line = std::atoi(rest.c_str() + p + 7);
                if (line > 0 && !path.empty())
                    sess.command("break " + path + ":" + std::to_string(line));
                p += 7;
            }
            ok(msg, "{\"breakpoints\":[{\"verified\":true}]}");
        } else if (cmd == "configurationDone") {
            ok(msg);
        } else if (cmd == "continue") {
            sess.command_async("continue");
            ok(msg, "{\"allThreadsContinued\":true}");
        } else if (cmd == "next") {
            sess.command_async("next");
            ok(msg);
        } else if (cmd == "stepIn") {
            sess.command_async("step");
            ok(msg);
        } else if (cmd == "stepOut") {
            sess.command_async("finish");
            ok(msg);
        } else if (cmd == "threads") {
            ok(msg, "{\"threads\":[{\"id\":1,\"name\":\"1\"}]}");
        } else if (cmd == "stackTrace") {
            auto bt = sess.alive() ? sess.command("bt") : std::string();
            ok(msg, "{\"stackFrames\":[{\"id\":0,\"name\":\"frame\",\"line\":1,\"column\":1}],"
                    "\"totalFrames\":1}");
            (void)bt;
        } else if (cmd == "disconnect" || cmd == "terminate") {
            if (sess.alive()) sess.command("quit", 1);
            sess.stop();
            ok(msg);
            break;
        } else {
            ok(msg);
        }
    }
    return 0;
}
