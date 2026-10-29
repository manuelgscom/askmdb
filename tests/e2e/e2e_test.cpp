// askmdb end-to-end tests: the real binary in a pseudo-terminal or with pipes, inside a sandbox
// (temporary HOME and XDG_*, NO_COLOR, controlled PATH, fake curl). Answers come from FakeEngine
// (ASKMDB_FAKE_*).
//
// Suites:
//   (default)  no server needed: usage, URI, database and connection errors, --schema-from
//   mongo      the `mongo` service with the humo seed: ASKMDB_E2E_URI=mongodb://127.0.0.1:27018/humo
//   tls        the `mongo-tls` service: ASKMDB_E2E_TLS_URI=mongodb://lector:lector-juguete@127.0.0.1:27019/
//              humo?tls=true&tlsCAFile=<repo>/tests/mongo/tls/ca.pem
// Without their variable, the mongo and tls cases are skipped with a message.

#include "doctest.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pty_session.hpp"

namespace fs = std::filesystem;
using askmdb::e2e::ExitStatus;
using askmdb::e2e::PipeResult;
using askmdb::e2e::PtySession;
using json = nlohmann::json;

namespace {

// Key fragments of the Spanish interface (LANG=es_ES.UTF-8 in the sandbox).
const char* kPromptRead = "[S/n/c(opiar)]";
const char* kPromptWarn = "[s/N/c(opiar)]";
const char* kPromptReadEn = "[Y/n/c(opy)]";
const char* kAsk = "¿Ejecutar?";
const char* kLevelRead = "solo lectura";
const char* kLevelWarn = "aviso";
const char* kLevelBlocked = "bloqueada";
const char* kCancelled = "Cancelado";
const char* kCopied = "Copiado al portapapeles";
const char* kMoreRows = "hay más filas";
const char* kUriPasswordWarning = "historial";

// Hosts that never answer: connection refused at once, and a black hole (TEST-NET-1).
const char* kRefusedUri = "mongodb://127.0.0.1:1/humo";
const char* kBlackHoleHost = "192.0.2.1:27017";

// humo seed: C-0001's email appears only in the result rows, never in the pipeline or the schema.
const std::string kRowMarker = "0001@correo.test";
const std::string kReadPipeline = R"([{"$match":{"_id":"C-0001"}},{"$project":{"email":1}}])";
// An unknown field path: "aviso", and still harmless to run.
const std::string kWarnPipeline =
    R"([{"$match":{"_id":"C-0001","campo_fantasma":{"$exists":false}}},{"$project":{"email":1}}])";
const std::string kBlockedPipeline = R"([{"$match":{"_id":"C-0001"}},{"$out":"e2e_out"}])";
// Read-only and slow on purpose: 50 documents x 2000 x 1000 = 1e8 unwound rows (~3 s locally).
const std::string kSlowPipeline =
    R"([{"$project":{"a":{"$range":[0,2000]}}},{"$unwind":"$a"},{"$project":{"b":{"$range":[0,1000]}}},)"
    R"({"$unwind":"$b"},{"$group":{"_id":null,"n":{"$sum":1}}}])";
// Same, ~2.5 times longer, for Ctrl-C during the execution.
const std::string kSlowerPipeline =
    R"([{"$project":{"a":{"$range":[0,5000]}}},{"$unwind":"$a"},{"$project":{"b":{"$range":[0,1000]}}},)"
    R"({"$unwind":"$b"},{"$group":{"_id":null,"n":{"$sum":1}}}])";

constexpr int kTimeoutMs = 20000;

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool exited_with(const ExitStatus& s, int code) { return s.exited && s.code == code; }

// Ctrl-C: exit code 130, or killed by SIGINT (the shell shows 130 as well).
bool interrupted(const ExitStatus& s) { return exited_with(s, 130) || (s.signaled && s.signal == SIGINT); }

std::optional<std::string> env_value(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

// "mongodb://host/db" -> "mongodb://user:password@host/db".
std::string with_credentials(const std::string& uri, const std::string& user, const std::string& password) {
    const std::string scheme = "://";
    const auto at = uri.find(scheme);
    REQUIRE(at != std::string::npos);
    return uri.substr(0, at + scheme.size()) + user + ":" + password + "@" + uri.substr(at + scheme.size());
}

json parse_json(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const std::exception& e) {
        FAIL_CHECK("stdout is not one JSON object: " << e.what() << "\n" << text);
        return json::object();
    }
}

// Own temporary directory with HOME, XDG dirs, bin/ and a controlled environment. Nothing of the
// test runner's environment is inherited.
struct Sandbox {
    std::string dir;
    std::map<std::string, std::string> vars;

    Sandbox() {
        std::string tmpl = (fs::temp_directory_path() / "askmdb-e2e-XXXXXX").string();
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        dir = tmpl;
        fs::create_directories(dir + "/bin");
        fs::create_directories(dir + "/.config/askmdb");
        vars = {
            {"HOME", dir},
            {"XDG_CONFIG_HOME", dir + "/.config"},
            {"XDG_CACHE_HOME", dir + "/.cache"},
            {"XDG_DATA_HOME", dir + "/.local/share"},
            {"TMPDIR", dir},
            {"NO_COLOR", "1"},
            {"LANG", "es_ES.UTF-8"},
            {"TERM", "xterm"},
            {"PATH", dir + "/bin:/usr/bin:/bin"},
        };
        // A fake curl: any download attempt is logged to curl.log and never reaches the network.
        fs::copy_file(std::string(ASKMDB_E2E_DIR) + "/fake_curl.sh", path("bin/curl"));
        fs::permissions(path("bin/curl"), fs::perms::owner_all);
        vars["FAKE_CURL_SRC"] = path("src.bin");
        vars["FAKE_CURL_LOG"] = path("curl.log");
        write_file("src.bin", "not a model");
    }
    ~Sandbox() {
        std::error_code ec;
        // Undo chmods so that everything can be removed.
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
        fs::remove_all(dir, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    std::string path(const std::string& name) const { return dir + "/" + name; }
    bool exists(const std::string& name) const { return fs::exists(path(name)); }

    void write_file(const std::string& name, const std::string& text) const {
        fs::create_directories(fs::path(path(name)).parent_path());
        std::ofstream(path(name), std::ios::binary) << text;
    }
    void config(const std::string& text, fs::perms mode = fs::perms::owner_read | fs::perms::owner_write) const {
        write_file(".config/askmdb/config", text);
        fs::permissions(path(".config/askmdb/config"), mode);
    }

    // Fake program in <sandbox>/bin.
    void fake_bin(const std::string& name, const std::string& script) const {
        write_file("bin/" + name, script);
        fs::permissions(path("bin/" + name), fs::perms::owner_all);
    }

    void fake_query(const std::string& collection, const std::string& pipeline,
                    const std::string& explanation = "Explicación de prueba.") {
        vars.erase("ASKMDB_FAKE_ERROR");
        vars["ASKMDB_FAKE_TYPE"] = "query";
        vars["ASKMDB_FAKE_COLLECTION"] = collection;
        vars["ASKMDB_FAKE_PIPELINE"] = pipeline;
        vars["ASKMDB_FAKE_EXPLANATION"] = explanation;
    }
    void fake_other(const std::string& type, const std::string& explanation) {
        vars.erase("ASKMDB_FAKE_COLLECTION");
        vars.erase("ASKMDB_FAKE_PIPELINE");
        vars["ASKMDB_FAKE_TYPE"] = type;
        vars["ASKMDB_FAKE_EXPLANATION"] = explanation;
    }

    std::vector<std::string> env() const {
        std::vector<std::string> out;
        for (const auto& [k, v] : vars) out.push_back(k + "=" + v);
        return out;
    }

    std::vector<std::string> argv(const std::vector<std::string>& args) const {
        std::vector<std::string> out{ASKMDB_BIN};
        out.insert(out.end(), args.begin(), args.end());
        return out;
    }

    std::unique_ptr<PtySession> launch(const std::vector<std::string>& args) const {
        return std::make_unique<PtySession>(argv(args), env());
    }

    PipeResult pipes(const std::vector<std::string>& args, const std::string& input = "",
                     int timeout_ms = kTimeoutMs) const {
        return askmdb::e2e::run_with_pipes(argv(args), env(), input, timeout_ms);
    }

    std::string cache_dir() const { return path(".cache/askmdb"); }

    // The schema cache files (schema-*), excluding temporaries.
    std::vector<std::string> cache_files() const {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cache_dir(), ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("schema-", 0) == 0) out.push_back(e.path().string());
        }
        return out;
    }

