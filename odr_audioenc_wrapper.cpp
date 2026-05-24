#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"
#endif
#include <nlohmann/json.hpp>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>

using json = nlohmann::json;
using namespace std;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_child_pid = -1;

static void handle_signal(int) {
    g_stop = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
    }
}

static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

[[maybe_unused]] static string unescape_string(const string& s) {
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case 'n': out.push_back('\n'); ++i; break;
                case 'r': out.push_back('\r'); ++i; break;
                case 't': out.push_back('\t'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                default: out.push_back(s[i]); break;
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static string now_iso() {
    time_t t = time(nullptr);
    struct tm tmv {};
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z", &tmv);
    return buf;
}

static long epoch_now() {
    return static_cast<long>(time(nullptr));
}

static string format_duration(long sec) {
    if (sec < 0) sec = 0;
    long d = sec / 86400;
    sec %= 86400;
    long h = sec / 3600;
    sec %= 3600;
    long m = sec / 60;
    sec %= 60;
    ostringstream oss;
    if (d > 0) oss << d << "d ";
    if (h < 10) oss << '0';
    oss << h << "h ";
    if (m < 10) oss << '0';
    oss << m << "m ";
    if (sec < 10) oss << '0';
    oss << sec << "s";
    return oss.str();
}


[[maybe_unused]] static string trim_json_ws(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

struct Config {
    string instance_name;
    string lock_file;
    string state_file;
    string stream_url;
    string fallback_stream_url;
    string audioenc_command;
    string fallback_audioenc_command;

    long check_interval_sec = 10;
    long connect_timeout_sec = 8;
    long transfer_timeout_sec = 12;
    long min_bytes = 2048;

    long ok_threshold = 2;
    long fail_threshold = 2;

    long restart_backoff_sec = 2;
    long restart_limit_count = 6;
    long restart_limit_window_sec = 300;
    long restart_cooldown_sec = 120;

    string mail_from;
    string mail_to;
    string msmtp_command = "/usr/bin/msmtp --read-envelope-from -t";

    string email_header;
    string email_footer;
    string owner_name;

    bool notify_on_audioenc_crash = false;
};

static Config load_config(const string& path) {
    ifstream in(path);
    if (!in) {
        throw runtime_error("Cannot open config: " + path);
    }

    json j;
    in >> j;

    Config c;
    c.instance_name = j.value("instance_name", string("odr-audioenc"));
    c.lock_file = j.value("lock_file", string("/var/run/") + c.instance_name + ".lock");
    c.state_file = j.value("state_file", string("/var/lib/odr-audioenc-wrapper/") + c.instance_name + ".state");
    c.stream_url = j.value("stream_url", string());
    c.fallback_stream_url = j.value("fallback_stream_url", string());
    c.audioenc_command = j.value("audioenc_command", string());
    c.fallback_audioenc_command = j.value("fallback_audioenc_command", string());

    c.check_interval_sec = j.value("check_interval_sec", c.check_interval_sec);
    c.connect_timeout_sec = j.value("connect_timeout_sec", c.connect_timeout_sec);
    c.transfer_timeout_sec = j.value("transfer_timeout_sec", c.transfer_timeout_sec);
    c.min_bytes = j.value("min_bytes", c.min_bytes);

    c.ok_threshold = j.value("ok_threshold", c.ok_threshold);
    c.fail_threshold = j.value("fail_threshold", c.fail_threshold);

    c.restart_backoff_sec = j.value("restart_backoff_sec", c.restart_backoff_sec);
    c.restart_limit_count = j.value("restart_limit_count", c.restart_limit_count);
    c.restart_limit_window_sec = j.value("restart_limit_window_sec", c.restart_limit_window_sec);
    c.restart_cooldown_sec = j.value("restart_cooldown_sec", c.restart_cooldown_sec);

    c.mail_from = j.value("mail_from", string());
    c.mail_to = j.value("mail_to", string());
    c.msmtp_command = j.value("msmtp_command", c.msmtp_command);

    c.email_header = j.value("email_header", string());
    c.email_footer = j.value("email_footer", string());
    c.owner_name = j.value("owner_name", string());

    c.notify_on_audioenc_crash = j.value("notify_on_audioenc_crash", false);

    if (c.stream_url.empty()) throw runtime_error("Missing required key: stream_url");
    if (c.audioenc_command.empty()) throw runtime_error("Missing required key: audioenc_command");
    if (!c.fallback_stream_url.empty() && c.fallback_audioenc_command.empty()) {
        throw runtime_error("Missing required key: fallback_audioenc_command when fallback_stream_url is set");
    }
    if (c.fallback_stream_url.empty() && !c.fallback_audioenc_command.empty()) {
        throw runtime_error("Missing required key: fallback_stream_url when fallback_audioenc_command is set");
    }

    return c;
}


static void ensure_parent_dir(const string& path) {
    size_t pos = path.rfind('/');
    if (pos == string::npos) return;
    string dir = path.substr(0, pos);
    if (dir.empty()) return;

    string cur = (dir[0] == '/') ? "/" : "";
    stringstream ss(dir);
    string part;
    while (getline(ss, part, '/')) {
        if (part.empty()) continue;
        if (!cur.empty() && cur.back() != '/') cur += "/";
        cur += part;
        mkdir(cur.c_str(), 0755);
    }
}

static string load_state_file(const string& path) {
    ifstream in(path);
    if (!in) return "UNKNOWN";
    string state;
    getline(in, state);
    state = trim(state);
    if (state.empty()) return "UNKNOWN";
    return state;
}

static void save_state_file(const string& path, const string& state) {
    ensure_parent_dir(path);
    ofstream out(path, ios::trunc);
    if (!out) {
        throw runtime_error("Cannot write state file: " + path);
    }
    out << state << "\n";
}

static string replace_vars(string text, const Config& cfg, const string& event_text, const string& active_stream_url = string()) {
    auto repl = [&](const string& key, const string& val) {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != string::npos) {
            text.replace(pos, key.size(), val);
            pos += val.size();
        }
    };
    repl("$owner_name", cfg.owner_name);
    repl("$instance_name", cfg.instance_name);
    repl("$stream_url", cfg.stream_url);
    repl("$fallback_stream_url", cfg.fallback_stream_url);
    repl("$active_stream_url", active_stream_url.empty() ? cfg.stream_url : active_stream_url);
    repl("$time", now_iso());
    repl("$event_text", event_text);
    return text;
}

struct ProbeCtx {
    curl_off_t bytes = 0;
    curl_off_t min_bytes = 0;
    bool enough = false;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr;
    auto* ctx = static_cast<ProbeCtx*>(userdata);
    size_t total = size * nmemb;
    ctx->bytes += static_cast<curl_off_t>(total);
    if (ctx->bytes >= ctx->min_bytes) ctx->enough = true;
    return total;
}

static int xferinfo_cb(void* clientp, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProbeCtx*>(clientp);
    if (dlnow > ctx->bytes) ctx->bytes = dlnow;
    if (ctx->bytes >= ctx->min_bytes) {
        ctx->enough = true;
        return 1;
    }
    return 0;
}

struct ProbeResult {
    bool ok = false;
    long http_code = 0;
    long long bytes = 0;
    string detail;
};

static ProbeResult probe_stream_url(const Config& cfg, const string& url) {
    ProbeResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.detail = "curl_easy_init failed";
        return r;
    }

    ProbeCtx ctx;
    ctx.min_bytes = cfg.min_bytes;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Icy-MetaData: 1");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg.connect_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg.transfer_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "odr-audioenc-wrapper-msmtp/1.1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_code);
    r.bytes = static_cast<long long>(ctx.bytes);

    if ((rc == CURLE_OK || rc == CURLE_ABORTED_BY_CALLBACK) &&
        r.http_code >= 200 && r.http_code < 300 &&
        ctx.bytes >= cfg.min_bytes) {
        r.ok = true;
        r.detail = "HTTP " + to_string(r.http_code) + ", bytes=" + to_string(r.bytes);
    } else {
        r.ok = false;
        r.detail = string("probe failed: ") + curl_easy_strerror(rc) +
                   ", HTTP " + to_string(r.http_code) +
                   ", bytes=" + to_string(r.bytes);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

static ProbeResult probe_stream(const Config& cfg) {
    return probe_stream_url(cfg, cfg.stream_url);
}

static ProbeResult probe_fallback_stream(const Config& cfg) {
    if (cfg.fallback_stream_url.empty()) {
        ProbeResult r;
        r.ok = false;
        r.detail = "fallback not configured";
        return r;
    }
    return probe_stream_url(cfg, cfg.fallback_stream_url);
}

static void syslog_msg(const string& ident, int prio, const string& msg) {
    openlog(ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(prio, "%s", msg.c_str());
    closelog();
}

static bool send_mail_msmtp(const Config& cfg, const string& subject, const string& event_text, const string& active_stream_url = string()) {
    if (cfg.mail_from.empty() || cfg.mail_to.empty()) return false;

    string body;
    if (!cfg.email_header.empty()) {
        body += replace_vars(cfg.email_header, cfg, event_text, active_stream_url);
        body += "\n\n";
    }
    body += event_text;
    if (!cfg.email_footer.empty()) {
        body += "\n\n";
        body += replace_vars(cfg.email_footer, cfg, event_text, active_stream_url);
    }

    string subject_final = replace_vars(subject, cfg, event_text, active_stream_url);

    FILE* pipe = popen(cfg.msmtp_command.c_str(), "w");
    if (!pipe) return false;

    fprintf(pipe, "From: %s\n", cfg.mail_from.c_str());
    fprintf(pipe, "To: %s\n", cfg.mail_to.c_str());
    fprintf(pipe, "Subject: %s\n", subject_final.c_str());
    fprintf(pipe, "Content-Type: text/plain; charset=UTF-8\n");
    fprintf(pipe, "\n");
    if (!body.empty()) {
        fwrite(body.data(), 1, body.size(), pipe);
    }
    fprintf(pipe, "\n");

    int rc = pclose(pipe);
    if (rc == -1) return false;
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static void notify(const Config& cfg, const string& subject, const string& body, int fallback_prio = LOG_ERR, const string& active_stream_url = string()) {
    if (!send_mail_msmtp(cfg, subject, body, active_stream_url)) {
        syslog_msg(cfg.instance_name, fallback_prio,
                   string("mail delivery failed, fallback to syslog only; subject=") + subject + "; body=" + body);
    } else {
        syslog_msg(cfg.instance_name, LOG_INFO, string("notification sent: ") + subject);
    }
}


enum class ActiveSource {
    PRIMARY,
    FALLBACK
};

static const char* active_source_name(ActiveSource s) {
    return s == ActiveSource::PRIMARY ? "primary" : "fallback";
}

static const string& active_stream_url(const Config& cfg, ActiveSource s) {
    return s == ActiveSource::PRIMARY ? cfg.stream_url : cfg.fallback_stream_url;
}

static const string& active_audioenc_command(const Config& cfg, ActiveSource s) {
    return s == ActiveSource::PRIMARY ? cfg.audioenc_command : cfg.fallback_audioenc_command;
}

static pid_t start_audioenc_for_source(const Config& cfg, ActiveSource source) {
    const string& cmd = active_audioenc_command(cfg, source);
    const string& url = active_stream_url(cfg, source);
    pid_t pid = fork();
    if (pid < 0) throw runtime_error("fork() failed");
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    g_child_pid = pid;
    syslog_msg(cfg.instance_name, LOG_INFO,
               string("started odr-audioenc pid=") + to_string(pid) +
               " source=" + active_source_name(source) +
               " url=" + url);
    return pid;
}


static string wait_status_to_string(int status) {
    if (WIFEXITED(status)) return "exit_code=" + to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return "signal=" + to_string(WTERMSIG(status));
    return "unknown";
}


static void print_help(const char* prog) {
    cout
        << "Usage:\n"
        << "  " << prog << " <config.json>\n"
        << "  " << prog << " --test-config <config.json>\n"
        << "  " << prog << " --probe <config.json>\n"
        << "  " << prog << " --help\n\n"
        << "Options:\n"
        << "  --help         Show this help and exit.\n"
        << "  --test-config  Load and validate JSON config, then exit.\n"
        << "  --probe        Probe Icecast stream once and exit with status 0/1.\n\n"
        << "Notes:\n"
        << "  - audioenc_command should start with 'exec '.\n"
        << "  - Optional fallback_stream_url + fallback_audioenc_command can be used for automatic failover.\n"
        << "  - Wrapper performs a maintenance restart of odr-audioenc every day at 02:30.\n";
}

static bool should_run_maintenance_restart(int& last_maintenance_yday) {
    time_t t = time(nullptr);
    struct tm tmv {};
    localtime_r(&t, &tmv);

    if (tmv.tm_hour == 2 && tmv.tm_min >= 30 && tmv.tm_yday != last_maintenance_yday) {
        last_maintenance_yday = tmv.tm_yday;
        return true;
    }
    return false;
}

static void terminate_child_process(pid_t& child_pid, const string& ident) {
    if (child_pid <= 0) return;

    kill(child_pid, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        int status = 0;
        pid_t rc = waitpid(child_pid, &status, WNOHANG);
        if (rc == child_pid) {
            child_pid = -1;
            return;
        }
        this_thread::sleep_for(chrono::seconds(1));
    }

    if (child_pid > 0) {
        syslog_msg(ident, LOG_WARNING, "child did not stop after SIGTERM, sending SIGKILL");
        kill(child_pid, SIGKILL);
        waitpid(child_pid, nullptr, 0);
        child_pid = -1;
    }
}



int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const string arg1 = argv[1];
            if (arg1 == "--help" || arg1 == "-h") {
                print_help(argv[0]);
                return 0;
            }
        }

        if (argc == 3) {
            const string cmd = argv[1];
            const string cfg_path = argv[2];

            if (cmd == "--test-config") {
                Config cfg = load_config(cfg_path);
                cout << "Config OK\n";
                cout << "instance_name=" << cfg.instance_name << "\n";
                cout << "stream_url=" << cfg.stream_url << "\n";
                cout << "fallback_stream_url=" << (cfg.fallback_stream_url.empty() ? string("(not set)") : cfg.fallback_stream_url) << "\n";
                cout << "state_file=" << cfg.state_file << "\n";
                cout << "lock_file=" << cfg.lock_file << "\n";
                cout << "check_interval_sec=" << cfg.check_interval_sec << "\n";
                cout << "fail_threshold=" << cfg.fail_threshold << "\n";
                cout << "ok_threshold=" << cfg.ok_threshold << "\n";
                return 0;
            }

            if (cmd == "--probe") {
                curl_global_init(CURL_GLOBAL_DEFAULT);
                try {
                    Config cfg = load_config(cfg_path);
                    ProbeResult pr = probe_stream(cfg);
                    cout << "PRIMARY " << (pr.ok ? "OK" : "FAIL") << ": " << pr.detail << "\n";
                    if (!cfg.fallback_stream_url.empty()) {
                        ProbeResult fr = probe_fallback_stream(cfg);
                        cout << "FALLBACK " << (fr.ok ? "OK" : "FAIL") << ": " << fr.detail << "\n";
                    }
                    curl_global_cleanup();
                    return pr.ok ? 0 : 1;
                } catch (...) {
                    curl_global_cleanup();
                    throw;
                }
            }
        }

        if (argc != 2) {
            print_help(argv[0]);
            return 1;
        }

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);

        curl_global_init(CURL_GLOBAL_DEFAULT);
        Config cfg = load_config(argv[1]);

        int lock_fd = open(cfg.lock_file.c_str(), O_CREAT | O_RDWR, 0644);
        if (lock_fd < 0) throw runtime_error("Cannot open lock file: " + cfg.lock_file);
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            throw runtime_error("Another instance already running for lock file: " + cfg.lock_file);
        }

        syslog_msg(cfg.instance_name, LOG_INFO, "wrapper started for primary " + cfg.stream_url +
                                               (cfg.fallback_stream_url.empty() ? string() : " with fallback " + cfg.fallback_stream_url));

        string persisted_state = load_state_file(cfg.state_file);

        ProbeResult startup_primary = probe_stream(cfg);
        ProbeResult startup_fallback;
        if (!cfg.fallback_stream_url.empty()) {
            startup_fallback = probe_fallback_stream(cfg);
        }

        ActiveSource current_source = ActiveSource::PRIMARY;
        if (startup_primary.ok) {
            current_source = ActiveSource::PRIMARY;
            persisted_state = "RUNNING_PRIMARY";
        } else if (!cfg.fallback_stream_url.empty() && startup_fallback.ok) {
            current_source = ActiveSource::FALLBACK;
            persisted_state = "RUNNING_FALLBACK";
        } else {
            persisted_state = "DOWN";
        }
        save_state_file(cfg.state_file, persisted_state);

        vector<long> restart_times;
        pid_t child_pid = -1;
        bool primary_down_notified = !startup_primary.ok;
        bool cooldown_notified = false;
        long primary_down_since = startup_primary.ok ? 0 : epoch_now();
        long primary_fail_count = 0;
        long primary_ok_count = startup_primary.ok ? cfg.ok_threshold : 0;
        int last_maintenance_yday = -1;

        if (!startup_primary.ok) {
            if (!cfg.fallback_stream_url.empty() && startup_fallback.ok) {
                notify(cfg,
                       "[" + cfg.instance_name + "] Icecast source DOWN, switching to fallback",
                       "Detekovali jsme vypadek hlavniho Icecast streamu.\n"
                       "Cas: " + now_iso() + "\n"
                       "Primary URL: " + cfg.stream_url + "\n"
                       "Fallback URL: " + cfg.fallback_stream_url + "\n"
                       "Primary detail: " + startup_primary.detail + "\n"
                       "Fallback detail: " + startup_fallback.detail + "\n"
                       "Wrapper prepina odr-audioenc na fallback stream.",
                       LOG_ERR,
                       cfg.fallback_stream_url);
            } else if (!startup_primary.ok) {
                string body =
                    "Detekovali jsme vypadek hlavniho Icecast streamu.\n"
                    "Cas: " + now_iso() + "\n"
                    "Primary URL: " + cfg.stream_url + "\n"
                    "Primary detail: " + startup_primary.detail + "\n";
                if (!cfg.fallback_stream_url.empty()) {
                    body += "Fallback URL: " + cfg.fallback_stream_url + "\n"
                            "Fallback detail: " + startup_fallback.detail + "\n"
                            "Fallback stream neni dostupny, failover nelze aktivovat.\n";
                } else {
                    body += "Fallback stream neni nakonfigurovan.\n";
                }
                body += "odr-audioenc nebude spousten, dokud nebude dostupny primary nebo fallback stream.";
                notify(cfg,
                       "[" + cfg.instance_name + "] Icecast source DOWN",
                       body,
                       LOG_ERR);
            }
        }

        while (!g_stop) {
            if (child_pid > 0 && should_run_maintenance_restart(last_maintenance_yday)) {
                syslog_msg(cfg.instance_name, LOG_INFO,
                           string("maintenance restart scheduled at 02:30, restarting odr-audioenc on ") +
                           active_source_name(current_source));
                terminate_child_process(child_pid, cfg.instance_name);
                g_child_pid = -1;
                this_thread::sleep_for(chrono::seconds(cfg.restart_backoff_sec));
            }

            if (child_pid > 0) {
                int status = 0;
                pid_t rc = waitpid(child_pid, &status, WNOHANG);
                if (rc == child_pid) {
                    syslog_msg(cfg.instance_name, LOG_WARNING,
                               "odr-audioenc exited: " + wait_status_to_string(status) +
                               ", source=" + string(active_source_name(current_source)));
                    child_pid = -1;
                    g_child_pid = -1;

                    ProbeResult active_probe = (current_source == ActiveSource::PRIMARY)
                        ? probe_stream(cfg)
                        : probe_fallback_stream(cfg);

                    if (active_probe.ok) {
                        if (cfg.notify_on_audioenc_crash) {
                            notify(cfg,
                                   "[" + cfg.instance_name + "] odr-audioenc crashed, active stream still OK",
                                   "Proces odr-audioenc skoncil, ale aktivni zdrojovy stream je stale dostupny.\n"
                                   "Cas: " + now_iso() + "\n"
                                   "Source: " + string(active_source_name(current_source)) + "\n"
                                   "URL: " + active_stream_url(cfg, current_source) + "\n"
                                   "Detail probe: " + active_probe.detail + "\n"
                                   "Wrapper se pokusi encoder znovu spustit.",
                                   LOG_WARNING,
                                   active_stream_url(cfg, current_source));
                        }
                        this_thread::sleep_for(chrono::seconds(cfg.restart_backoff_sec));
                    } else {
                        if (current_source == ActiveSource::PRIMARY) {
                            if (!primary_down_notified) {
                                primary_down_since = epoch_now();
                                primary_fail_count = 0;
                                primary_ok_count = 0;
                                primary_down_notified = true;
                            }

                            if (!cfg.fallback_stream_url.empty()) {
                                ProbeResult fallback_probe = probe_fallback_stream(cfg);
                                if (fallback_probe.ok) {
                                    notify(cfg,
                                           "[" + cfg.instance_name + "] Icecast source DOWN, switching to fallback",
                                           "odr-audioenc skoncil a hlavni zdrojovy stream neni dostupny.\n"
                                           "Cas: " + now_iso() + "\n"
                                           "Primary URL: " + cfg.stream_url + "\n"
                                           "Fallback URL: " + cfg.fallback_stream_url + "\n"
                                           "Primary detail: " + active_probe.detail + "\n"
                                           "Fallback detail: " + fallback_probe.detail + "\n"
                                           "Wrapper prepina odr-audioenc na fallback stream.",
                                           LOG_ERR,
                                           cfg.fallback_stream_url);
                                    current_source = ActiveSource::FALLBACK;
                                    persisted_state = "RUNNING_FALLBACK";
                                    save_state_file(cfg.state_file, persisted_state);
                                } else {
                                    notify(cfg,
                                           "[" + cfg.instance_name + "] Icecast source DOWN",
                                           "odr-audioenc skoncil a hlavni zdrojovy stream neni dostupny.\n"
                                           "Cas: " + now_iso() + "\n"
                                           "Primary URL: " + cfg.stream_url + "\n"
                                           "Fallback URL: " + cfg.fallback_stream_url + "\n"
                                           "Primary detail: " + active_probe.detail + "\n"
                                           "Fallback detail: " + fallback_probe.detail + "\n"
                                           "Fallback stream neni dostupny, failover nelze aktivovat.",
                                           LOG_ERR);
                                    persisted_state = "DOWN";
                                    save_state_file(cfg.state_file, persisted_state);
                                }
                            } else {
                                notify(cfg,
                                       "[" + cfg.instance_name + "] Icecast source DOWN",
                                       "odr-audioenc skoncil a hlavni zdrojovy stream neni dostupny.\n"
                                       "Cas: " + now_iso() + "\n"
                                       "Primary URL: " + cfg.stream_url + "\n"
                                       "Detail: " + active_probe.detail + "\n"
                                       "Fallback stream neni nakonfigurovan.",
                                       LOG_ERR);
                                persisted_state = "DOWN";
                                save_state_file(cfg.state_file, persisted_state);
                            }
                        } else {
                            ProbeResult primary_probe = probe_stream(cfg);
                            if (primary_probe.ok) {
                                notify(cfg,
                                       "[" + cfg.instance_name + "] Primary stream restored, switching back",
                                       "Fallback encoder skoncil, ale hlavni stream je opet dostupny.\n"
                                       "Cas: " + now_iso() + "\n"
                                       "Primary URL: " + cfg.stream_url + "\n"
                                       "Fallback URL: " + cfg.fallback_stream_url + "\n"
                                       "Primary detail: " + primary_probe.detail + "\n"
                                       "Wrapper prepne odr-audioenc zpet na primary stream.",
                                       LOG_INFO,
                                       cfg.stream_url);
                                current_source = ActiveSource::PRIMARY;
                                primary_down_notified = false;
                                primary_down_since = 0;
                                primary_ok_count = 0;
                                primary_fail_count = 0;
                                persisted_state = "RUNNING_PRIMARY";
                                save_state_file(cfg.state_file, persisted_state);
                            } else {
                                syslog_msg(cfg.instance_name, LOG_WARNING,
                                           "fallback encoder exited and fallback stream probe failed: " + active_probe.detail);
                            }
                        }
                    }
                }
            }

            ProbeResult primary_probe = probe_stream(cfg);

            if (primary_probe.ok) {
                primary_fail_count = 0;
                ++primary_ok_count;
            } else {
                primary_ok_count = 0;
                ++primary_fail_count;
                syslog_msg(cfg.instance_name, LOG_WARNING, "primary stream probe failed: " + primary_probe.detail);
            }

            if (current_source == ActiveSource::PRIMARY) {
                if (child_pid < 0) {
                    if (primary_probe.ok) {
                        bool ready_to_start_primary = !primary_down_notified || (primary_ok_count >= cfg.ok_threshold);

                        if (primary_down_notified && primary_ok_count >= cfg.ok_threshold) {
                            long duration = primary_down_since > 0 ? (epoch_now() - primary_down_since) : 0;
                            string body =
                                "Hlavni zdrojovy stream je opet dostupny.\n"
                                "Cas: " + now_iso() + "\n"
                                "Primary URL: " + cfg.stream_url + "\n"
                                "Doba vypadku primary: " + format_duration(duration) + "\n"
                                "Primary detail: " + primary_probe.detail + "\n";
                            if (!cfg.fallback_stream_url.empty()) {
                                body += "Fallback URL: " + cfg.fallback_stream_url + "\n";
                            } else {
                                body += "Fallback stream nebyl nakonfigurovan.\n";
                            }
                            body += "Wrapper znovu spusti odr-audioenc na primary streamu.";
                            notify(cfg,
                                   "[" + cfg.instance_name + "] Primary stream restored",
                                   body,
                                   LOG_INFO,
                                   cfg.stream_url);
                            primary_down_notified = false;
                            primary_down_since = 0;
                            primary_fail_count = 0;
                            restart_times.clear();
                        }

                        if (!ready_to_start_primary) {
                            this_thread::sleep_for(chrono::seconds(cfg.check_interval_sec));
                            continue;
                        }

                        long now = epoch_now();
                        restart_times.erase(
                            remove_if(restart_times.begin(), restart_times.end(),
                                      [&](long t) { return now - t > cfg.restart_limit_window_sec; }),
                            restart_times.end());

                        if (static_cast<long>(restart_times.size()) >= cfg.restart_limit_count) {
                            if (!cooldown_notified) {
                                notify(cfg,
                                       "[" + cfg.instance_name + "] encoder restart limit reached",
                                       "Byl dosazen limit restartu odr-audioenc.\n"
                                       "Cas: " + now_iso() + "\n"
                                       "URL: " + cfg.stream_url + "\n"
                                       "Limit: " + to_string(cfg.restart_limit_count) + " restartu za " +
                                           to_string(cfg.restart_limit_window_sec) + " s.\n"
                                       "Wrapper pocka " + to_string(cfg.restart_cooldown_sec) +
                                           " s, nez znovu zkusi start.",
                                       LOG_WARNING,
                                       cfg.stream_url);
                                cooldown_notified = true;
                            }
                            this_thread::sleep_for(chrono::seconds(cfg.restart_cooldown_sec));
                            restart_times.clear();
                            cooldown_notified = false;
                        } else {
                            child_pid = start_audioenc_for_source(cfg, ActiveSource::PRIMARY);
                            restart_times.push_back(now);
                            persisted_state = "RUNNING_PRIMARY";
                            save_state_file(cfg.state_file, persisted_state);
                        }
                    } else if (primary_fail_count >= cfg.fail_threshold) {
                        if (!primary_down_notified) {
                            primary_down_since = epoch_now();
                            primary_down_notified = true;
                            string body =
                                "Detekovali jsme vypadek hlavniho Icecast streamu.\n"
                                "Cas: " + now_iso() + "\n"
                                "Primary URL: " + cfg.stream_url + "\n"
                                "Primary detail: " + primary_probe.detail + "\n";
                            bool switched = false;
                            if (!cfg.fallback_stream_url.empty()) {
                                ProbeResult fallback_probe = probe_fallback_stream(cfg);
                                body += "Fallback URL: " + cfg.fallback_stream_url + "\n"
                                        "Fallback detail: " + fallback_probe.detail + "\n";
                                if (fallback_probe.ok) {
                                    body += "Wrapper prepina odr-audioenc na fallback stream.";
                                    current_source = ActiveSource::FALLBACK;
                                    persisted_state = "RUNNING_FALLBACK";
                                    switched = true;
                                    save_state_file(cfg.state_file, persisted_state);
                                    notify(cfg,
                                           "[" + cfg.instance_name + "] Icecast source DOWN, switching to fallback",
                                           body,
                                           LOG_ERR,
                                           cfg.fallback_stream_url);
                                } else {
                                    body += "Fallback stream neni dostupny, failover nelze aktivovat.";
                                    persisted_state = "DOWN";
                                    save_state_file(cfg.state_file, persisted_state);
                                    notify(cfg,
                                           "[" + cfg.instance_name + "] Icecast source DOWN",
                                           body,
                                           LOG_ERR);
                                }
                            } else {
                                body += "Fallback stream neni nakonfigurovan.";
                                persisted_state = "DOWN";
                                save_state_file(cfg.state_file, persisted_state);
                                notify(cfg,
                                       "[" + cfg.instance_name + "] Icecast source DOWN",
                                       body,
                                       LOG_ERR);
                            }
                            if (switched) {
                                primary_fail_count = 0;
                            }
                        }
                    }
                }
            } else {
                // We are currently running the fallback source.  The decision to switch
                // back must depend on the actual source state and consecutive successful
                // primary probes, not on primary_down_notified only.  In older builds the
                // flag could become false while current_source stayed FALLBACK, which made
                // failback impossible even though --probe showed PRIMARY OK.
                if (primary_probe.ok && primary_ok_count >= cfg.ok_threshold) {
                    long duration = primary_down_since > 0 ? (epoch_now() - primary_down_since) : 0;
                    notify(cfg,
                           "[" + cfg.instance_name + "] Primary stream restored, switching back",
                           "Hlavni zdrojovy stream je opet dostupny.\n"
                           "Cas: " + now_iso() + "\n"
                           "Primary URL: " + cfg.stream_url + "\n"
                           "Fallback URL: " + cfg.fallback_stream_url + "\n"
                           "Doba vypadku primary: " + format_duration(duration) + "\n"
                           "Primary detail: " + primary_probe.detail + "\n"
                           "Wrapper prepne odr-audioenc zpet na primary stream.",
                           LOG_INFO,
                           cfg.stream_url);

                    if (child_pid > 0) {
                        terminate_child_process(child_pid, cfg.instance_name);
                        g_child_pid = -1;
                        child_pid = -1;
                        this_thread::sleep_for(chrono::seconds(cfg.restart_backoff_sec));
                    }

                    current_source = ActiveSource::PRIMARY;
                    primary_down_notified = false;
                    primary_down_since = 0;
                    primary_ok_count = 0;
                    primary_fail_count = 0;
                    restart_times.clear();
                    persisted_state = "RUNNING_PRIMARY";
                    save_state_file(cfg.state_file, persisted_state);
                }

                if (child_pid < 0) {
                    ProbeResult fallback_probe = probe_fallback_stream(cfg);
                    if (fallback_probe.ok) {
                        long now = epoch_now();
                        restart_times.erase(
                            remove_if(restart_times.begin(), restart_times.end(),
                                      [&](long t) { return now - t > cfg.restart_limit_window_sec; }),
                            restart_times.end());

                        if (static_cast<long>(restart_times.size()) >= cfg.restart_limit_count) {
                            if (!cooldown_notified) {
                                notify(cfg,
                                       "[" + cfg.instance_name + "] encoder restart limit reached on fallback",
                                       "Byl dosazen limit restartu odr-audioenc na fallback streamu.\n"
                                       "Cas: " + now_iso() + "\n"
                                       "Fallback URL: " + cfg.fallback_stream_url + "\n"
                                       "Limit: " + to_string(cfg.restart_limit_count) + " restartu za " +
                                           to_string(cfg.restart_limit_window_sec) + " s.\n"
                                       "Wrapper pocka " + to_string(cfg.restart_cooldown_sec) +
                                           " s, nez znovu zkusi start.",
                                       LOG_WARNING,
                                       cfg.fallback_stream_url);
                                cooldown_notified = true;
                            }
                            this_thread::sleep_for(chrono::seconds(cfg.restart_cooldown_sec));
                            restart_times.clear();
                            cooldown_notified = false;
                        } else {
                            child_pid = start_audioenc_for_source(cfg, ActiveSource::FALLBACK);
                            restart_times.push_back(now);
                            persisted_state = "RUNNING_FALLBACK";
                            save_state_file(cfg.state_file, persisted_state);
                        }
                    } else {
                        syslog_msg(cfg.instance_name, LOG_WARNING, "fallback stream probe failed: " + fallback_probe.detail);
                        if (primary_down_notified && primary_down_since == 0) {
                            primary_down_since = epoch_now();
                        }
                    }
                }
            }

            for (long i = 0; i < cfg.check_interval_sec && !g_stop; ++i) {
                this_thread::sleep_for(chrono::seconds(1));
            }
        }

        if (child_pid > 0) {
            terminate_child_process(child_pid, cfg.instance_name);
        }

        syslog_msg(cfg.instance_name, LOG_INFO, "wrapper stopped");
        curl_global_cleanup();
        return 0;
    } catch (const exception& e) {
        cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