    // Every file under the sandbox whose content contains `needle` (the config file excluded).
    std::vector<std::string> files_containing(const std::string& needle) const {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string p = it->path().string();
            if (p == path(".config/askmdb/config")) continue;
            if (contains(read_file(p), needle)) out.push_back(p);
        }
        return out;
    }
};

std::int64_t now_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

unsigned mode_of(const std::string& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return static_cast<unsigned>(st.st_mode & 07777);
}

// The single schema cache file of a sandbox (after one run against one database).
std::string single_cache_file(const Sandbox& sb) {
    const auto files = sb.cache_files();
    REQUIRE_MESSAGE(files.size() == 1, "expected one schema-* file in " << sb.cache_dir());
    return files.front();
}

std::int64_t sampled_at(const std::string& cache_file) {
    const json j = parse_json(read_file(cache_file));
    REQUIRE_MESSAGE(j.contains("sampled_at"), "no sampled_at in " << cache_file);
    return j["sampled_at"].get<std::int64_t>();
}

void set_sampled_at(const std::string& cache_file, std::int64_t value) {
    json j = parse_json(read_file(cache_file));
    j["sampled_at"] = value;
    std::ofstream(cache_file, std::ios::binary | std::ios::trunc) << j.dump();
}

// ASKMDB_E2E_URI, or nullopt (and the case is skipped with a message).
std::optional<std::string> mongo_uri() {
    auto uri = env_value("ASKMDB_E2E_URI");
    if (!uri) MESSAGE("ASKMDB_E2E_URI is not set: skipped (e.g. mongodb://127.0.0.1:27018/humo)");
    return uri;
}

std::optional<std::string> tls_uri() {
    auto uri = env_value("ASKMDB_E2E_TLS_URI");
    if (!uri) MESSAGE("ASKMDB_E2E_TLS_URI is not set: skipped");
    return uri;
}

// A sandbox already pointed at the humo database through ASKMDB_URI.
struct HumoSandbox : Sandbox {
    explicit HumoSandbox(const std::string& uri) { vars["ASKMDB_URI"] = uri; }
};

}  // namespace

// ============================================================================================
// Default suite: no server needed.
// ============================================================================================

TEST_CASE("--version, --help, no question and unknown options") {
    Sandbox sb;
    const auto v = sb.pipes({"--version"});
    CHECK(exited_with(v.status, 0));
    CHECK(v.out.rfind("askmdb ", 0) == 0);

    const auto help = sb.pipes({"--help"});
    CHECK(exited_with(help.status, 0));
    CHECK(contains(help.out, "askmdb"));
    CHECK(contains(help.out, "--uri"));

    const auto none = sb.pipes({});
    CHECK(exited_with(none.status, 2));
    CHECK(contains(none.err, "askmdb"));

    const auto bad = sb.pipes({"--nope", "pedidos"});
    CHECK(exited_with(bad.status, 2));
    CHECK(contains(bad.err, "--nope"));

    const auto bad_mode = sb.pipes({"--mode", "siempre", "pedidos"});
    CHECK(exited_with(bad_mode.status, 2));

    const auto missing_value = sb.pipes({"pedidos", "--db"});
    CHECK(exited_with(missing_value.status, 2));
    CHECK_FALSE(sb.exists("curl.log"));
}

TEST_CASE("question longer than 1000 bytes: code 2 without connecting") {
    Sandbox sb;
    sb.vars["ASKMDB_URI"] = std::string("mongodb://") + kBlackHoleHost + "/humo";
    sb.fake_query("pedidos", kReadPipeline);
    const auto start = std::chrono::steady_clock::now();
    const auto r = sb.pipes({std::string(1001, 'a')});
    CHECK(exited_with(r.status, 2));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("no URI or no database: code 2") {
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    const auto no_uri = sb.pipes({"pedidos de hoy"});
    CHECK(exited_with(no_uri.status, 2));
    CHECK(contains(no_uri.err, "ASKMDB_URI"));

    const auto no_db = sb.pipes({"--uri", std::string("mongodb://") + kBlackHoleHost, "pedidos de hoy"});
    CHECK(exited_with(no_db.status, 2));
    CHECK(contains(no_db.err, "--db"));

    sb.vars["ASKMDB_URI"] = std::string("mongodb://") + kBlackHoleHost + "/";
    const auto env_no_db = sb.pipes({"pedidos de hoy"});
    CHECK(exited_with(env_no_db.status, 2));
}

TEST_CASE("admin, local and config databases: code 3 before connecting") {
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    for (const char* db : {"admin", "local", "config"}) {
        CAPTURE(db);
        const auto start = std::chrono::steady_clock::now();
        const auto by_flag =
            sb.pipes({"--uri", std::string("mongodb://") + kBlackHoleHost + "/humo", "--db", db, "pedidos"});
        CHECK(exited_with(by_flag.status, 3));
        sb.vars["ASKMDB_URI"] = std::string("mongodb://") + kBlackHoleHost + "/" + db;
        const auto by_uri = sb.pipes({"pedidos"});
        CHECK(exited_with(by_uri.status, 3));
        const auto as_json = sb.pipes({"--json", "pedidos"});
        CHECK(exited_with(as_json.status, 3));
        CHECK(parse_json(as_json.out).value("executed", true) == false);
        sb.vars.erase("ASKMDB_URI");
        // The black hole would take 5 s to time out: these never tried to connect.
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
    }
}

TEST_CASE("config with a password in uri must be 0600") {
    const std::string password = "clave-config-e2e";
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    const std::string text = "uri = mongodb://e2e:" + password + "@127.0.0.1:1/humo\n";

    sb.config(text, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
    const auto open = sb.pipes({"pedidos"});
    CHECK(exited_with(open.status, 1));
    CHECK(contains(open.err, "0600"));
    CHECK_FALSE(contains(open.out + open.err, password));

    // With 0600 it starts and gets as far as the (refused) connection.
    sb.config(text);
    const auto ok = sb.pipes({"pedidos"});
    CHECK(exited_with(ok.status, 4));
    CHECK_FALSE(contains(ok.out + ok.err, password));

    // Without a password, any mode is fine.
    sb.config("uri = " + std::string(kRefusedUri) + "\n",
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
    CHECK(exited_with(sb.pipes({"pedidos"}).status, 4));
}

TEST_CASE("invalid config: code 1 with the line number") {
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    sb.vars["ASKMDB_URI"] = kRefusedUri;
    sb.config("# comentario\nmode = siempre\n");
    const auto r = sb.pipes({"pedidos"});
    CHECK(exited_with(r.status, 1));
    CHECK(contains(r.err, "config:2:"));
}

TEST_CASE("--uri with a password: warning, and the password never shows") {
    const std::string user = "usuario-e2e";
    const std::string password = "clave-juguete-e2e";
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    const std::string uri = with_credentials(kRefusedUri, user, password);

    const auto text = sb.pipes({"--uri", uri, "pedidos"});
    CHECK(exited_with(text.status, 4));
    CHECK(contains(text.err, kUriPasswordWarning));
    CHECK_FALSE(contains(text.out + text.err, password));
    CHECK_FALSE(contains(text.out + text.err, user + ":"));

    const auto as_json = sb.pipes({"--json", "--uri", uri, "pedidos"});
    CHECK(exited_with(as_json.status, 4));
    CHECK_FALSE(contains(as_json.out + as_json.err, password));
    CHECK(sb.files_containing(password).empty());

    // ASKMDB_URI with a password: no warning.
    sb.vars["ASKMDB_URI"] = uri;
    const auto env = sb.pipes({"pedidos"});
    CHECK(exited_with(env.status, 4));
    CHECK_FALSE(contains(env.err, kUriPasswordWarning));
    CHECK_FALSE(contains(env.out + env.err, password));
}

TEST_CASE("unreachable server: code 4 in under 6 s") {
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    for (const std::string& uri : {std::string(kRefusedUri), std::string("mongodb://") + kBlackHoleHost + "/humo"}) {
        CAPTURE(uri);
        sb.vars["ASKMDB_URI"] = uri;
        const auto start = std::chrono::steady_clock::now();
        const auto r = sb.pipes({"pedidos de hoy"});
        CHECK(exited_with(r.status, 4));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(6));
        CHECK_FALSE(r.err.empty());

        const auto j = sb.pipes({"--json", "pedidos de hoy"});
        CHECK(exited_with(j.status, 4));
        const json obj = parse_json(j.out);
        CHECK(obj.contains("error"));
        CHECK_FALSE(obj.value("error", json()).is_null());
        CHECK(obj.value("executed", true) == false);
    }
    CHECK_FALSE(sb.exists("curl.log"));
}

TEST_CASE("--schema-from: an invalid block is code 2; a broken model answer is code 1") {
    Sandbox sb;
    sb.write_file("bad.txt", "db=humo\n## pedidos\nestado str [\"a\", \"b\"]\n");
    const auto bad = sb.pipes({"--json", "--mode", "suggest", "--schema-from", sb.path("bad.txt"), "pedidos"});
    CHECK(exited_with(bad.status, 2));
    CHECK(parse_json(bad.out).value("error", "") == "schema_from");
    const auto missing = sb.pipes({"--schema-from", sb.path("missing.txt"), "--print-schema"});
    CHECK(exited_with(missing.status, 2));
    CHECK(missing.out.empty());

    // A model answer that does not parse: code 1 and nothing runs. The URI is resolved (auto may
    // run) but the black hole is never reached, so it is fast.
    sb.write_file("s.txt", "db=humo\n## pedidos\n_id oid\nestado str\n");
    sb.vars["ASKMDB_URI"] = std::string("mongodb://") + kBlackHoleHost + "/humo";
    sb.vars["ASKMDB_FAKE_RAW"] = R"({"type":"query","collection":"pedidos","pipeline":[{"$match")";
    const auto start = std::chrono::steady_clock::now();
    const auto broken = sb.pipes({"--json", "--mode", "auto", "--schema-from", sb.path("s.txt"), "pedidos"});
    CHECK(exited_with(broken.status, 1));
    const json j = parse_json(broken.out);
    CHECK_FALSE(j.value("error", json()).is_null());
    CHECK(j.value("executed", true) == false);
    CHECK(j["rows"].is_null());
    CHECK(j.value("raw", "") == sb.vars["ASKMDB_FAKE_RAW"]);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));

    // Same as text in a terminal (confirm): the honest message, no question.
    auto s = sb.launch({"--schema-from", sb.path("s.txt"), "pedidos"});
    CHECK(s->wait_for("no he podido generar una respuesta", kTimeoutMs));
    const auto st = s->wait_exit(kTimeoutMs);
    CHECK(exited_with(st, 1));
    CHECK_FALSE(contains(s->output(), kAsk));
    CHECK_FALSE(sb.exists("curl.log"));
}

TEST_CASE("Ctrl-C while connecting: 130") {
    Sandbox sb;
    sb.fake_query("pedidos", kReadPipeline);
    sb.vars["ASKMDB_URI"] = std::string("mongodb://") + kBlackHoleHost + "/humo";
    auto s = sb.launch({"pedidos de hoy"});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (const auto early = s->poll_exit()) {
        MESSAGE("askmdb ended before Ctrl-C (code " << early->code << "): the black hole host answered");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    s->write("\x03");
    const auto st = s->wait_exit(5000);
    CHECK(interrupted(st));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

// --- First start: device and model of the family (ASKMDB_TEST_HW, fake curl, FakeEngine) -------

namespace {

const std::string kStatePath = ".local/state/askmdb/choice";
const std::string kModelsPath = ".local/share/askmdb/models/";

// FakeEngine (unsure) plus the download hooks: fake curl writes src.bin ("not a model").
struct FamilySandbox : Sandbox {
    explicit FamilySandbox(const std::string& hw) {
        vars["ASKMDB_TEST_HW"] = hw;
        vars["ASKMDB_TEST_MODEL_URL"] = "https://example.invalid/m.gguf";
        vars["ASKMDB_TEST_MODEL_SIZE"] = "11";
        vars["ASKMDB_TEST_MODEL_SHA256"] = "708811ccb1510c6d6c6e6379ef09be39bdbb0e7edcf44fefcca21c6228ee6d89";
        fake_other("unsure", "No lo sé con este esquema.");
        write_file("s.txt", "db=humo\n## pedidos\n_id oid\nestado str\n");
    }
    std::vector<std::string> ask() const { return {"--mode", "suggest", "--schema-from", path("s.txt"), "pedidos"}; }
    std::vector<std::string> ask_json() const {
        return {"--json", "--mode", "suggest", "--schema-from", path("s.txt"), "pedidos"};
    }
    int curl_calls() const {
        if (!exists("curl.log")) return 0;
        std::istringstream in(read_file(path("curl.log")));
        int n = 0;
        for (std::string line; std::getline(in, line);) ++n;
        return n;
    }
};

}  // namespace

TEST_CASE("first start: the proposal follows the hardware; declining downloads nothing") {
    struct Case {
        const char* hw;
        const char* proposal;
        bool gpu;
    };
    // With >= 8 GiB free the user picks 4B or 9B instead ("Elige 1 o 2"), and "n" means neither.
    for (const Case& c : {Case{"discrete:RTX:12000:16000", "¿Cuál quieres instalar?", true},
                          Case{"discrete:RTX:6000:8000", "Recomendado: askmdb-4b en la GPU RTX", true},
                          Case{"integrated:Intel:3000:3000", "Recomendado: askmdb-0.8b en la CPU", false},
                          Case{"none", "Recomendado: askmdb-0.8b en la CPU", false}}) {
        CAPTURE(c.hw);
        FamilySandbox sb(c.hw);
        auto s = sb.launch(sb.ask());
        REQUIRE(s->wait_for(c.proposal, kTimeoutMs));
        REQUIRE(s->wait_for(std::string(c.hw).find("12000") != std::string::npos ? "n = ninguno): " : "[S/n]",
                            kTimeoutMs));
        s->write("n\n");
        if (c.gpu) {
            REQUIRE(s->wait_for("¿Prefieres askmdb-0.8b en la CPU", kTimeoutMs));
            s->write("n\n");
        }
        CHECK(exited_with(s->wait_exit(kTimeoutMs), 1));
        CHECK(contains(s->output(), "no se ha elegido ningún modelo"));
        CHECK(sb.curl_calls() == 0);
        CHECK_FALSE(sb.exists(kStatePath));
    }
}

TEST_CASE("first start: accept downloads, checks and saves; the second run asks nothing") {
    FamilySandbox sb("discrete:RTX:6000:8000");
    auto s = sb.launch(sb.ask());
    REQUIRE(s->wait_for("¿Descargarlo ahora? [S/n]", kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), "Comprobando que el modelo funciona en la GPU RTX"));
    CHECK(contains(s->output(), "Elección guardada: askmdb-4b en la GPU RTX"));
    CHECK(contains(s->output(), "No lo sé"));
    CHECK(sb.curl_calls() == 1);
    CHECK(contains(read_file(sb.path("curl.log")), "https://example.invalid/m.gguf"));
    CHECK(sb.exists(kModelsPath + "askmdb-4b-1.0.0-Q4_K_M.gguf"));
    const std::string state = read_file(sb.path(kStatePath));
    CHECK(contains(state, "device = gpu\n"));
    CHECK(contains(state, "model = askmdb-4b\n"));
    CHECK(contains(state, "gpu_id = Vulkan0\n"));
    CHECK(mode_of(sb.path(".local/state/askmdb")) == 0700);

    // Second run, in a terminal, with the GPU now second and less free memory: no dialog, no
    // download (the saved GPU is found by its name).
    sb.vars["ASKMDB_TEST_HW"] = "integrated:Intel:1000:2000;discrete:RTX:1000:8000";
    auto again = sb.launch(sb.ask());
    CHECK(exited_with(again->wait_exit(kTimeoutMs), 0));
    CHECK_FALSE(contains(again->output(), "[S/n]"));
    CHECK(contains(again->output(), "No lo sé"));
    const auto j = sb.pipes(sb.ask_json());
    CHECK(exited_with(j.status, 0));
    const json r = parse_json(j.out);
    CHECK(r.value("device", "") == "gpu");
    CHECK(r.value("model_name", "") == "askmdb-4b");
    CHECK(sb.curl_calls() == 1);
}

TEST_CASE("first start with >= 8 GiB free: the user picks the 4B and only that one is downloaded") {
    FamilySandbox sb("discrete:RTX:12000:16000");
    auto s = sb.launch(sb.ask());
    REQUIRE(s->wait_for("  1) askmdb-4b", kTimeoutMs));
    REQUIRE(s->wait_for("un pelín más lento, pero más preciso", kTimeoutMs));
    REQUIRE(s->wait_for("Elige 1 o 2 (Intro = 1, n = ninguno): ", kTimeoutMs));
    s->write("1\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK_FALSE(contains(s->output(), "[S/n]"));
    CHECK(contains(s->output(), "Elección guardada: askmdb-4b en la GPU RTX"));
    CHECK(sb.curl_calls() == 1);
    CHECK(sb.exists(kModelsPath + "askmdb-4b-1.0.0-Q4_K_M.gguf"));
    CHECK_FALSE(sb.exists(kModelsPath + "askmdb-9b-1.0.0-Q4_K_M.gguf"));
    CHECK(contains(read_file(sb.path(kStatePath)), "model = askmdb-4b\n"));
}

TEST_CASE("first start: GPU declined, askmdb-0.8b on the CPU accepted") {
    FamilySandbox sb("discrete:RTX:12000:16000");
    auto s = sb.launch(sb.ask());
    REQUIRE(s->wait_for("Elige 1 o 2 (Intro = 1, n = ninguno): ", kTimeoutMs));
    s->write("n\n");
    REQUIRE(s->wait_for("¿Prefieres askmdb-0.8b en la CPU", kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(sb.curl_calls() == 1);
    CHECK(sb.exists(kModelsPath + "askmdb-0.8b-1.0.0-Q4_K_M.gguf"));
    CHECK_FALSE(sb.exists(kModelsPath + "askmdb-9b-1.0.0-Q4_K_M.gguf"));
    CHECK(contains(read_file(sb.path(kStatePath)), "device = cpu\nmodel = askmdb-0.8b\n"));
}

TEST_CASE("first start: the GPU fails the check, askmdb-0.8b on the CPU after asking") {
    FamilySandbox sb("discrete:RTX:6000:8000");
    sb.vars["ASKMDB_TEST_GPU_ERROR"] = "gpu_unavailable";
    auto s = sb.launch(sb.ask());
    REQUIRE(s->wait_for("¿Descargarlo ahora? [S/n]", kTimeoutMs));
    s->write("\n");
    REQUIRE(s->wait_for("el modelo no funciona en la GPU (gpu_unavailable)", kTimeoutMs));
    REQUIRE(s->wait_for("¿Prefieres askmdb-0.8b en la CPU", kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(sb.curl_calls() == 2);
    CHECK(contains(read_file(sb.path(kStatePath)), "device = cpu\n"));
}

TEST_CASE("first start without a terminal: no download, an error and no state") {
    FamilySandbox sb("discrete:RTX:12000:16000");
    const auto text = sb.pipes(sb.ask());
    CHECK(exited_with(text.status, 1));
    CHECK(contains(text.err, "askmdb-4b-1.0.0-Q4_K_M.gguf"));  // the default with >= 8 GiB
    const auto j = sb.pipes(sb.ask_json());
    CHECK(exited_with(j.status, 1));
    CHECK(parse_json(j.out).value("error", "") == "model_not_downloaded");
    CHECK(sb.curl_calls() == 0);
    CHECK_FALSE(sb.exists(kStatePath));

    // askmdb-0.8b already there: the CPU with it, still nothing saved.
    sb.write_file(kModelsPath + "askmdb-0.8b-1.0.0-Q4_K_M.gguf", "not a model");
    const auto cpu = sb.pipes(sb.ask_json());
    CHECK(exited_with(cpu.status, 0));
    const json r = parse_json(cpu.out);
    CHECK(r.value("device", "") == "cpu");
    CHECK(r.value("model_name", "") == "askmdb-0.8b");
    CHECK(sb.curl_calls() == 0);
    CHECK_FALSE(sb.exists(kStatePath));
}

TEST_CASE("--detect in a terminal: a failed download keeps the previous choice") {
    FamilySandbox sb("discrete:RTX:6000:8000");
    sb.write_file(kStatePath, "device = cpu\nmodel = askmdb-0.8b\n");
    sb.vars["ASKMDB_TEST_MODEL_SHA256"] = std::string(64, '0');  // the download fails its check
    auto s = sb.launch({"--detect"});
    REQUIRE(s->wait_for("¿Descargarlo ahora? [S/n]", kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 1));
    CHECK(sb.curl_calls() == 1);
    CHECK(read_file(sb.path(kStatePath)) == "device = cpu\nmodel = askmdb-0.8b\n");
}

TEST_CASE("--detect --json: the hardware and the choice, nothing saved") {
    FamilySandbox sb("discrete:RTX 4080:12000:16000;integrated:Intel:1000:2000");
    const auto r = sb.pipes({"--detect", "--json"});
    CHECK(exited_with(r.status, 0));
    CHECK(r.err.empty());
    const json j = parse_json(r.out);
    for (const char* k : {"vulkan", "gpus", "ram_total_mib", "ram_available_mib", "cores", "choice"})
        CHECK(j.contains(k));
    CHECK(j.size() == 6);
    CHECK(j["vulkan"] == true);
    REQUIRE(j["gpus"].size() == 2);
    CHECK(j["gpus"][0]["id"] == "Vulkan0");
    CHECK(j["gpus"][0]["name"] == "RTX 4080");
    CHECK(j["gpus"][0]["kind"] == "discrete");
    CHECK(j["gpus"][0]["free_mib"] == 12000);
    CHECK(j["gpus"][0]["total_mib"] == 16000);
    CHECK(j["gpus"][1]["kind"] == "integrated");
    CHECK(j["choice"] == json::parse(R"({"device":"gpu","gpu":"Vulkan0","model":"askmdb-9b"})"));
    CHECK_FALSE(sb.exists(kStatePath));
    CHECK(sb.curl_calls() == 0);

    const auto v = sb.pipes({"--version"});
    CHECK((contains(v.out, "\nVulkan: yes\n") || contains(v.out, "\nVulkan: no\n")));
}

// ============================================================================================
// Suite mongo: the `mongo` service with the humo seed.
// ============================================================================================

TEST_SUITE("mongo") {

TEST_CASE("suggest: shows, never asks or runs; blocked is code 3") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = suggest\n");

    sb.fake_query("clientes", kReadPipeline);
    auto s = sb.launch({"email de C-0001"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), "db.clientes.aggregate("));
    CHECK(contains(s->output(), kLevelRead));
    CHECK_FALSE(contains(s->output(), kAsk));
    CHECK_FALSE(contains(s->output(), kRowMarker));

    sb.fake_query("clientes", kWarnPipeline);
    s = sb.launch({"email de C-0001"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kLevelWarn));
    CHECK(contains(s->output(), "campo_fantasma"));
    CHECK_FALSE(contains(s->output(), kAsk));
    CHECK_FALSE(contains(s->output(), kRowMarker));

    sb.fake_query("clientes", kBlockedPipeline);
    s = sb.launch({"copia los clientes"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 3));
    CHECK(contains(s->output(), kLevelBlocked));
    CHECK(contains(s->output(), "$out"));
    CHECK_FALSE(contains(s->output(), kAsk));
}

TEST_CASE("confirm + read: Enter runs, n cancels") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.fake_query("clientes", kReadPipeline);

    auto s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    CHECK(contains(s->output(), kLevelRead));
    CHECK_FALSE(contains(s->output(), kRowMarker));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));

    s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    s->write("n\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kCancelled));
    CHECK_FALSE(contains(s->output(), kRowMarker));
}

TEST_CASE("confirm + warn: Enter cancels, s runs") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.fake_query("clientes", kWarnPipeline);

    auto s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptWarn, kTimeoutMs));
    CHECK(contains(s->output(), kLevelWarn));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kCancelled));
    CHECK_FALSE(contains(s->output(), kRowMarker));

    s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptWarn, kTimeoutMs));
    s->write("s\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));
}

TEST_CASE("confirm + blocked: reason, no question, code 3") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.fake_query("clientes", kBlockedPipeline);
    auto s = sb.launch({"copia los clientes"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 3));
    CHECK(contains(s->output(), kLevelBlocked));
    CHECK_FALSE(contains(s->output(), kAsk));

    // It never ran: no e2e_out collection appears in a fresh sampling.
    const auto schema = sb.pipes({"--print-schema", "--refresh"});
    CHECK(exited_with(schema.status, 0));
    CHECK_FALSE(contains(schema.out, "e2e_out"));
}

TEST_CASE("auto: read runs without asking; warn asks; blocked never runs") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = auto\n");

    sb.fake_query("clientes", kReadPipeline);
    auto s = sb.launch({"email de C-0001"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));
    CHECK_FALSE(contains(s->output(), kAsk));

    sb.fake_query("clientes", kWarnPipeline);
    s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptWarn, kTimeoutMs));
    CHECK_FALSE(contains(s->output(), kRowMarker));
    s->write("s\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));

    s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptWarn, kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK_FALSE(contains(s->output(), kRowMarker));

    sb.fake_query("clientes", kBlockedPipeline);
    s = sb.launch({"copia los clientes"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 3));
    CHECK_FALSE(contains(s->output(), kAsk));

    // --mode on the command line overrides the config.
    sb.fake_query("clientes", kReadPipeline);
    s = sb.launch({"--mode", "confirm", "email de C-0001"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    s->write("n\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
}

TEST_CASE("English interface") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("lang = en\n");
    sb.fake_query("clientes", kReadPipeline);
    auto s = sb.launch({"email of C-0001"});
    REQUIRE(s->wait_for(kPromptReadEn, kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));
}

TEST_CASE("without a TTY nothing runs (text output)") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = auto\n");

    sb.fake_query("clientes", kReadPipeline, "Email del cliente C-0001.");
    const auto read = sb.pipes({"email de C-0001"}, "s\n");
    CHECK(exited_with(read.status, 0));
    CHECK(contains(read.out, "db.clientes.aggregate("));
    CHECK(contains(read.out, "Email del cliente C-0001."));
    CHECK_FALSE(contains(read.out, kRowMarker));
    CHECK_FALSE(contains(read.out, "\x1b["));

    sb.fake_query("clientes", kWarnPipeline);
    const auto warn = sb.pipes({"email de C-0001"}, "s\n");
    CHECK(exited_with(warn.status, 0));
    CHECK_FALSE(contains(warn.out, kRowMarker));

    sb.fake_query("clientes", kBlockedPipeline);
    const auto blocked = sb.pipes({"copia los clientes"}, "s\n");
    CHECK(exited_with(blocked.status, 3));
}

TEST_CASE("--json: runs only read-only in auto; every field present") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);

    sb.fake_query("clientes", kReadPipeline, "Email del cliente C-0001.");
    const auto r = sb.pipes({"--json", "--mode", "auto", "email de C-0001"});
    CHECK(exited_with(r.status, 0));
    const json j = parse_json(r.out);
    for (const char* key : {"contract_version", "askmdb_version", "server", "question", "type", "collection",
                            "pipeline", "mongosh", "explanation", "level", "reasons", "executed", "rows",
                            "truncated", "error", "model", "raw", "tokens", "timings_ms", "prefix_cache"}) {
        CAPTURE(key);
        CHECK(j.contains(key));
    }
    CHECK(j.value("type", "") == "query");
    CHECK(j.value("collection", "") == "clientes");
    CHECK(j.value("level", "") == "read");
    CHECK(j.value("executed", false) == true);
    CHECK(j.value("truncated", true) == false);
    CHECK(j.value("question", "") == "email de C-0001");
    CHECK(contains(j.value("mongosh", ""), "db.clientes.aggregate("));
    REQUIRE(j.contains("rows"));
    REQUIRE(j["rows"].is_array());
    CHECK(j["rows"].size() == 1);
    CHECK(contains(j["rows"].dump(), kRowMarker));
    CHECK(j["error"].is_null());
    // FakeEngine: the engine was called, but there is no model file, raw text or prefix state.
    CHECK(j["model"].is_null());
    CHECK(j.value("raw", "x") == "");
    CHECK(j["tokens"].is_object());
    CHECK(j["timings_ms"].is_object());
    CHECK(j.value("prefix_cache", "") == "off");
    if (j.contains("server")) {
        CHECK(j["server"].value("db", "") == "humo");
        CHECK(j["server"].contains("hosts"));
    }

    // Warning: shown, never run.
    sb.fake_query("clientes", kWarnPipeline);
    const auto w = sb.pipes({"--json", "--mode", "auto", "email de C-0001"});
    CHECK(exited_with(w.status, 0));
    const json jw = parse_json(w.out);
    CHECK(jw.value("level", "") == "warn");
    CHECK(jw.value("executed", true) == false);
    CHECK_FALSE(jw.value("reasons", json::array()).empty());
    CHECK_FALSE(contains(w.out, kRowMarker));

    // Blocked: code 3, the object is still written.
    sb.fake_query("clientes", kBlockedPipeline);
    const auto b = sb.pipes({"--json", "--mode", "auto", "copia los clientes"});
    CHECK(exited_with(b.status, 3));
    const json jb = parse_json(b.out);
    CHECK(jb.value("level", "") == "blocked");
    CHECK(jb.value("executed", true) == false);
    CHECK_FALSE(jb.value("reasons", json::array()).empty());

    // confirm: --json never asks, so it never runs.
    sb.fake_query("clientes", kReadPipeline);
    const auto c = sb.pipes({"--json", "--mode", "confirm", "email de C-0001"}, "s\n");
    CHECK(exited_with(c.status, 0));
    const json jc = parse_json(c.out);
    CHECK(jc.value("level", "") == "read");
    CHECK(jc.value("executed", true) == false);
    CHECK_FALSE(contains(c.out, kRowMarker));
    CHECK_FALSE(sb.exists("curl.log"));
}

TEST_CASE("copy: c copies the mongosh text and runs nothing") {
    const auto uri = mongo_uri();
    if (!uri) return;
    {
        HumoSandbox sb(*uri);
        sb.fake_query("clientes", kReadPipeline);
        sb.vars["WAYLAND_DISPLAY"] = "e2e";
        sb.fake_bin("wl-copy", "#!/bin/sh\ncat > '" + sb.path("clip.txt") + "'\n");
        auto s = sb.launch({"email de C-0001"});
        REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
        s->write("c\n");
        CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
        CHECK(contains(s->output(), kCopied));
        const std::string clip = read_file(sb.path("clip.txt"));
        CHECK(contains(clip, "db.clientes.aggregate("));
        CHECK(contains(clip, "C-0001"));
        CHECK_FALSE(contains(s->output(), kRowMarker));
    }
    {
        // No graphical session: OSC 52 to the terminal.
        HumoSandbox sb(*uri);
        sb.fake_query("clientes", kWarnPipeline);
        auto s = sb.launch({"email de C-0001"});
        REQUIRE(s->wait_for(kPromptWarn, kTimeoutMs));
        s->write("c\n");
        CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
        CHECK(contains(s->output(), "\x1b]52;c;"));
        CHECK(contains(s->output(), kCopied));
        CHECK_FALSE(contains(s->output(), kRowMarker));
    }
}

TEST_CASE("refuse and unsure: shown without a question, code 0") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.fake_other("refuse", "Eso no se puede preguntar a esta base.");
    auto s = sb.launch({"borra los pedidos"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), "Eso no se puede preguntar a esta base."));
    CHECK_FALSE(contains(s->output(), kAsk));

    sb.fake_other("unsure", "No sé a qué te refieres con «lo de ayer».");
    s = sb.launch({"lo de ayer"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), "No sé a qué te refieres"));
    CHECK_FALSE(contains(s->output(), kAsk));

    const auto j = sb.pipes({"--json", "--mode", "auto", "lo de ayer"});
    CHECK(exited_with(j.status, 0));
    const json obj = parse_json(j.out);
    CHECK(obj.value("type", "") == "unsure");
    CHECK(obj.value("executed", true) == false);
}

TEST_CASE("more rows than max_rows") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = auto\nmax_rows = 3\n");
    sb.fake_query("pedidos", R"([{"$sort":{"_id":1}},{"$project":{"estado":1}}])");

    auto s = sb.launch({"estados de los pedidos"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kMoreRows));

    const auto r = sb.pipes({"--json", "estados de los pedidos"});
    CHECK(exited_with(r.status, 0));
    const json j = parse_json(r.out);
    CHECK(j.value("truncated", false) == true);
    REQUIRE(j["rows"].is_array());
    CHECK(j["rows"].size() == 3);

    // Exactly max_rows rows: not truncated.
    sb.fake_query("pedidos", R"([{"$sort":{"_id":1}},{"$limit":3},{"$project":{"estado":1}}])");
    const auto exact = sb.pipes({"--json", "tres pedidos"});
    CHECK(exited_with(exact.status, 0));
    const json je = parse_json(exact.out);
    CHECK(je.value("truncated", true) == false);
    CHECK(je["rows"].size() == 3);
}

TEST_CASE("--print-schema: the humo block") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    const auto r = sb.pipes({"--print-schema"});
    CHECK(exited_with(r.status, 0));
    const std::string& b = r.out;
    CHECK(b.rfind("db=humo\n", 0) == 0);
    for (const char* header : {"## ancha\n", "## clientes\n", "## pedidos\n", "## pedidos_madrid\n", "## productos\n"}) {
        CAPTURE(header);
        CHECK(contains(b, header));
    }
    CHECK(b.find("## ancha") < b.find("## clientes"));
    CHECK(b.find("## pedidos\n") < b.find("## pedidos_madrid"));
    CHECK(contains(b, "\n_id oid"));
    CHECK(contains(b, "\n_id str"));
    CHECK(contains(b, "\ncliente.ciudad str"));
    CHECK(contains(b, "\nitems[].sku str"));
    CHECK(contains(b, "\nitems[].cantidad int"));
    CHECK(contains(b, "\nimporte double"));
    CHECK(contains(b, "\nprecio decimal"));
    CHECK(contains(b, "\ncupon? str"));
    CHECK(contains(b, "\ncodigo_postal? int|str"));
    CHECK(contains(b, "\ndescuento double|null"));
    CHECK(contains(b, "\nf01 int"));
    // Caps: at most 40 paths (f60 is past them) and depth 4.
    CHECK_FALSE(contains(b, "\nf60 "));
    CHECK_FALSE(contains(b, "n1.n2.n3.n4.n5"));
    // Personal-looking values are never listed.
    CHECK(contains(b, "\nemail str"));
    CHECK_FALSE(contains(b, "@correo.test"));
    CHECK_FALSE(contains(b, "+34 "));
    CHECK_FALSE(contains(b, "system.views"));

    // Deterministic: a second sampling prints the same block.
    const auto again = sb.pipes({"--print-schema", "--refresh"});
    CHECK(exited_with(again.status, 0));
    CHECK(again.out == b);

    // -c keeps only the chosen collections.
    const auto one = sb.pipes({"--print-schema", "-c", "clientes"});
    CHECK(exited_with(one.status, 0));
    CHECK(contains(one.out, "## clientes\n"));
    CHECK_FALSE(contains(one.out, "## pedidos"));
}

TEST_CASE("parity: --print-schema, --schema-from and --print-prompt") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    // 1. The block of humo, saved as a snapshot (it is also the versioned smoke snapshot).
    const auto schema = sb.pipes({"--print-schema"});
    REQUIRE(exited_with(schema.status, 0));
    sb.write_file("s.txt", schema.out);
    CHECK(schema.out == read_file(std::string(ASKMDB_TESTS_DIR) + "/smoke/esquema-humo.txt"));

    // 2. Read back without a URI (nothing to connect to): byte for byte the same block.
    Sandbox offline;
    const auto again = offline.pipes({"--schema-from", sb.path("s.txt"), "--print-schema"});
    CHECK(exited_with(again.status, 0));
    CHECK(again.out == schema.out);
    CHECK_FALSE(fs::exists(offline.cache_dir()));

    // 3-4. The prompt from the snapshot is the prompt from the cache, in Spanish, English and with
    // Unicode spaces (normalized the same way on both sides).
    for (const std::string q : {"q", "pedidos de hoy en Madrid", "top 5 products by price",
                                "  clientes\xC2\xA0" "de\xE3\x80\x80Sevilla \xE2\x80\x83",
                                "¿cuántos pedidos\xE2\x80\xAF" "tiene «C-0001»?\tsin cancelar"}) {
        CAPTURE(q);
        const auto cached = sb.pipes({"--print-prompt", q});
        const auto from_file = offline.pipes({"--schema-from", sb.path("s.txt"), "--print-prompt", q});
        CHECK(exited_with(cached.status, 0));
        CHECK(exited_with(from_file.status, 0));
        CHECK(cached.out == from_file.out);
        // The chat template trims the system turn: the block's last "\n" goes before <|im_end|>.
        CHECK(contains(cached.out, schema.out.substr(0, schema.out.size() - 1) + "<|im_end|>\n"));
        CHECK(cached.out.size() > 2);
        CHECK(cached.out.substr(cached.out.size() - 2) == "\n\n");
    }
    CHECK_FALSE(fs::exists(offline.cache_dir()));
    CHECK_FALSE(sb.exists("curl.log"));
}

TEST_CASE("schema cache: permissions, fresh, stale, --refresh, corrupt, unreadable") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    const auto first = sb.pipes({"--print-schema"});
    REQUIRE(exited_with(first.status, 0));
    const std::string file = single_cache_file(sb);
    CHECK(mode_of(sb.cache_dir()) == 0700);
    CHECK(mode_of(file) == 0600);
    const std::int64_t t0 = sampled_at(file);
    CHECK(t0 > now_seconds() - 120);

    // Fresh: used as is (the file is not rewritten).
    const std::string before = read_file(file);
    const auto cached = sb.pipes({"--print-schema"});
    CHECK(exited_with(cached.status, 0));
    CHECK(cached.out == first.out);
    CHECK(read_file(file) == before);

    // Stale (8 days old): sampled again.
    const std::int64_t old = now_seconds() - 8 * 24 * 3600;
    set_sampled_at(file, old);
    const auto stale = sb.pipes({"--print-schema"});
    CHECK(exited_with(stale.status, 0));
    CHECK(stale.out == first.out);
    CHECK(sampled_at(file) > old + 7 * 24 * 3600);

    // 1 day old: still fresh, unless --refresh.
    const std::int64_t recent = now_seconds() - 24 * 3600;
    set_sampled_at(file, recent);
    CHECK(exited_with(sb.pipes({"--print-schema"}).status, 0));
    CHECK(sampled_at(file) == recent);
    CHECK(exited_with(sb.pipes({"--print-schema", "--refresh"}).status, 0));
    CHECK(sampled_at(file) > recent);

    // Corrupt: sampled again and rewritten.
    sb.write_file(file.substr(sb.dir.size() + 1), "{esto no es json");
    const auto corrupt = sb.pipes({"--print-schema"});
    CHECK(exited_with(corrupt.status, 0));
    CHECK(corrupt.out == first.out);
    CHECK(sampled_at(file) > now_seconds() - 120);

    // Another format version: sampled again.
    {
        json j = parse_json(read_file(file));
        j["format_version"] = 999;
        std::ofstream(file, std::ios::binary | std::ios::trunc) << j.dump();
    }
    CHECK(exited_with(sb.pipes({"--print-schema"}).status, 0));
    CHECK(parse_json(read_file(file)).value("format_version", 0) != 999);

    // Unreadable: sampled again (replaced through rename).
    if (::geteuid() != 0) {
        fs::permissions(file, fs::perms::none);
        const auto unreadable = sb.pipes({"--print-schema"});
        CHECK(exited_with(unreadable.status, 0));
        CHECK(unreadable.out == first.out);
    }
    CHECK(sb.files_containing("@correo.test").empty());
}

TEST_CASE("schema that does not fit: code 2 with the cost list; -c fixes it") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = suggest\n");
    sb.fake_query("g01", R"([{"$match":{"_id":1}}])");
    // humo_grande: 10 collections of 40 long top-level paths (~14 KB, nothing left to trim).
    const auto r = sb.pipes({"--db", "humo_grande", "indicadores"});
    CHECK(exited_with(r.status, 2));
    for (int i = 1; i <= 10; ++i) {
        const std::string name = std::string(i < 10 ? "g0" : "g") + std::to_string(i);
        CAPTURE(name);
        CHECK(contains(r.err, name));
    }
    CHECK(contains(r.err, "tokens"));
    CHECK(contains(r.err, "-c"));

    const auto fixed = sb.pipes({"--db", "humo_grande", "-c", "g01", "indicadores"});
    CHECK(exited_with(fixed.status, 0));
    CHECK(contains(fixed.out, "db.g01.aggregate("));
}

TEST_CASE("budget trimming: value lists go first, from the last path; paths stay") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    // humo_grande.valores: 30 string paths with ~12 KB of value lists in total.
    const auto r = sb.pipes({"--print-schema", "--db", "humo_grande", "-c", "valores"});
    CHECK(exited_with(r.status, 0));
    for (int i = 1; i <= 30; ++i) {
        const std::string path = std::string(i < 10 ? "\nv0" : "\nv") + std::to_string(i) + " str";
        CAPTURE(path);
        CHECK(contains(r.out, path));
    }
    CHECK(contains(r.out, "\nv01 str [\""));
    CHECK(contains(r.out, "\nv30 str\n"));
    CHECK(r.out.size() < 12000);

    // Deterministic: the same block from a fresh sampling.
    const auto again = sb.pipes({"--print-schema", "--refresh", "--db", "humo_grande", "-c", "valores"});
    CHECK(again.out == r.out);
}

TEST_CASE("more than 50 collections without -c: code 2") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = suggest\n");
    sb.fake_query("c01", R"([{"$match":{"_id":1}}])");
    const auto r = sb.pipes({"--db", "humo_muchas", "algo"});
    CHECK(exited_with(r.status, 2));
    CHECK(contains(r.err, "-c"));
    CHECK(contains(r.err, "51"));

    const auto fixed = sb.pipes({"--db", "humo_muchas", "-c", "c01,c02", "algo"});
    CHECK(exited_with(fixed.status, 0));
    sb.config("mode = suggest\ncollections = c01\n");
    CHECK(exited_with(sb.pipes({"--db", "humo_muchas", "algo"}).status, 0));
}

TEST_CASE("maxTimeMS exceeded: code 4") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.config("mode = auto\nmax_time_ms = 100\n");
    sb.fake_query("ancha", kSlowPipeline);

    const auto start = std::chrono::steady_clock::now();
    auto s = sb.launch({"cuenta mucho"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 4));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));

    const auto j = sb.pipes({"--json", "cuenta mucho"});
    CHECK(exited_with(j.status, 4));
    const json obj = parse_json(j.out);
    CHECK_FALSE(obj.value("error", json()).is_null());
    CHECK(obj.value("executed", true) == false);
}

TEST_CASE("Ctrl-C at the question and during the execution: 130") {
    const auto uri = mongo_uri();
    if (!uri) return;
    HumoSandbox sb(*uri);
    sb.fake_query("clientes", kReadPipeline);
    auto s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    s->write("\x03");
    CHECK(interrupted(s->wait_exit(kTimeoutMs)));
    CHECK_FALSE(contains(s->output(), kRowMarker));

    sb.fake_query("ancha", kSlowerPipeline);
    s = sb.launch({"cuenta muchísimo"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    s->write("\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto start = std::chrono::steady_clock::now();
    s->write("\x03");
    CHECK(interrupted(s->wait_exit(kTimeoutMs)));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("credentials never show (failed authentication)") {
    const auto uri = mongo_uri();
    if (!uri) return;
    const std::string user = "usuario-e2e";
    const std::string password = "clave-juguete-e2e";
    Sandbox sb;
    sb.vars["ASKMDB_URI"] = with_credentials(*uri, user, password);
    sb.fake_query("clientes", kReadPipeline);

    auto s = sb.launch({"email de C-0001"});
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 4));
    CHECK_FALSE(contains(s->output(), password));
    CHECK_FALSE(contains(s->output(), user + ":"));

    const auto j = sb.pipes({"--json", "--mode", "auto", "email de C-0001"});
    CHECK(exited_with(j.status, 4));
    CHECK_FALSE(contains(j.out + j.err, password));
    CHECK_FALSE(contains(j.out + j.err, user + ":"));
    CHECK(sb.files_containing(password).empty());
}

TEST_CASE("every humo case of tests/verify/lectura.jsonl runs without a server error") {
    const auto uri = mongo_uri();
    if (!uri) return;
    const std::string cases_path = std::string(ASKMDB_TESTS_DIR) + "/verify/lectura.jsonl";
    if (!fs::exists(cases_path)) {
        MESSAGE(cases_path << " does not exist yet: skipped");
        return;
    }
    HumoSandbox sb(*uri);
    std::ifstream in(cases_path);
    std::string line;
    int lineno = 0;
    int ran = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        json c;
        try {
            c = json::parse(line);
        } catch (const std::exception& e) {
            FAIL_CHECK("lectura.jsonl:" << lineno << ": not JSON: " << e.what());
            continue;
        }
        if (c.value("schema", "") != "humo") continue;
        if (c.contains("db") && c["db"].is_string() && c["db"] != "humo") continue;
        const std::string pipeline = c["pipeline"].is_string() ? c["pipeline"].get<std::string>() : c["pipeline"].dump();
        const std::string collection = c.value("collection", "");
        CAPTURE(lineno);
        CAPTURE(pipeline);
        sb.fake_query(collection, pipeline);
        const auto r = sb.pipes({"--json", "--mode", "auto", "caso de lectura"});
        CHECK(exited_with(r.status, 0));
        const json j = parse_json(r.out);
        CHECK(j.value("level", "") == "read");
        CHECK(j.value("executed", false) == true);
        CHECK(j.value("error", json()).is_null());
        ++ran;
    }
    MESSAGE("lectura.jsonl: " << ran << " humo cases run against the server");
    CHECK(ran > 0);
}

}  // TEST_SUITE("mongo")

// ============================================================================================
// Suite tls: the `mongo-tls` service (TLS + SCRAM-SHA-256, user with role read).
// ============================================================================================

TEST_SUITE("tls") {

TEST_CASE("TLS + SCRAM: --print-schema and one execution; the password never shows") {
    const auto uri = tls_uri();
    if (!uri) return;
    const std::string password = "lector-juguete";
    Sandbox sb;
    sb.vars["ASKMDB_URI"] = *uri;

    const auto schema = sb.pipes({"--print-schema"});
    CHECK(exited_with(schema.status, 0));
    CHECK(schema.out.rfind("db=humo\n", 0) == 0);
    CHECK(contains(schema.out, "## pedidos\n"));
    CHECK(contains(schema.out, "## pedidos_madrid\n"));
    CHECK_FALSE(contains(schema.out + schema.err, password));

    sb.fake_query("clientes", kReadPipeline);
    const auto r = sb.pipes({"--json", "--mode", "auto", "email de C-0001"});
    CHECK(exited_with(r.status, 0));
    const json j = parse_json(r.out);
    CHECK(j.value("executed", false) == true);
    CHECK(contains(r.out, kRowMarker));
    CHECK_FALSE(contains(r.out + r.err, password));

    auto s = sb.launch({"email de C-0001"});
    REQUIRE(s->wait_for(kPromptRead, kTimeoutMs));
    s->write("\n");
    CHECK(exited_with(s->wait_exit(kTimeoutMs), 0));
    CHECK(contains(s->output(), kRowMarker));
    CHECK_FALSE(contains(s->output(), password));

    CHECK(sb.files_containing(password).empty());

    // Wrong password: authentication error, code 4, nothing leaks.
    const std::string bad = "clave-mala-e2e";
    std::string wrong = *uri;
    const auto at = wrong.find(password);
    REQUIRE(at != std::string::npos);
    wrong.replace(at, password.size(), bad);
    sb.vars["ASKMDB_URI"] = wrong;
    const auto denied = sb.pipes({"--print-schema", "--refresh"});
    CHECK(exited_with(denied.status, 4));
    CHECK_FALSE(contains(denied.out + denied.err, bad));
}

}  // TEST_SUITE("tls")
