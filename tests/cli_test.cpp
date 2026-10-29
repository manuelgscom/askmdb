#include "doctest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "cli/args.hpp"
#include "cli/cli.hpp"
#include "cli/flow.hpp"
#include "cli/i18n.hpp"
#include "cli/model.hpp"
#include "cli/report.hpp"
#include "cli/ui.hpp"
#include "config/state.hpp"
#include "fetch/model_info.hpp"
#include "infer/fake_engine.hpp"
#include "infer/hw.hpp"
#include "infer/prompt.hpp"
#include "infer/vk_loader.hpp"
#include "infer/vk_loader.hpp"
#include "mongo/mongo.hpp"
#include "schema/schema.hpp"

using namespace askmdb::cli;
using askmdb::config::Lang;
using askmdb::config::Mode;
using askmdb::infer::Suggestion;
using askmdb::verify::Assessment;
using askmdb::verify::Level;
using askmdb::verify::Reason;
using Json = nlohmann::ordered_json;

namespace {

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// Saves an environment variable and restores it when going out of scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) old_ = v;
    }
    ~EnvGuard() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }
    void set(const std::string& v) { setenv(name_, v.c_str(), 1); }
    void unset() { unsetenv(name_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

Suggestion query(const std::string& collection, const std::string& pipeline = R"([{"$match":{"a":1}}])") {
    Suggestion s;
    s.kind = Suggestion::Kind::Query;
    s.collection = collection;
    s.pipeline = askmdb::verify::load_pipeline(pipeline);
    s.explanation = "Explica.";
    return s;
}

// Level from the collection name, so as not to depend on the real verifier.
Assessment fake_verify(const Suggestion& s) {
    Assessment a;
    if (s.collection == "BLOCKED") {
        a.level = Level::Blocked;
        a.reasons.push_back({"B-OUT", "escribe en otra colección", "writes to another collection"});
    } else if (s.collection == "WARN") {
        a.level = Level::Warn;
        a.reasons.push_back({"W-FIELD", "campo que no está en el esquema", "field not in the schema"});
    }
    return a;
}

struct FakeWorld {
    std::deque<std::optional<std::string>> input;
    std::string screen;
    std::vector<std::string> executed, copied;
    int exec_code = 0;
    bool copy_ok = true;

    Io io() {
        Io io;
        io.write = [this](std::string_view t) { screen += t; };
        io.read_line = [this]() -> std::optional<std::string> {
            if (input.empty()) return std::nullopt;
            auto v = input.front();
            input.pop_front();
            return v;
        };
        io.verify = fake_verify;
        io.execute = [this](const Suggestion& s) {
            executed.push_back(s.collection);
            return exec_code;
        };
        io.copy = [this](const std::string& t) {
            copied.push_back(t);
            return copy_ok;
        };
        return io;
    }

    int go(const std::string& collection, Mode mode = Mode::Confirm, bool interactive = true, UiLang lang = UiLang::Es) {
        FlowOptions opt;
        opt.mode = mode;
        opt.lang = lang;
        opt.interactive = interactive;
        return run_flow(query(collection), "db." + collection + ".aggregate([])", opt, io());
    }
};

// Runs the whole program in-process with a captured terminal.
struct Term {
    std::string out, err;
    std::deque<std::string> input;
    bool tty = false;

    int run(const std::vector<std::string>& args) {
        Host h;
        h.out = [this](std::string_view t) { out += t; };
        h.err = [this](std::string_view t) { err += t; };
        h.read_line = [this]() -> std::optional<std::string> {
            if (input.empty()) return std::nullopt;
            auto v = input.front();
            input.pop_front();
            return v;
        };
        h.copy = [](const std::string&) { return true; };
        h.stdin_tty = h.stdout_tty = h.stderr_tty = tty;
        return askmdb::cli::run(args, h);
    }
};

// Clean environment for in-process runs: temporary config and cache, no URI, no fake engine.
struct Sandbox {
    std::filesystem::path dir;
    EnvGuard xdg_config{"XDG_CONFIG_HOME"}, xdg_cache{"XDG_CACHE_HOME"}, xdg_data{"XDG_DATA_HOME"},
        uri{"ASKMDB_URI"}, lc_all{"LC_ALL"}, lc_messages{"LC_MESSAGES"}, lang{"LANG"}, type{"ASKMDB_FAKE_TYPE"},
        pipeline{"ASKMDB_FAKE_PIPELINE"}, error{"ASKMDB_FAKE_ERROR"}, test_model{"ASKMDB_TEST_MODEL_URL"},
        raw{"ASKMDB_FAKE_RAW"}, collection{"ASKMDB_FAKE_COLLECTION"}, explanation{"ASKMDB_FAKE_EXPLANATION"},
        timeout{"ASKMDB_TEST_TIMEOUT_MS"}, debug{"ASKMDB_DEBUG"}, xdg_state{"XDG_STATE_HOME"},
        test_hw{"ASKMDB_TEST_HW"}, model_size{"ASKMDB_TEST_MODEL_SIZE"}, model_sha{"ASKMDB_TEST_MODEL_SHA256"},
        gpu_error{"ASKMDB_TEST_GPU_ERROR"};

    Sandbox() {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path() /
              ("askmdb_cli_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        xdg_config.set((dir / "config").string());
        xdg_cache.set((dir / "cache").string());
        xdg_data.set((dir / "data").string());
        xdg_state.set((dir / "state").string());
        for (EnvGuard* g : {&uri, &lc_all, &lc_messages, &type, &pipeline, &error, &test_model, &raw, &collection,
                            &explanation, &timeout, &debug, &model_size, &model_sha, &gpu_error})
            g->unset();
        test_hw.set("none");  // never the real hardware
        lang.set("es_ES.UTF-8");
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void config(const std::string& text) {
        std::filesystem::create_directories(dir / "config" / "askmdb");
        std::ofstream(dir / "config" / "askmdb" / "config") << text;
    }
    // Writes a file in the sandbox and returns its path.
    std::string file(const std::string& name, const std::string& text) const {
        std::ofstream(dir / name, std::ios::binary) << text;
        return (dir / name).string();
    }
    // A model of the family already "downloaded" (size = ASKMDB_TEST_MODEL_SIZE).
    void model(const std::string& filename, std::size_t size = 5) const {
        std::filesystem::create_directories(dir / "data" / "askmdb" / "models");
        std::ofstream(dir / "data" / "askmdb" / "models" / filename, std::ios::binary) << std::string(size, 'g');
    }
    std::filesystem::path state_file() const { return dir / "state" / "askmdb" / "choice"; }
    std::string state() const {
        std::ifstream f(state_file());
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    // FakeEngine plus the model hooks: the device and model are resolved (first start included).
    void family() {
        type.set("unsure");
        explanation.set("No sé.");
        test_model.set("https://example.invalid/m.gguf");
        model_size.set("5");
        model_sha.set("x");
    }
    // The cache directory exists only if something wrote to it (schema or prefix states).
    bool cache_touched() const { return std::filesystem::exists(dir / "cache"); }
};

// A valid block, exactly as schema::render writes it.
const std::string kTiendaBlock =
    "db=tienda\n"
    "## clientes\n"
    "_id str\n"
    "nombre str\n"
    "## pedidos\n"
    "_id oid\n"
    "fecha date\n"
    "importe double\n"
    "estado str [\"entregado\",\"pendiente\"]\n"
    "cliente.ciudad str [\"Madrid\",\"Barcelona\"]\n";

// The real URI parser and check_db are linked (not the skeleton's stubs). Otherwise the tests
// that need them would try to connect.
bool real_uri_parser() {
    std::string e;
    return askmdb::mongo::Uri::parse("mongodb://localhost:27017/db", e).has_value() &&
           askmdb::verify::check_db("admin").has_value();
}

Json one_object(const std::string& out) {
    REQUIRE(!out.empty());
    CHECK(out.back() == '\n');
    CHECK(out.find('\n') == out.size() - 1);  // exactly one line
    return Json::parse(out);
}

}  // namespace

TEST_SUITE("cli") {

TEST_CASE("parse_args: acciones") {
    CHECK(parse_args({}).action == Args::Action::Run);
    CHECK(parse_args({}).question.empty());
    CHECK(parse_args({"--help"}).action == Args::Action::Help);
    CHECK(parse_args({"-h", "x"}).action == Args::Action::Help);
    CHECK(parse_args({"--version"}).action == Args::Action::Version);
    const Args bad = parse_args({"pedidos", "--raro"});
    CHECK(bad.action == Args::Action::BadFlag);
    CHECK(bad.bad_flag == "--raro");
    CHECK(parse_args({"--raro", "--help"}).action == Args::Action::BadFlag);
    const Args ok = parse_args({"pedidos", "de", "Madrid"});
    CHECK(ok.action == Args::Action::Run);
    CHECK(ok.question == "pedidos de Madrid");
    CHECK(parse_args({"--", "-x", "--json"}).question == "-x --json");
    CHECK_FALSE(parse_args({"--", "--json", "x"}).json);
    CHECK(parse_args({"", "a"}).question == "a");
    CHECK(parse_args({"-"}).question == "-");
}

TEST_CASE("parse_args: opciones con valor") {
    const Args a = parse_args({"--uri", "mongodb://h/db", "--db", "tienda", "-c", "pedidos, clientes,pedidos",
                               "--refresh", "--mode", "auto", "--model", "m.gguf", "--lang", "en", "--json", "top",
                               "5"});
    CHECK(a.action == Args::Action::Run);
    CHECK(a.uri == "mongodb://h/db");
    CHECK(a.db == "tienda");
    REQUIRE(a.collections);
    CHECK(*a.collections == std::vector<std::string>{"pedidos", "clientes"});
    CHECK(a.refresh);
    CHECK(a.mode == Mode::Auto);
    CHECK(a.model == "m.gguf");
    CHECK(a.lang == Lang::En);
    CHECK(a.json);
    CHECK_FALSE(a.print_schema);
    CHECK(a.question == "top 5");

    const Args eq = parse_args({"--uri=mongodb://u:p@h/db?x=1", "--collections=a", "--mode=suggest", "--lang=es", "q"});
    CHECK(eq.uri == "mongodb://u:p@h/db?x=1");
    CHECK(*eq.collections == std::vector<std::string>{"a"});
    CHECK(eq.mode == Mode::Suggest);
    CHECK(eq.lang == Lang::Es);

    // The value is always the next argument, even if it starts with "-".
    CHECK(parse_args({"--db", "--json", "q"}).db == "--json");
    CHECK_FALSE(parse_args({"--db", "--json", "q"}).json);
    CHECK(parse_args({"--db", "a", "--db", "b", "q"}).db == "b");

    const Args ps = parse_args({"--print-schema"});
    CHECK(ps.action == Args::Action::Run);
    CHECK(ps.print_schema);
    CHECK(ps.question.empty());

    const Args pp = parse_args({"--schema-from", "s.txt", "--print-prompt", "pedidos", "de", "hoy"});
    CHECK(pp.action == Args::Action::Run);
    CHECK(pp.print_prompt);
    CHECK_FALSE(pp.print_schema);
    CHECK(pp.schema_from == "s.txt");
    CHECK(pp.question == "pedidos de hoy");
    CHECK(parse_args({"--schema-from=a b.txt", "q"}).schema_from == "a b.txt");
    CHECK_FALSE(parse_args({"q"}).schema_from);
    CHECK_FALSE(parse_args({"q"}).print_prompt);
}

TEST_CASE("parse_args: errores de valor") {
    for (const auto& v : std::vector<std::vector<std::string>>{
             {"q", "--uri"}, {"--uri=", "q"}, {"--db", "", "q"}, {"q", "-c"}, {"--mode"}, {"--help", "--lang"}}) {
        CAPTURE(v.front());
        CHECK(parse_args(v).action == Args::Action::MissingValue);
    }
    CHECK(parse_args({"q", "-c"}).bad_flag == "-c");
    CHECK(parse_args({"--db"}).bad_flag == "--db");

    const Args mode = parse_args({"--mode", "fast", "q"});
    CHECK(mode.action == Args::Action::BadValue);
    CHECK(mode.bad_flag == "--mode");
    CHECK(mode.bad_value == "fast");
    CHECK(parse_args({"--lang", "fr", "q"}).action == Args::Action::BadValue);
    CHECK(parse_args({"--lang", "auto", "q"}).action == Args::Action::BadValue);
    CHECK(parse_args({"-c", " , ", "q"}).action == Args::Action::BadValue);
    CHECK(parse_args({"--modelo", "x"}).action == Args::Action::BadFlag);
    CHECK(parse_args({"-c=a", "q"}).action == Args::Action::BadFlag);

    const Args conflict = parse_args({"--print-schema", "--json"});
    CHECK(conflict.action == Args::Action::Conflict);
    CHECK(conflict.bad_flag == "--print-schema --json");
    CHECK(parse_args({"--print-prompt", "--json", "q"}).bad_flag == "--print-prompt --json");
    CHECK(parse_args({"--print-prompt", "--print-schema", "q"}).bad_flag == "--print-schema --print-prompt");
    CHECK(parse_args({"--schema-from", "s", "--db", "x", "q"}).bad_flag == "--schema-from --db");
    CHECK(parse_args({"--schema-from", "s", "--refresh", "q"}).bad_flag == "--schema-from --refresh");
    CHECK(parse_args({"--print-prompt", "--json", "q"}).action == Args::Action::Conflict);
    CHECK(parse_args({"--schema-from"}).action == Args::Action::MissingValue);
    // Precedence: unknown flag > missing value > bad value > help > version > conflict.
    CHECK(parse_args({"--mode", "x", "--db"}).action == Args::Action::MissingValue);
    CHECK(parse_args({"--mode", "x", "--help"}).action == Args::Action::BadValue);
    CHECK(parse_args({"--version", "--help"}).action == Args::Action::Help);
    CHECK(parse_args({"--print-schema", "--json", "--version"}).action == Args::Action::Version);
}

TEST_CASE("parse_args: --device y --detect") {
    using askmdb::config::DeviceMode;
    CHECK_FALSE(parse_args({"q"}).device);
    CHECK_FALSE(parse_args({"q"}).detect);
    CHECK(parse_args({"--device", "auto", "q"}).device == DeviceMode::Auto);
    CHECK(parse_args({"--device", "cpu", "q"}).device == DeviceMode::Cpu);
    CHECK(parse_args({"--device=gpu", "q"}).device == DeviceMode::Gpu);
    const Args bad = parse_args({"--device", "cuda", "q"});
    CHECK(bad.action == Args::Action::BadValue);
    CHECK(bad.bad_flag == "--device");
    CHECK(bad.bad_value == "cuda");
    CHECK(parse_args({"q", "--device"}).action == Args::Action::MissingValue);

    const Args d = parse_args({"--detect"});
    CHECK(d.action == Args::Action::Run);
    CHECK(d.detect);
    CHECK(parse_args({"--detect", "--json", "--lang", "en"}).action == Args::Action::Run);
    for (const auto& [v, flags] : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"--detect", "--print-schema"}, "--detect --print-schema"},
             {{"--detect", "--print-prompt"}, "--detect --print-prompt"},
             {{"--detect", "--schema-from", "s"}, "--detect --schema-from"},
             {{"--detect", "--model", "m.gguf"}, "--detect --model"},
             {{"--detect", "--device", "gpu"}, "--detect --device"},
             {{"--detect", "pedidos"}, "--detect"}}) {
        CAPTURE(flags);
        const Args c = parse_args(v);
        CHECK(c.action == Args::Action::Conflict);
        CHECK(c.bad_flag == flags);
    }
}

TEST_CASE("normalize_request") {
    CHECK(askmdb::infer::normalize_request("  hola   mundo \n") == "hola mundo");
    CHECK(askmdb::infer::normalize_request("a\tb\r\nc") == "a b c");
    CHECK(askmdb::infer::normalize_request("a\xC2\xA0" "b\xE3\x80\x80" "c\xE2\x80\xAF") == "a b c");  // NBSP, ideographic, NNBSP
    CHECK(askmdb::infer::normalize_request("\xE2\x80\x83x\xE2\x80\x83") == "x");                         // em space
    CHECK(askmdb::infer::normalize_request("año ñ") == "año ñ");
    CHECK(askmdb::infer::normalize_request("a\xFF b") == "a\xFF b");  // invalid UTF-8: kept as is
    CHECK(askmdb::infer::normalize_request(" \t ").empty());
}

TEST_CASE("idioma de la interfaz") {
    CHECK(ui_lang(Lang::Es, nullptr, nullptr, "en_US.UTF-8") == UiLang::Es);
    CHECK(ui_lang(Lang::En, "es_ES.UTF-8", nullptr, nullptr) == UiLang::En);
    CHECK(ui_lang(Lang::Auto, "", "es_ES", "en_US") == UiLang::Es);
    CHECK(ui_lang(Lang::Auto, "C", "es_ES", nullptr) == UiLang::En);
    CHECK(ui_lang(Lang::Auto, nullptr, nullptr, "es_AR.UTF-8") == UiLang::Es);
    CHECK(ui_lang(Lang::Auto, nullptr, nullptr, nullptr) == UiLang::En);
}

TEST_CASE("textos: completos y distintos por idioma") {
    for (int i = 0; i < static_cast<int>(Msg::Count_); ++i) {
        CAPTURE(i);
        const auto m = static_cast<Msg>(i);
        CHECK_FALSE(tr(UiLang::Es, m).empty());
        CHECK_FALSE(tr(UiLang::En, m).empty());
    }
    for (UiLang l : {UiLang::Es, UiLang::En})
        for (const char* flag : {"--uri", "--db", "--collections", "--refresh", "--print-schema", "--mode", "--json",
                                 "--model", "--lang", "--help", "--version", "--print-prompt", "--schema-from",
                                 "max_time_ms", "0600"})
            CHECK(has(std::string(tr(l, Msg::Help)), flag));
    CHECK(tr(UiLang::Es, Msg::InvalidResponse) == "askmdb: no he podido generar una respuesta válida");
    CHECK(fill("hay más filas (tope {})", "50") == "hay más filas (tope 50)");
    CHECK(fill_all("{} y {} ({})", {"a{}", "b", "c"}) == "a{} y b (c)");
    CHECK(fill_all("{} y {}", {"a"}) == "a y ");
}

TEST_CASE("use_color") {
    CHECK(use_color(true, nullptr, "xterm"));
    CHECK(use_color(true, "", "xterm"));
    CHECK_FALSE(use_color(true, "1", "xterm"));
    CHECK_FALSE(use_color(true, nullptr, "dumb"));
    CHECK_FALSE(use_color(false, nullptr, "xterm"));
}

TEST_CASE("pantallas") {
    CHECK(render_suggestion("db.a.aggregate([\n  { $match: {} }\n])", "Lista.", false) ==
          "\n  ❯ db.a.aggregate([\n      { $match: {} }\n    ])\n    Lista.\n\n");
    CHECK(render_suggestion("x\x1b[2J", "", false) == "\n  ❯ x\\x1b[2J\n\n");
    CHECK(render_suggestion("a", "B.", true) == "\n  ❯ a\n    \x1b[2mB.\x1b[0m\n\n");

    const Assessment read;
    CHECK(render_level(read, UiLang::Es, false, true) == "  ● solo lectura  ·  ¿Ejecutar? [S/n/c(opiar)] ");
    CHECK(render_level(read, UiLang::En, false, false) == "  ● read only\n");
    CHECK(render_level(read, UiLang::Es, true, false) == "  \x1b[32m● solo lectura\x1b[0m\n");

    Assessment warn;
    warn.level = Level::Warn;
    warn.reasons = {{"W-1", "campo raro", "odd field"}, {"W-2", "otro\x1b", "other"}};
    CHECK(render_level(warn, UiLang::Es, false, true) ==
          "  ▲ aviso\n    · campo raro\n    · otro\\x1b\n  ¿Ejecutar? [s/N/c(opiar)] ");
    CHECK(render_level(warn, UiLang::En, false, false) == "  ▲ warning\n    · odd field\n    · other\n");

    Assessment blocked;
    blocked.level = Level::Blocked;
    blocked.reasons = {{"B-1", "escribe", "writes"}};
    CHECK(render_level(blocked, UiLang::Es, false, true) == "  ■ bloqueada  No se ejecutará.\n    · escribe\n");
    CHECK(render_level(blocked, UiLang::En, true, false) ==
          "  \x1b[1;31m■ blocked\x1b[0m  It will not be run.\n    · writes\n");
    CHECK(render_prompt(Level::Read, UiLang::En) == "  Run it? [Y/n/c(opy)] ");
    CHECK(render_prompt(Level::Warn, UiLang::En) == "  Run it? [y/N/c(opy)] ");
    CHECK(render_prompt(Level::Blocked, UiLang::En).empty());

    CHECK(render_refusal(false, "solo leo.", UiLang::Es, false) == "\n  ✗ No puedo: solo leo.\n\n");
    CHECK(render_refusal(true, "no hay repartidores.", UiLang::Es, false) ==
          "\n  ? No lo sé: no hay repartidores.\n\n");
    CHECK(render_refusal(true, "no data.", UiLang::En, false) == "\n  ? I don't know: no data.\n\n");
    CHECK(render_refusal(false, "a\nb", UiLang::En, true) == "\n  \x1b[2m✗\x1b[0m I can't: a\n    b\n\n");

    CHECK(render_status(true, "Copiado", false) == "  ✓ Copiado\n");
    CHECK(render_status(false, "Cancelado", true) == "  \x1b[2m✗\x1b[0m Cancelado\n");

    CHECK(render_results("", false, 50, UiLang::Es, false) == "\n  (sin resultados)\n");
    CHECK(render_results(" a\n 1\n", false, 50, UiLang::Es, false) == "\n a\n 1\n");
    CHECK(render_results(" a\n 1\n", true, 50, UiLang::Es, false) == "\n a\n 1\n  (hay más filas (tope 50))\n");
    CHECK(render_results(" a", true, 7, UiLang::En, false) == "\n a\n  (there are more rows (limit 7))\n");
    CHECK(render_results("  a  b\n  1  2\n", false, 50, UiLang::Es, true) == "\n  \x1b[1ma  b\x1b[0m\n  1  2\n");
    CHECK(render_results("  n", true, 7, UiLang::En, true) ==
          "\n  \x1b[1mn\x1b[0m\n  \x1b[2m(there are more rows (limit 7))\x1b[0m\n");
}

namespace {

std::string strip_ansi(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i = text.find('m', i);
            REQUIRE(i != std::string_view::npos);
            continue;
        }
        out += text[i];
    }
    return out;
}

// No line leaves a color open: the last escape code of each line is a reset.
bool lines_end_reset(std::string_view text) {
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        const std::size_t esc = line.rfind("\x1b[");
        if (esc != std::string_view::npos && line.substr(esc, 4) != "\x1b[0m") return false;
        start = end + 1;
    }
    return true;
}

std::string colored(std::string_view mongosh) { return render_suggestion(mongosh, "", true); }

}  // namespace

TEST_CASE("pantallas: resaltado de la consulta") {
    const std::string q =
        "db.pedidos.aggregate([\n"
        "  { $match: { estado: \"cancelado\", \"cliente.ciudad\": \"Madrid\", "
        "fecha: { $gte: ISODate(\"2026-01-01T00:00:00Z\") }, nota: /^a\\/b/i, borrado: null } },\n"
        "  { $group: { _id: \"$estado\", n: { $sum: 1 }, m: { $max: -3e+300 }, ok: { $first: true } } },\n"
        "  { $set: { hoy: \"$$NOW\", d: { $in: [\"$a\", [1.0, -Infinity]] } } },\n"
        "  { $limit: NumberLong(\"5\") }\n"
        "])";
    const std::string out = colored(q);
    // Head: db. and .aggregate dimmed, collection bold.
    CHECK(out.starts_with("\n  ❯ \x1b[2mdb.\x1b[0m\x1b[1mpedidos\x1b[0m\x1b[2m.aggregate\x1b[0m([\n"));
    // Stages bold cyan, inner operators cyan, plain and quoted keys uncolored.
    CHECK(has(out, "    { \x1b[1;36m$match\x1b[0m: { estado: \x1b[32m\"cancelado\"\x1b[0m, \"cliente.ciudad\": "));
    CHECK(has(out, "{ \x1b[1;36m$group\x1b[0m: { _id: "));
    CHECK(has(out, "fecha: { \x1b[36m$gte\x1b[0m: ISODate(\x1b[32m\"2026-01-01T00:00:00Z\"\x1b[0m) }"));
    CHECK(has(out, "n: { \x1b[36m$sum\x1b[0m: \x1b[33m1\x1b[0m }"));
    // Regex green; field paths and system variables magenta.
    CHECK(has(out, "nota: \x1b[32m/^a\\/b/i\x1b[0m,"));
    CHECK(has(out, "_id: \x1b[35m\"$estado\"\x1b[0m,"));
    CHECK(has(out, "hoy: \x1b[35m\"$$NOW\"\x1b[0m,"));
    CHECK(has(out, "[\x1b[35m\"$a\"\x1b[0m, [\x1b[33m1.0\x1b[0m, \x1b[33m-Infinity\x1b[0m]]"));
    // Numbers and literals yellow.
    CHECK(has(out, "\x1b[33m-3e+300\x1b[0m"));
    CHECK(has(out, "ok: { \x1b[36m$first\x1b[0m: \x1b[33mtrue\x1b[0m }"));
    CHECK(has(out, "borrado: \x1b[33mnull\x1b[0m"));
    CHECK(has(out, "{ \x1b[1;36m$limit\x1b[0m: NumberLong(\x1b[32m\"5\"\x1b[0m) }\n    ])\n"));
    CHECK(lines_end_reset(out));
    CHECK(strip_ansi(out) == render_suggestion(q, "", false));

    // Odd collection names: the quoted name is bold.
    CHECK(colored(R"(db.getCollection("mis-pedidos").aggregate([]))") ==
          "\n  ❯ \x1b[2mdb.\x1b[0mgetCollection(\x1b[1m\"mis-pedidos\"\x1b[0m)\x1b[2m.aggregate\x1b[0m([])\n\n");
    // Control characters are still escaped; unterminated literals stop at the end of the line.
    CHECK(colored("db.a.aggregate([\n  { $match: { x: \"a\x1b[2J\n/b") ==
          "\n  ❯ \x1b[2mdb.\x1b[0m\x1b[1ma\x1b[0m\x1b[2m.aggregate\x1b[0m([\n"
          "      { \x1b[1;36m$match\x1b[0m: { x: \x1b[32m\"a\\x1b[2J\x1b[0m\n    \x1b[32m/b\x1b[0m\n\n");
}

TEST_CASE("pantallas: el resaltado solo añade códigos de color") {
    int checked = 0;
    for (const char* file : {"lectura.jsonl", "aviso.jsonl", "bloqueada.jsonl"}) {
        std::ifstream in(std::string(ASKMDB_VERIFY_CASES_DIR) + "/" + file);
        REQUIRE(in);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line.front() != '{') continue;
            const auto c = nlohmann::json::parse(line, nullptr, false);
            if (c.is_discarded()) continue;  // some cases carry ill-formed UTF-8 on purpose
            const std::string pipeline = c.at("pipeline").dump();
            bson_error_t err{};
            bson_t* doc = bson_new_from_json(reinterpret_cast<const uint8_t*>(pipeline.data()),
                                             static_cast<ssize_t>(pipeline.size()), &err);
            if (doc == nullptr) continue;  // not every case is valid JSON for libbson
            const std::string mongosh = askmdb::mongo::to_mongosh(c.at("collection").get<std::string>(), *doc);
            bson_destroy(doc);
            const std::string out = render_suggestion(mongosh, "Explica.", true);
            CHECK(strip_ansi(out) == render_suggestion(mongosh, "Explica.", false));
            CHECK(lines_end_reset(out));
            ++checked;
        }
    }
    CHECK(checked > 500);
}

TEST_CASE("respuestas") {
    for (const char* yes : {"s", "S", "si", "SI", "sí", "SÍ", "Sí", "y", "Y", "yes", "YES", "  s  "})
        CHECK(parse_answer(yes) == Answer::Yes);
    CHECK(parse_answer("n") == Answer::No);
    CHECK(parse_answer("No") == Answer::No);
    CHECK(parse_answer("c") == Answer::Copy);
    CHECK(parse_answer("C") == Answer::Copy);
    CHECK(parse_answer("") == Answer::Empty);
    CHECK(parse_answer(" ") == Answer::Empty);
    CHECK(parse_answer("e") == Answer::Invalid);  // no inline editing
    CHECK(parse_answer("vale") == Answer::Invalid);
}

TEST_CASE("flujo: tabla de modos × niveles × TTY") {
    struct Cell {
        Mode mode;
        const char* level;  // collection name that drives fake_verify
        bool interactive;
        const char* answer;  // nullptr = nothing is read
        int code;
        bool runs;
        bool asks;
    };
    const Cell cells[] = {
        // suggest: shows, never asks or runs; blocked exits with 3.
        {Mode::Suggest, "ok", true, nullptr, 0, false, false},
        {Mode::Suggest, "WARN", true, nullptr, 0, false, false},
        {Mode::Suggest, "BLOCKED", true, nullptr, 3, false, false},
        // confirm: read Enter = yes; warn Enter = no; blocked never asks.
        {Mode::Confirm, "ok", true, "", 0, true, true},
        {Mode::Confirm, "WARN", true, "", 0, false, true},
        {Mode::Confirm, "WARN", true, "s", 0, true, true},
        {Mode::Confirm, "BLOCKED", true, nullptr, 3, false, false},
        // auto: read runs without asking; warn asks with Enter = no; blocked never asks.
        {Mode::Auto, "ok", true, nullptr, 0, true, false},
        {Mode::Auto, "WARN", true, "", 0, false, true},
        {Mode::Auto, "WARN", true, "y", 0, true, true},
        {Mode::Auto, "BLOCKED", true, nullptr, 3, false, false},
        // No TTY: shows, never runs (not even auto + read).
        {Mode::Confirm, "ok", false, nullptr, 0, false, false},
        {Mode::Auto, "ok", false, nullptr, 0, false, false},
        {Mode::Auto, "WARN", false, nullptr, 0, false, false},
        {Mode::Auto, "BLOCKED", false, nullptr, 3, false, false},
    };
    for (const Cell& c : cells) {
        CAPTURE(static_cast<int>(c.mode));
        CAPTURE(c.level);
        CAPTURE(c.interactive);
        FakeWorld w;
        w.input = {std::string(c.answer ? c.answer : "s"), std::string("s")};
        CHECK(w.go(c.level, c.mode, c.interactive) == c.code);
        CHECK(w.executed.size() == (c.runs ? 1u : 0u));
        CHECK(has(w.screen, "¿Ejecutar?") == c.asks);
        CHECK(w.input.size() == (c.answer ? 1u : 2u));  // read exactly once when asking
        CHECK(has(w.screen, "  ❯ db." + std::string(c.level) + ".aggregate([])\n    Explica.\n"));
        if (std::string(c.level) == "BLOCKED") {
            CHECK(has(w.screen, "■ bloqueada  No se ejecutará.\n    · escribe en otra colección\n"));
        } else if (std::string(c.level) == "WARN") {
            CHECK(has(w.screen, "▲ aviso\n    · campo que no está en el esquema\n"));
        } else {
            CHECK(has(w.screen, "● solo lectura"));
        }
    }
}

TEST_CASE("flujo: respuestas, reintentos, EOF y copia") {
    SUBCASE("n cancela") {
        FakeWorld w;
        w.input = {"n"};
        CHECK(w.go("ok") == 0);
        CHECK(w.executed.empty());
        CHECK(has(w.screen, "  ✗ Cancelado\n"));
    }
    SUBCASE("tres respuestas no válidas cancelan") {
        FakeWorld w;
        w.input = {"x", "e", "zz", "s"};
        CHECK(w.go("ok") == 0);
        CHECK(w.executed.empty());
        CHECK(w.input.size() == 1);
        CHECK(has(w.screen, "Cancelado"));
    }
    SUBCASE("dos no válidas y luego sí; el prompt se repite") {
        FakeWorld w;
        w.input = {"x", "zz", "s"};
        CHECK(w.go("WARN") == 0);
        CHECK(w.executed.size() == 1);
        std::size_t prompts = 0;
        for (std::size_t p = 0; (p = w.screen.find("[s/N/c(opiar)]", p)) != std::string::npos; ++p) ++prompts;
        CHECK(prompts == 3);
    }
    SUBCASE("EOF cancela") {
        FakeWorld w;
        CHECK(w.go("ok") == 0);
        CHECK(w.executed.empty());
        CHECK(has(w.screen, "\n  ✗ Cancelado\n"));
    }
    SUBCASE("c copia la consulta en sintaxis mongosh") {
        FakeWorld w;
        w.input = {"c"};
        CHECK(w.go("WARN", Mode::Auto) == 0);
        CHECK(w.copied == std::vector<std::string>{"db.WARN.aggregate([])"});
        CHECK(has(w.screen, "  ✓ Copiado al portapapeles\n"));
        CHECK(w.executed.empty());
    }
    SUBCASE("copia fallida: 1") {
        FakeWorld w;
        w.copy_ok = false;
        w.input = {"c"};
        CHECK(w.go("ok") == 1);
        CHECK(has(w.screen, "No se ha podido copiar"));
    }
    SUBCASE("el código del ejecutor se propaga (error de MongoDB = 4)") {
        FakeWorld w;
        w.exec_code = 4;
        CHECK(w.go("ok", Mode::Auto) == 4);
        w.input = {"s"};
        CHECK(w.go("ok") == 4);
    }
    SUBCASE("inglés") {
        FakeWorld w;
        w.input = {"yes"};
        CHECK(w.go("ok", Mode::Confirm, true, UiLang::En) == 0);
        CHECK(has(w.screen, "  ● read only  ·  Run it? [Y/n/c(opy)] "));
        CHECK(w.executed.size() == 1);
    }
}

TEST_CASE("json_report: campos, orden y nulos") {
    const std::vector<std::string> expected = {
        "contract_version", "askmdb_version", "server",    "question",     "type",  "collection", "pipeline",
        "mongosh",          "explanation",    "level",     "reasons",      "executed", "rows",    "truncated",
        "error",            "error_detail",   "model",     "model_name",   "device", "raw",        "tokens",
        "timings_ms",       "prefix_cache"};
    CHECK(report_keys() == expected);

    SUBCASE("vacío: todo a null salvo lo fijo") {
        Report r;
        r.question = "q";
        const Json j = one_object(json_report(r, UiLang::Es) + "\n");
        std::vector<std::string> got;
        for (const auto& [k, v] : j.items()) got.push_back(k);
        CHECK(got == expected);
        CHECK(j["contract_version"] == 1);
        CHECK(j["askmdb_version"] == ASKMDB_VERSION);
        CHECK(j["question"] == "q");
        CHECK(j["executed"] == false);
        for (const char* k : {"server", "type", "collection", "pipeline", "mongosh", "explanation", "level", "reasons",
                              "rows", "truncated", "error", "error_detail", "model", "model_name", "device", "raw",
                              "tokens", "timings_ms", "prefix_cache"})
            CHECK(j[k].is_null());
    }
    SUBCASE("completo: pipeline en EJSON relajado y filas tal cual") {
        Report r;
        r.hosts = std::vector<std::string>{"h1:27017", "h2:27017"};
        r.db = "tienda";
        r.question = "pedidos";
        r.type = "query";
        r.collection = "pedidos";
        r.pipeline = askmdb::verify::load_pipeline(
                         R"([{"$match":{"f":{"$date":"2026-01-01T00:00:00Z"},"x":1.5,"n":{"$numberLong":"3"}}}])")
                         .bson;
        r.mongosh = "db.pedidos.aggregate([])";
        r.explanation = "Explica.";
        r.assessment = Assessment{Level::Warn, {{"W-1", "raro", "odd"}}};
        r.executed = true;
        r.rows_json = R"([{"_id":1}])";
        r.truncated = true;
        const std::string line = json_report(r, UiLang::En);
        const Json j = one_object(line + "\n");
        CHECK(j["server"] == Json{{"hosts", {"h1:27017", "h2:27017"}}, {"db", "tienda"}});
        CHECK(j["pipeline"] ==
              Json::parse(R"([{"$match":{"f":{"$date":"2026-01-01T00:00:00Z"},"x":1.5,"n":3}}])"));
        CHECK(j["level"] == "warn");
        CHECK(j["reasons"] == Json::parse(R"([{"id":"W-1","text":"odd"}])"));
        CHECK(j["executed"] == true);
        CHECK(j["rows"] == Json::parse(R"([{"_id":1}])"));
        CHECK(j["truncated"] == true);
        CHECK(has(line, R"("rows":[{"_id":1}],)"));
        CHECK(Json::parse(json_report(r, UiLang::Es))["reasons"][0]["text"] == "raro");
    }
    SUBCASE("campos del modelo") {
        Report r;
        r.question = "q";
        EngineInfo e;
        e.model = "m.gguf";
        e.raw = "{\"type\":\"unsure\"}";
        e.timings = {12.345, 100.04, 250.06, 362.4, 210, 180, 25};
        e.prefix_cache = askmdb::infer::PrefixCache::Hit;
        r.engine = e;
        const std::string line = json_report(r, UiLang::Es);
        const Json j = one_object(line + "\n");
        CHECK(j["model"] == "m.gguf");
        CHECK(j["raw"] == "{\"type\":\"unsure\"}");
        CHECK(j["tokens"] == Json::parse(R"({"prompt":210,"prefix":180,"generated":25})"));
        CHECK(has(line, R"("timings_ms":{"load":12.3,"prefill":100.0,"generate":250.1,"total":362.4})"));
        CHECK(j["prefix_cache"] == "hit");
        CHECK(j["model_name"].is_null());
        CHECK(j["device"].is_null());
        r.model_name = "askmdb-4b";
        r.device = "gpu";
        const Json named = one_object(json_report(r, UiLang::Es) + "\n");
        CHECK(named["model_name"] == "askmdb-4b");
        CHECK(named["device"] == "gpu");
        r.model_name.reset();
        r.device.reset();

        r.engine->model.reset();
        r.engine->prefix_cache = askmdb::infer::PrefixCache::Off;
        const Json fake = one_object(json_report(r, UiLang::Es) + "\n");
        CHECK(fake["model"].is_null());
        CHECK(fake["prefix_cache"] == "off");
        r.engine->prefix_cache = askmdb::infer::PrefixCache::Miss;
        CHECK(one_object(json_report(r, UiLang::Es) + "\n")["prefix_cache"] == "miss");
    }
    SUBCASE("UTF-8 no válido se reemplaza") {
        Report r;
        r.question = "a\xC3";
        r.error = "invalid_json";
        r.error_detail = "x";
        const Json j = one_object(json_report(r, UiLang::Es) + "\n");
        CHECK(j["question"] == "a\xEF\xBF\xBD");
        CHECK(j["error"] == "invalid_json");
    }
}

TEST_CASE("FakeEngine por variables de entorno") {
    EnvGuard type("ASKMDB_FAKE_TYPE"), coll("ASKMDB_FAKE_COLLECTION"), pipe("ASKMDB_FAKE_PIPELINE"),
        expl("ASKMDB_FAKE_EXPLANATION"), err("ASKMDB_FAKE_ERROR"), detail("ASKMDB_FAKE_ERROR_DETAIL"),
        raw("ASKMDB_FAKE_RAW"), timeout("ASKMDB_TEST_TIMEOUT_MS");
    for (EnvGuard* g : {&type, &coll, &pipe, &expl, &err, &detail, &raw, &timeout}) g->unset();
    using askmdb::infer::fake_engine_from_env;
    CHECK(fake_engine_from_env() == nullptr);

    pipe.set(R"([{"$count":"n"}])");
    coll.set("pedidos");
    expl.set("Cuenta.");
    auto r = fake_engine_from_env()->suggest({});
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->kind == Suggestion::Kind::Query);
    CHECK(r.suggestion->collection == "pedidos");
    CHECK(r.suggestion->pipeline.text == R"([{"$count":"n"}])");
    CHECK(r.suggestion->pipeline.bson);
    CHECK(r.suggestion->explanation == "Cuenta.");

    pipe.set("[{");
    r = fake_engine_from_env()->suggest({});
    REQUIRE(r.suggestion);
    CHECK_FALSE(r.suggestion->pipeline.bson);
    CHECK_FALSE(r.suggestion->pipeline.parse_error.empty());

    pipe.unset();
    type.set("unsure");
    r = fake_engine_from_env()->suggest({});
    REQUIRE(r.suggestion);
    CHECK(r.suggestion->kind == Suggestion::Kind::Unsure);
    CHECK(r.suggestion->collection.empty());

    type.set("raro");
    r = fake_engine_from_env()->suggest({});
    CHECK_FALSE(r.suggestion);
    CHECK(r.error == "invalid_schema");

    type.unset();
    err.set("timeout");
    detail.set("30 s");
    r = fake_engine_from_env()->suggest({});
    CHECK_FALSE(r.suggestion);
    CHECK(r.error == "timeout");
    CHECK(r.error_detail == "30 s");
}

TEST_CASE("resolución del modelo") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("askmdb-model-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const ModelSpec spec{"m", "m.gguf", "https://example.invalid/m.gguf", 3, "x"};
    using S = ModelResolution::Status;

    const std::string other = (dir / "otro.gguf").string();
    CHECK(resolve_model(other, dir.string(), spec).status == S::Missing);
    std::ofstream(other) << "cualquier tamaño";
    CHECK(resolve_model(other, dir.string(), spec).status == S::Ready);

    const std::string def = (dir / "m.gguf").string();
    CHECK(resolve_model("", dir.string(), spec).status == S::NeedsDownload);
    std::ofstream(def) << "abcd";
    CHECK(resolve_model("", dir.string(), spec).status == S::NeedsDownload);
    std::ofstream(def) << "abc";
    CHECK(resolve_model("", dir.string(), spec).status == S::Ready);
    CHECK(resolve_model("", "", spec).status == S::NoDir);
    fs::remove_all(dir);
    CHECK(size_mb(3) == 1);
    CHECK(size_mb(529298016ULL) == 529);   // decimal MB, as the model cards say
    CHECK(size_mb(2708805088ULL) == 2709);
    CHECK(size_mb(5629109824ULL) == 5629);
}

TEST_CASE("run: ayuda, versión y uso") {
    Sandbox sb;
    Term t;
    CHECK(t.run({"--help"}) == 0);
    CHECK(has(t.out, "Uso: askmdb"));
    CHECK(t.err.empty());

    Term v;
    CHECK(v.run({"--version", "--lang", "en"}) == 0);
    CHECK(v.out.rfind("askmdb " ASKMDB_VERSION "\nTLS: ", 0) == 0);
    CHECK((has(v.out, "TLS: yes\n") || has(v.out, "TLS: no\n")));
    CHECK(has(v.out, askmdb::infer::vulkan_built() ? "\nVulkan: yes\n" : "\nVulkan: no\n"));
    CHECK(has(v.out, "\nModels: askmdb-0.8b, askmdb-4b, askmdb-9b 1.0.0 (Q4_K_M)\n"));
    CHECK(has(v.out, "\nlibmongoc 2."));
    CHECK((has(v.out, ", OpenSSL 3.") || has(v.out, ", OpenSSL: none")));
    CHECK(v.out.back() == '\n');
    CHECK(v.err.empty());

    Term bad;
    CHECK(bad.run({"--raro", "q"}) == 2);
    CHECK(bad.out.empty());
    CHECK(bad.err.rfind("askmdb: opción desconocida: --raro\n\nUso: askmdb", 0) == 0);

    Term badv;
    CHECK(badv.run({"--lang", "en", "--mode", "fast", "--json", "q"}) == 2);
    CHECK(badv.out.empty());  // argument errors: no JSON
    CHECK(badv.err.rfind("askmdb: invalid value for --mode: fast\n\nUsage:", 0) == 0);

    Term conflict;
    CHECK(conflict.run({"--print-schema", "--json"}) == 2);

    Term none;
    CHECK(none.run({}) == 2);
    CHECK(none.out.empty());
    CHECK(has(none.err, "Uso: askmdb"));

    Term spaces;
    CHECK(spaces.run({"--json", " \xC2\xA0 "}) == 2);
    const Json j = one_object(spaces.out);
    CHECK(j["error"] == "usage");
    CHECK(j["question"] == "");

    Term long_q;
    CHECK(long_q.run({std::string(1001, 'a')}) == 2);
    CHECK(has(long_q.err, "pregunta demasiado larga (máximo 1000 bytes)"));
    Term just_fits;  // 1000 bytes after normalizing: goes on (and stops at the URI)
    CHECK(just_fits.run({"  " + std::string(1000, 'a') + "  "}) == 2);
    CHECK(has(just_fits.err, "falta la URI"));
}

TEST_CASE("run: configuración y URI") {
    Sandbox sb;
    SUBCASE("configuración rota: 1, y --json lo dice") {
        sb.config("mode = rapido\n");
        Term t;
        CHECK(t.run({"--json", "q"}) == 1);
        const Json j = one_object(t.out);
        CHECK(j["error"] == "config");
        CHECK(j["server"].is_null());
        CHECK(has(t.err, "askmdb: ~/") == false);  // outside HOME: full path
        CHECK(has(t.err, "config:1: valor no válido para mode"));
    }
    SUBCASE("uri con contraseña y fichero 0644: no arranca") {
        sb.config("uri = mongodb://ana:secreto@h/db\n");
        ::chmod((sb.dir / "config" / "askmdb" / "config").c_str(), 0644);
        Term t;
        CHECK(t.run({"--json", "q"}) == 1);
        CHECK_FALSE(has(t.out + t.err, "secreto"));
        CHECK(has(t.err, "0600"));
    }
    SUBCASE("sin URI: 2") {
        Term t;
        CHECK(t.run({"--lang", "en", "q"}) == 2);
        CHECK(t.err == "askmdb: missing MongoDB URI: use --uri, ASKMDB_URI or \"uri = …\" in the config\n");
        Term ps;
        CHECK(ps.run({"--print-schema"}) == 2);
        Term js;
        CHECK(js.run({"--json", "q"}) == 2);
        CHECK(one_object(js.out)["error"] == "no_uri");
    }
    SUBCASE("URI no válida: 2, sin la contraseña") {
        Term t;
        CHECK(t.run({"--json", "--uri", "mongodb://ana:secreto@", "q"}) == 2);
        CHECK(one_object(t.out)["error"] == "invalid_uri");
        CHECK(has(t.err, "aviso: la contraseña de --uri"));
        CHECK_FALSE(has(t.out, "secreto"));
    }
    SUBCASE("ASKMDB_URI y config: el orden manda") {
        if (!real_uri_parser()) {
            MESSAGE("skipped: Uri::parse or check_db is still a stub");
            return;
        }
        sb.config("uri = mongodb://desde-config/admin\n");
        sb.uri.set("mongodb://desde-env/local");
        Term env;
        CHECK(env.run({"--json", "q"}) == 3);
        Json j = one_object(env.out);
        CHECK(j["server"]["db"] == "local");
        CHECK(j["error"] == "db_blocked");
        CHECK(j["level"] == "blocked");

        Term flag;
        CHECK(flag.run({"--json", "--uri", "mongodb://desde-flag/config", "q"}) == 3);
        CHECK(one_object(flag.out)["server"]["db"] == "config");

        sb.uri.unset();
        Term cfg;
        CHECK(cfg.run({"--json", "q"}) == 3);
        CHECK(one_object(cfg.out)["server"]["db"] == "admin");
    }
    SUBCASE("sin base: 2; --db la pone") {
        if (!real_uri_parser()) {
            MESSAGE("skipped: Uri::parse or check_db is still a stub");
            return;
        }
        Term t;
        CHECK(t.run({"--json", "--uri", "mongodb://u:p@h1:27017,h2:27018/", "q"}) == 2);
        const Json j = one_object(t.out);
        CHECK(j["error"] == "no_db");
        CHECK(j["server"]["hosts"].size() == 2);
        CHECK_FALSE(has(t.out, "u:p"));
        Term d;
        CHECK(d.run({"--uri", "mongodb://h/tienda", "--db", "admin", "q"}) == 3);
        CHECK(has(d.err, "admin"));
    }
}

}  // TEST_SUITE

TEST_SUITE("cli") {

TEST_CASE("run: --schema-from que no vale es un error de uso") {
    Sandbox sb;
    struct Bad {
        const char* name;
        std::string text;  // empty name of file = does not exist
    };
    const Bad cases[] = {
        {"no existe", ""},
        {"no es un bloque", "hola\n"},
        {"sin salto final", kTiendaBlock.substr(0, kTiendaBlock.size() - 1)},
        {"no canónico", "db=tienda\n## pedidos\nx str [\"b\", \"a\"]\n"},
        {"demasiado grande", std::string((1 << 20) + 1, '\n')},
    };
    for (const Bad& c : cases) {
        CAPTURE(std::string(c.name));
        const std::string path = c.text.empty() ? (sb.dir / "no-existe.txt").string() : sb.file("s.txt", c.text);
        Term t;
        CHECK(t.run({"--json", "--mode", "suggest", "--schema-from", path, "q"}) == 2);
        const Json j = one_object(t.out);
        CHECK(j["error"] == "schema_from");
        CHECK(j["error_detail"].is_string());
        CHECK(has(t.err, "el esquema " + path));
        Term ps;
        CHECK(ps.run({"--schema-from", path, "--print-schema"}) == 2);
        CHECK(ps.out.empty());
    }
    Term en;
    CHECK(en.run({"--lang", "en", "--schema-from", sb.file("s.txt", "db=x\n## b\n## a\n"), "q"}) == 2);
    CHECK(has(en.err, "askmdb: the schema "));
    CHECK(has(en.err, "is not a valid block: line 3: "));
    Term render;
    CHECK(render.run({"--lang", "en", "--schema-from", sb.file("s.txt", "db=x\n## a\nb str [\"u\", \"v\"]\n"), "q"}) == 2);
    CHECK(has(render.err, "is not written exactly as askmdb writes it"));
    CHECK_FALSE(sb.cache_touched());
}

TEST_CASE("run: --schema-from en suggest no resuelve la URI ni conecta") {
    if (!real_uri_parser()) {
        MESSAGE("skipped: Uri::parse or check_db is still a stub");
        return;
    }
    Sandbox sb;
    const std::string block = sb.file("s.txt", kTiendaBlock);
    sb.type.set("query");
    sb.collection.set("pedidos");
    sb.pipeline.set(R"([{"$match":{"estado":"pendiente"}}])");
    // Unparseable on purpose: it would be a usage error if it were read.
    sb.uri.set("mongodb://ana:secreto@");

    for (const char* mode : {"suggest", "confirm"}) {
        CAPTURE(mode);
        Term t;
        CHECK(t.run({"--json", "--mode", mode, "--schema-from", block, "pedidos pendientes"}) == 0);
        const Json j = one_object(t.out);
        CHECK(j["server"] == Json::parse(R"({"hosts":[],"db":"tienda"})"));
        CHECK(j["type"] == "query");
        CHECK(j["level"] == "read");
        CHECK(j["executed"] == false);
        CHECK(j["error"].is_null());
        // FakeEngine: no model, empty raw, no prefix cache.
        CHECK(j["model"].is_null());
        CHECK(j["raw"] == "");
        CHECK(j["tokens"] == Json::parse(R"({"prompt":0,"prefix":0,"generated":0})"));
        CHECK(j["timings_ms"] == Json::parse(R"({"load":0,"prefill":0,"generate":0,"total":0})"));
        CHECK(j["prefix_cache"] == "off");
    }
    Term text;  // text output without a TTY never runs either
    CHECK(text.run({"--mode", "auto", "--schema-from", block, "pedidos pendientes"}) == 0);
    CHECK(has(text.out, "db.pedidos.aggregate("));
    CHECK_FALSE(sb.cache_touched());

    // --json in auto may run: now the URI is needed.
    Term bad_uri;
    CHECK(bad_uri.run({"--json", "--mode", "auto", "--schema-from", block, "q"}) == 2);
    CHECK(one_object(bad_uri.out)["error"] == "invalid_uri");
    sb.uri.unset();
    Term no_uri;
    CHECK(no_uri.run({"--json", "--mode", "auto", "--schema-from", block, "q"}) == 2);
    CHECK(one_object(no_uri.out)["error"] == "no_uri");

    // The database is the block's, and admin/local/config are still blocked.
    Term admin;
    CHECK(admin.run({"--json", "--mode", "suggest", "--schema-from", sb.file("a.txt", "db=admin\n## x\n_id oid\n"),
                     "q"}) == 3);
    const Json ja = one_object(admin.out);
    CHECK(ja["error"] == "db_blocked");
    CHECK(ja["server"]["db"] == "admin");

    // The verifier gets the snapshot: an unknown field is a warning.
    sb.pipeline.set(R"([{"$match":{"campo_fantasma":1}}])");
    Term warn;
    CHECK(warn.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
    CHECK(one_object(warn.out)["level"] == "warn");
}

TEST_CASE("run: --print-schema y --print-prompt con --schema-from") {
    Sandbox sb;
    const std::string block = sb.file("s.txt", kTiendaBlock);
    // No engine and no model: neither option looks for one.
    Term ps;
    CHECK(ps.run({"--schema-from", block, "--print-schema"}) == 0);
    CHECK(ps.out == kTiendaBlock);
    CHECK(ps.err.empty());

    Term one;
    CHECK(one.run({"--schema-from", block, "--print-schema", "-c", "pedidos,otra"}) == 0);
    CHECK(one.out.rfind("db=tienda\n## pedidos\n", 0) == 0);
    CHECK_FALSE(has(one.out, "## clientes"));

    Term pp;
    CHECK(pp.run({"--schema-from", block, "--print-prompt", "--model", "/no/existe.gguf", " pedidos\xC2\xA0" "de",
                  "hoy\xE3\x80\x80"}) == 0);
    CHECK(pp.out == askmdb::infer::render_prompt({kTiendaBlock, "pedidos de hoy"}));
    CHECK(pp.out.size() > 2);
    CHECK(pp.out.substr(pp.out.size() - 2) == "\n\n");
    CHECK(has(pp.out, "\n\ndb=tienda\n## clientes\n"));
    CHECK(pp.err.empty());

    // Raw: the question goes out as the model sees it, control characters included.
    Term ctl;
    CHECK(ctl.run({"--schema-from", block, "--print-prompt", "a\x1b" "b"}) == 0);
    CHECK(has(ctl.out, "<|im_start|>user\na\x1b" "b<|im_end|>\n"));

    Term none;
    CHECK(none.run({"--schema-from", block, "--print-prompt"}) == 2);
    CHECK(none.out.empty());
    Term too_long;
    CHECK(too_long.run({"--schema-from", block, "--print-prompt", std::string(1001, 'a')}) == 2);
    Term conflict;
    CHECK(conflict.run({"--schema-from", block, "--print-prompt", "--json", "q"}) == 2);
    CHECK(conflict.out.empty());
    CHECK(has(conflict.err, "--print-prompt --json"));
    CHECK_FALSE(sb.cache_touched());
    CHECK_FALSE(std::filesystem::exists(sb.dir / "data"));
}

TEST_CASE("run: --print-prompt sin --schema-from necesita la URI, como --print-schema") {
    Sandbox sb;
    Term t;
    CHECK(t.run({"--print-prompt", "q"}) == 2);
    CHECK(has(t.err, "falta la URI"));
}

TEST_CASE("run: motor real con un modelo que no existe") {
    Sandbox sb;
    const std::string block = sb.file("s.txt", kTiendaBlock);
    Term t;
    CHECK(t.run({"--json", "--mode", "suggest", "--schema-from", block, "--model", "/no/existe.gguf", "q"}) == 1);
    const Json j = one_object(t.out);
    CHECK(j["error"] == "model_missing");
    for (const char* k : {"model", "raw", "tokens", "timings_ms", "prefix_cache"}) CHECK(j[k].is_null());
}

TEST_CASE("run: ASKMDB_FAKE_RAW pasa por el parser real") {
    Sandbox sb;
    const std::string block = sb.file("s.txt", kTiendaBlock);
    const std::string good =
        R"({"type":"query","collection":"pedidos","pipeline":[{"$match":{"estado":"pendiente"}}],)"
        R"("explanation":"Pedidos pendientes."})";
    sb.raw.set(good);
    Term ok;
    CHECK(ok.run({"--json", "--mode", "suggest", "--schema-from", block, "pedidos pendientes"}) == 0);
    const Json j = one_object(ok.out);
    CHECK(j["raw"] == good);
    CHECK(j["type"] == "query");
    CHECK(j["collection"] == "pedidos");
    CHECK(j["pipeline"] == Json::parse(R"([{"$match":{"estado":"pendiente"}}])"));
    CHECK(j["explanation"] == "Pedidos pendientes.");
    CHECK(j["level"] == "read");
    CHECK(j["model"].is_null());

    const std::string broken = R"({"type":"query","collection":)";
    sb.raw.set(broken);
    sb.debug.set("1");
    Term bad;
    CHECK(bad.run({"--json", "--mode", "suggest", "--schema-from", block, "pedidos pendientes"}) == 1);
    const Json jb = one_object(bad.out);
    CHECK(jb["raw"] == broken);
    CHECK(jb["error"].is_string());
    CHECK(jb["type"].is_null());
    CHECK(jb["executed"] == false);
    CHECK(has(bad.err, "no he podido generar una respuesta válida"));
    CHECK(has(bad.err, "askmdb: raw: "));
}

}  // TEST_SUITE("cli")

TEST_SUITE("cli") {

TEST_CASE("modelos de la familia: una tabla por nivel y ganchos de test") {
    using askmdb::infer::ModelTier;
    for (ModelTier t : {ModelTier::M08B, ModelTier::M4B, ModelTier::M9B}) {
        const auto& m = askmdb::fetch::model_info(t);
        CAPTURE(m.name);
        CHECK(askmdb::infer::tier_name(t) == m.name);
        CHECK(std::string(m.filename) == std::string(m.name) + "-1.0.0-Q4_K_M.gguf");
        CHECK(has(m.url, "https://huggingface.co/manuelgscom/" + std::string(m.name) + "-GGUF/resolve/"));
        CHECK(std::string(m.sha256).size() == 64);
        CHECK(has(m.url, m.filename));
    }
    CHECK(askmdb::fetch::model_info(ModelTier::M08B).size == 529298016ULL);
    CHECK(askmdb::fetch::model_info(ModelTier::M4B).size == 2708805088ULL);
    CHECK(askmdb::fetch::model_info(ModelTier::M9B).size == 5629109824ULL);
    CHECK(std::string(askmdb::fetch::model_info(ModelTier::M08B).sha256) ==
          "856c3502c6fe4061a77d9a4940a5e36670f35dded1d32cecc298af978cb3509c");
    CHECK(&askmdb::fetch::kDefaultModel == &askmdb::fetch::model_info(ModelTier::M08B));

    Sandbox sb;
    const ModelSpec real = model_spec(ModelTier::M4B);
    CHECK(real.name == "askmdb-4b");
    CHECK(real.size == 2708805088ULL);
    sb.family();
    const ModelSpec hooked = model_spec(ModelTier::M9B);
    CHECK(hooked.name == "askmdb-9b");
    CHECK(hooked.filename == "askmdb-9b-1.0.0-Q4_K_M.gguf");
    CHECK(hooked.url == "https://example.invalid/m.gguf");
    CHECK(hooked.size == 5);
    CHECK(hooked.sha256 == "x");
    CHECK(default_model_spec().filename == "askmdb-0.8b-1.0.0-Q4_K_M.gguf");
}

TEST_CASE("primer inicio sin terminal: nunca descarga; usa el modelo si ya está") {
    Sandbox sb;
    sb.family();
    const std::string block = sb.file("s.txt", kTiendaBlock);
    sb.test_hw.set("discrete:RTX:12000:16000");
    Term missing;
    CHECK(missing.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 1);
    const Json jm = one_object(missing.out);
    CHECK(jm["error"] == "model_not_downloaded");
    CHECK(has(missing.err, "askmdb-4b-1.0.0-Q4_K_M.gguf"));  // the default with >= 8 GiB
    CHECK_FALSE(std::filesystem::exists(sb.state_file()));

    // Only askmdb-0.8b there: the CPU with it, nothing saved.
    sb.model("askmdb-0.8b-1.0.0-Q4_K_M.gguf");
    Term cpu;
    CHECK(cpu.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
    const Json jc = one_object(cpu.out);
    CHECK(jc["error"].is_null());
    CHECK(jc["device"] == "cpu");
    CHECK(jc["model_name"] == "askmdb-0.8b");
    CHECK_FALSE(std::filesystem::exists(sb.state_file()));

    // The chosen model there: used, but never saved without a terminal or with --json (nothing
    // was checked), not even when the terminal is there.
    sb.model("askmdb-9b-1.0.0-Q4_K_M.gguf");
    Term ok;
    CHECK(ok.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
    const Json j = one_object(ok.out);
    CHECK(j["error"].is_null());
    CHECK(j["type"] == "unsure");
    CHECK(j["device"] == "gpu");
    CHECK(j["model_name"] == "askmdb-9b");
    CHECK(ok.err.empty());
    CHECK_FALSE(std::filesystem::exists(sb.state_file()));
    Term tty_json;
    tty_json.tty = true;
    CHECK(tty_json.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
    CHECK(one_object(tty_json.out)["model_name"] == "askmdb-9b");
    CHECK(tty_json.err.empty());
    Term text;
    CHECK(text.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
    CHECK(has(text.out, "No lo sé"));
    CHECK_FALSE(std::filesystem::exists(sb.state_file()));
}

TEST_CASE("primer inicio en un terminal: propuesta según el hardware") {
    Sandbox sb;
    sb.family();
    const std::string block = sb.file("s.txt", kTiendaBlock);
    for (const auto& name : {"askmdb-0.8b-1.0.0-Q4_K_M.gguf", "askmdb-4b-1.0.0-Q4_K_M.gguf", "askmdb-9b-1.0.0-Q4_K_M.gguf"})
        sb.model(name);

    SUBCASE("GPU con 6000 MiB libres: askmdb-4b; Enter acepta") {
        sb.test_hw.set("discrete:RTX:6000:8000");
        Term t;
        t.tty = true;
        t.input = {""};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
        CHECK(has(t.err, "GPU Vulkan0: RTX (dedicada), 6000 MiB libres de 8000 MiB"));
        CHECK(has(t.err, "Recomendado: askmdb-4b en la GPU RTX (1 MB)"));
        CHECK(has(t.err, "ya descargado: "));
        CHECK(has(t.err, "¿Usarlo? [S/n]"));
        CHECK(has(t.err, "Comprobando que el modelo funciona en la GPU RTX"));
        CHECK(has(t.err, "Elección guardada: askmdb-4b en la GPU RTX"));
        CHECK(has(t.err, "La primera vez puede tardar unos 15 segundos: se prepara la GPU (solo una vez)"));
        CHECK(has(t.err, "La GPU ya está preparada. La primera respuesta después de encender el equipo"));
        CHECK(has(t.out, "No lo sé"));
        CHECK(has(sb.state(), "model = askmdb-4b\n"));

        // Second run: no dialog, even with less free memory now.
        sb.test_hw.set("discrete:RTX:1000:8000");
        Term again;
        again.tty = true;
        CHECK(again.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
        const Json j = one_object(again.out);
        CHECK(j["device"] == "gpu");
        CHECK(j["model_name"] == "askmdb-4b");
        CHECK(again.err.empty());

        // Without that GPU now: the CPU with askmdb-0.8b, a warning, and the choice stays.
        const std::string saved = sb.state();
        sb.test_hw.set("none");
        Term gone;
        gone.tty = true;
        CHECK(gone.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
        const Json jg = one_object(gone.out);
        CHECK(jg["device"] == "cpu");
        CHECK(jg["model_name"] == "askmdb-0.8b");
        CHECK(has(gone.err, "la GPU elegida (RTX) no está disponible; se usa la CPU con askmdb-0.8b"));
        CHECK(sb.state() == saved);
    }
    SUBCASE("solo integrada: askmdb-0.8b en la CPU") {
        sb.test_hw.set("integrated:Intel:3000:3000");
        Term t;
        t.tty = true;
        t.input = {"s"};
        CHECK(t.run({"--lang", "en", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
        CHECK(has(t.err, "GPU Vulkan0: Intel (integrated)"));
        CHECK(has(t.err, "Recommended: askmdb-0.8b on the CPU"));
        CHECK_FALSE(has(t.err, "instead"));
        const std::string st = sb.state();
        CHECK(has(st, "device = cpu\n"));
        CHECK(has(st, "model = askmdb-0.8b\n"));
        CHECK_FALSE(has(st, "gpu_id"));
    }
    SUBCASE("GPU con ≥ 8 GiB libres: el usuario elige 4B o 9B") {
        sb.test_hw.set("discrete:RTX:12000:16000");
        const struct {
            std::deque<std::string> input;
            const char* model;
        } cases[] = {{{"1"}, "askmdb-4b"},    {{" 4B "}, "askmdb-4b"}, {{""}, "askmdb-4b"},
                     {{"2"}, "askmdb-9b"},    {{"9b"}, "askmdb-9b"},   {{"x", "zz", "1"}, "askmdb-4b"}};
        for (const auto& c : cases) {
            std::filesystem::remove(sb.state_file());
            Term t;
            t.tty = true;
            t.input = c.input;
            CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
            CHECK(has(t.err, "La GPU RTX tiene memoria para los dos modelos de GPU. ¿Cuál quieres instalar?\n"));
            CHECK(has(t.err, "  1) askmdb-4b (1 MB): rápido y preciso\n"));
            CHECK(has(t.err, "  2) askmdb-9b (1 MB): un pelín más lento, pero más preciso\n"));
            CHECK(has(t.err, "Elige 1 o 2 (Intro = 1, n = ninguno): "));
            CHECK_FALSE(has(t.err, "Recomendado:"));
            CHECK_FALSE(has(t.err, "[S/n]"));  // choosing one is the yes
            CHECK(has(t.err, std::string("Elección guardada: ") + c.model + " en la GPU RTX"));
            CHECK(has(sb.state(), std::string("model = ") + c.model + "\n"));
            CHECK(t.input.empty());
        }
    }
    SUBCASE("ninguno de los dos de GPU: se ofrece askmdb-0.8b en la CPU") {
        sb.test_hw.set("discrete:RTX:12000:16000");
        Term t;
        t.tty = true;
        t.input = {"n", ""};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
        CHECK(has(t.err, "¿Cuál quieres instalar?"));
        CHECK(has(t.err, "¿Prefieres askmdb-0.8b en la CPU (1 MB)? [S/n]"));
        CHECK(has(sb.state(), "device = cpu\n"));
    }
    SUBCASE("tres respuestas no válidas cuentan como ninguno") {
        sb.test_hw.set("discrete:RTX:12000:16000");
        Term t;
        t.tty = true;
        t.input = {"x", "y", "3", "n"};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 1);
        CHECK(has(t.err, "¿Prefieres askmdb-0.8b en la CPU"));
        CHECK_FALSE(std::filesystem::exists(sb.state_file()));
    }
    SUBCASE("rechaza las dos: sale sin elegir nada") {
        sb.test_hw.set("discrete:RTX:12000:16000");
        Term t;
        t.tty = true;
        t.input = {"n", "n"};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 1);
        CHECK(has(t.err, "no se ha elegido ningún modelo"));
        CHECK_FALSE(std::filesystem::exists(sb.state_file()));
        CHECK(t.out.empty());
    }
    SUBCASE("la GPU falla la comprobación: vuelve a la CPU") {
        sb.test_hw.set("discrete:RTX:6000:8000");
        sb.gpu_error.set("load_failed");
        Term t;
        t.tty = true;
        t.input = {""};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
        CHECK(has(t.err, "el modelo no funciona en la GPU (load_failed); se usa la CPU"));
        CHECK(has(t.err, "Comprobando que el modelo funciona en la CPU"));
        CHECK(has(t.err, "Elección guardada: askmdb-0.8b en la CPU"));
        CHECK_FALSE(has(t.err, "La GPU ya está preparada"));
        CHECK(has(sb.state(), "device = cpu\n"));
    }
    SUBCASE("un estado roto cuenta como ausente") {
        sb.test_hw.set("none");
        std::filesystem::create_directories(sb.state_file().parent_path());
        std::ofstream(sb.state_file()) << "device = tpu\nmodel = askmdb-4b\n";
        Term t;
        t.tty = true;
        t.input = {""};
        CHECK(t.run({"--mode", "suggest", "--schema-from", block, "q"}) == 0);
        CHECK(has(t.err, "Recomendado: askmdb-0.8b en la CPU"));
        CHECK(has(sb.state(), "device = cpu\n"));
    }
}

TEST_CASE("precedencia de dispositivo y modelo") {
    Sandbox sb;
    sb.family();
    const std::string block = sb.file("s.txt", kTiendaBlock);
    for (const auto& name : {"askmdb-0.8b-1.0.0-Q4_K_M.gguf", "askmdb-4b-1.0.0-Q4_K_M.gguf"}) sb.model(name);
    sb.test_hw.set("discrete:RTX:6000:8000");  // the saved GPU is there
    askmdb::config::State gpu;
    gpu.device = askmdb::infer::Device::Gpu;
    gpu.model = askmdb::infer::ModelTier::M4B;
    gpu.gpu_id = "Vulkan0";
    gpu.gpu_name = "RTX";
    std::string error;
    REQUIRE(askmdb::config::save_state(gpu, error));
    const std::string saved = sb.state();
    const auto json_run = [&](std::vector<std::string> extra) {
        Term t;
        std::vector<std::string> args{"--json", "--mode", "suggest", "--schema-from", block};
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back("q");
        const int code = t.run(args);
        return std::make_tuple(code, t.out.empty() ? Json() : one_object(t.out), t.err);
    };

    auto [code, j, err] = json_run({});
    CHECK(code == 0);
    CHECK(j["device"] == "gpu");
    CHECK(j["model_name"] == "askmdb-4b");

    std::tie(code, j, err) = json_run({"--device", "cpu"});
    CHECK(j["device"] == "cpu");
    CHECK(j["model_name"] == "askmdb-0.8b");

    sb.config("device = cpu\n");
    std::tie(code, j, err) = json_run({});
    CHECK(j["device"] == "cpu");
    std::tie(code, j, err) = json_run({"--device", "auto"});  // the flag beats the config
    CHECK(j["device"] == "gpu");
    sb.config("");

    // --model beats the saved model, on the saved device; a file of the family keeps its name.
    const std::string own = sb.file("propio.gguf", "x");
    std::tie(code, j, err) = json_run({"--model", own});
    CHECK(code == 0);
    CHECK(j["device"] == "gpu");
    CHECK(j["model_name"].is_null());
    std::tie(code, j, err) =
        json_run({"--model", (sb.dir / "data" / "askmdb" / "models" / "askmdb-0.8b-1.0.0-Q4_K_M.gguf").string()});
    CHECK(j["model_name"] == "askmdb-0.8b");

    // The saved GPU is gone: warning, the CPU with askmdb-0.8b, and the state stays.
    sb.gpu_error.set("gpu_unavailable");
    std::tie(code, j, err) = json_run({});
    CHECK(code == 0);
    CHECK(j["device"] == "cpu");
    CHECK(j["model_name"] == "askmdb-0.8b");
    CHECK(j["error"].is_null());
    CHECK(has(err, "la GPU elegida (RTX) no está disponible; se usa la CPU con askmdb-0.8b"));
    CHECK(sb.state() == saved);
    // A GPU that fails while decoding: the same.
    sb.gpu_error.set("decode_failed");
    std::tie(code, j, err) = json_run({});
    CHECK(code == 0);
    CHECK(j["device"] == "cpu");
    CHECK(j["model_name"] == "askmdb-0.8b");
    CHECK(has(err, "la GPU elegida (RTX) no está disponible"));
    CHECK(sb.state() == saved);
    sb.gpu_error.set("gpu_unavailable");
    // Forced: no fallback.
    std::tie(code, j, err) = json_run({"--device", "gpu"});
    CHECK(code == 1);
    CHECK(j["error"] == "gpu_unavailable");
    CHECK(has(err, "no hay ninguna GPU disponible"));
    // Without askmdb-0.8b and without a terminal: the usual error.
    std::filesystem::remove(sb.dir / "data" / "askmdb" / "models" / "askmdb-0.8b-1.0.0-Q4_K_M.gguf");
    std::tie(code, j, err) = json_run({});
    CHECK(code == 1);
    CHECK(j["error"] == "model_not_downloaded");
    CHECK(sb.state() == saved);
    sb.gpu_error.unset();

    // --device gpu without a saved GPU and without a discrete GPU: gpu_unavailable (integrated
    // and software devices are never used, not even when forced).
    REQUIRE(askmdb::config::forget_state());
    for (const char* hw : {"none", "integrated:Intel:5000:6000", "software:llvmpipe:9000:9000"}) {
        CAPTURE(hw);
        sb.test_hw.set(hw);
        std::tie(code, j, err) = json_run({"--device", "gpu"});
        CHECK(code == 1);
        CHECK(j["error"] == "gpu_unavailable");
    }
    // With a discrete one too small for choose(): askmdb-4b on it.
    sb.test_hw.set("integrated:Intel:9000:9000;discrete:GTX:2000:4000");
    std::tie(code, j, err) = json_run({"--device", "gpu"});
    CHECK(code == 0);
    CHECK(j["device"] == "gpu");
    CHECK(j["model_name"] == "askmdb-4b");
    CHECK_FALSE(std::filesystem::exists(sb.state_file()));  // forced: nothing saved
    // A saved GPU that is gone, forced: another discrete GPU, with the model by its memory.
    REQUIRE(askmdb::config::save_state(gpu, error));
    sb.test_hw.set("discrete:GTX:2000:4000");
    std::tie(code, j, err) = json_run({"--device", "gpu"});
    CHECK(code == 0);
    CHECK(j["device"] == "gpu");
    CHECK(j["model_name"] == "askmdb-4b");
    CHECK(sb.state() == saved);
}

TEST_CASE("la GPU guardada se busca en el hardware de ahora") {
    Sandbox sb;
    sb.family();
    const std::string block = sb.file("s.txt", kTiendaBlock);
    for (const auto& name : {"askmdb-0.8b-1.0.0-Q4_K_M.gguf", "askmdb-9b-1.0.0-Q4_K_M.gguf"}) sb.model(name);
    askmdb::config::State gpu;
    gpu.device = askmdb::infer::Device::Gpu;
    gpu.model = askmdb::infer::ModelTier::M9B;
    gpu.gpu_id = "Vulkan1";
    gpu.gpu_name = "RTX 4080 Super";
    std::string error;
    REQUIRE(askmdb::config::save_state(gpu, error));
    const std::string saved = sb.state();

    struct Case {
        const char* hw;
        bool gpu;
    };
    for (const Case& c : {
             Case{"discrete:GTX:9000:9000;discrete:RTX 4080 Super:1000:16000", true},     // same id and name
             Case{"discrete:RTX 4080 Super:12000:16000", true},                           // id moved, one by name
             Case{"discrete:GTX:9000:9000;discrete:RTX 4080:12000:16000", false},      // same id, other name
             Case{"discrete:RTX 4080 Super:9000:9000;discrete:GTX:1:1;discrete:RTX 4080 Super:9000:9000", false},
             Case{"integrated:X:1:1;integrated:RTX 4080 Super:9000:9000", false},         // never an iGPU
             Case{"software:RTX 4080 Super:9000:9000", false},
             Case{"none", false},
         }) {
        CAPTURE(c.hw);
        sb.test_hw.set(c.hw);
        Term t;
        CHECK(t.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 0);
        const Json j = one_object(t.out);
        CHECK(j["error"].is_null());
        CHECK(j["device"] == (c.gpu ? "gpu" : "cpu"));
        CHECK(j["model_name"] == (c.gpu ? "askmdb-9b" : "askmdb-0.8b"));
        CHECK(has(t.err, "la GPU elegida (RTX 4080 Super) no está disponible") != c.gpu);
        CHECK(sb.state() == saved);
    }

    // Gone and without askmdb-0.8b: its download is what is missing (the 9b is not asked for).
    std::filesystem::remove(sb.dir / "data" / "askmdb" / "models" / "askmdb-0.8b-1.0.0-Q4_K_M.gguf");
    std::filesystem::remove(sb.dir / "data" / "askmdb" / "models" / "askmdb-9b-1.0.0-Q4_K_M.gguf");
    sb.test_hw.set("none");
    Term t;
    CHECK(t.run({"--json", "--mode", "suggest", "--schema-from", block, "q"}) == 1);
    CHECK(one_object(t.out)["error"] == "model_not_downloaded");
    CHECK(has(t.err, "askmdb-0.8b-1.0.0-Q4_K_M.gguf"));
    CHECK_FALSE(has(t.err, "askmdb-9b-1.0.0-Q4_K_M.gguf"));
    CHECK(sb.state() == saved);
}

TEST_CASE("--detect") {
    Sandbox sb;
    sb.test_hw.set("discrete:RTX 4080:12000:16000;integrated:Intel:1000:2000");
    SUBCASE("--json: solo el JSON, nada guardado") {
        Term t;
        t.tty = true;
        CHECK(t.run({"--detect", "--json"}) == 0);
        const Json j = one_object(t.out);
        std::vector<std::string> keys;
        for (const auto& [k, v] : j.items()) keys.push_back(k);
        CHECK(keys ==
              std::vector<std::string>{"vulkan", "gpus", "ram_total_mib", "ram_available_mib", "cores", "choice"});
        CHECK(j["vulkan"] == true);
        REQUIRE(j["gpus"].size() == 2);
        CHECK(j["gpus"][0] ==
              Json::parse(R"({"id":"Vulkan0","name":"RTX 4080","kind":"discrete","free_mib":12000,"total_mib":16000})"));
        CHECK(j["gpus"][1]["kind"] == "integrated");
        CHECK(j["ram_total_mib"].get<std::uint64_t>() > 0);
        CHECK(j["cores"].get<int>() > 0);
        CHECK(j["choice"] == Json::parse(R"({"device":"gpu","gpu":"Vulkan0","model":"askmdb-9b"})"));
        CHECK(t.err.empty());
        CHECK_FALSE(std::filesystem::exists(sb.state_file()));

        sb.test_hw.set("none");
        Term none;
        CHECK(none.run({"--detect", "--json"}) == 0);
        const Json jn = one_object(none.out);
        CHECK(jn["vulkan"] == false);
        CHECK(jn["gpus"] == Json::array());
        CHECK(jn["choice"] == Json::parse(R"({"device":"cpu","gpu":null,"model":"askmdb-0.8b"})"));
    }
    SUBCASE("sin terminal: resumen legible, nada guardado") {
        Term t;
        CHECK(t.run({"--detect", "--lang", "en"}) == 0);
        CHECK(has(t.out, "Detected hardware:\n"));
        CHECK(has(t.out, "  GPU Vulkan0: RTX 4080 (discrete), 12000 MiB free of 16000 MiB\n"));
        CHECK(has(t.out, "Choice: askmdb-9b on the GPU RTX 4080\n"));
        CHECK_FALSE(std::filesystem::exists(sb.state_file()));
        CHECK_FALSE(has(t.out, "libvulkan.so.1"));

        // No loader: a Vulkan build tells the user to install it; a build without Vulkan does not.
        sb.test_hw.set("none");
        Term none;
        CHECK(none.run({"--detect", "--lang", "en"}) == 0);
        if (askmdb::infer::vulkan_built()) {
            CHECK(has(none.out, "  Vulkan: libvulkan.so.1 not found. To use the GPU, install the Vulkan loader\n"));
            CHECK(has(none.out, "libvulkan1 on Debian/Ubuntu"));
        } else {
            CHECK(has(none.out, "  Vulkan: no\n"));
        }
        CHECK(has(none.out, "Choice: askmdb-0.8b on the CPU\n"));
    }
    SUBCASE("en un terminal: olvida lo guardado y vuelve a elegir") {
        sb.family();
        sb.model("askmdb-9b-1.0.0-Q4_K_M.gguf");
        askmdb::config::State old;
        std::string error;
        REQUIRE(askmdb::config::save_state(old, error));
        Term t;
        t.tty = true;
        t.input = {"2"};
        CHECK(t.run({"--detect"}) == 0);
        CHECK(has(t.err, "La GPU RTX 4080 tiene memoria para los dos modelos de GPU"));
        CHECK(t.out.empty());
        CHECK(has(sb.state(), "model = askmdb-9b\n"));
        CHECK_FALSE(has(t.err, "Nota:"));
        sb.config("device = cpu\n");
        Term noted;
        noted.tty = true;
        noted.input = {"2"};
        CHECK(noted.run({"--detect"}) == 0);
        CHECK(has(noted.err, "Nota: «device» o «model» de la configuración mandan"));
    }
    SUBCASE("en un terminal: rechazar deja la elección anterior") {
        sb.family();
        askmdb::config::State old;
        old.model = askmdb::infer::ModelTier::M4B;
        std::string error;
        REQUIRE(askmdb::config::save_state(old, error));
        const std::string saved = sb.state();
        Term declined;
        declined.tty = true;
        declined.input = {"n", "n"};
        CHECK(declined.run({"--detect"}) == 1);
        CHECK(has(declined.err, "no se ha elegido ningún modelo"));
        CHECK(sb.state() == saved);
        Term eof;  // EOF (as after Ctrl-D) answers no
        eof.tty = true;
        CHECK(eof.run({"--detect"}) == 1);
        CHECK(sb.state() == saved);
    }
    SUBCASE("con pregunta: error de uso") {
        Term t;
        CHECK(t.run({"--detect", "pedidos"}) == 2);
        CHECK(has(t.err, "askmdb: --detect no lleva pregunta"));
    }
}

}  // TEST_SUITE("cli")

// Needs the base GGUF: ASKMDB_TEST_MODEL=<path to the Qwen3.5-0.8B Q4_K_M GGUF>
TEST_SUITE("model") {

TEST_CASE("CLI con el modelo: too_long y timeout, sin ejecutar nada") {
    const char* model = std::getenv("ASKMDB_TEST_MODEL");
    if (model == nullptr || *model == '\0') {
        MESSAGE("ASKMDB_TEST_MODEL is not set: skipped");
        return;
    }
    // Same device as the rest of the model suite (ASKMDB_TEST_DEVICE=gpu): ggml's device registry
    // is created once per process, so a first CPU run would leave Vulkan off for the later tests.
    const char* device_env = std::getenv("ASKMDB_TEST_DEVICE");
    const std::string device = device_env != nullptr && std::string(device_env) == "gpu" ? "gpu" : "cpu";
    Sandbox sb;
    if (device == "gpu") sb.test_hw.unset();  // the real GPU
    SUBCASE("too_long: un bloque grande y una pregunta de 1000 bytes") {
        // Digits are one token each: ~4,000 tokens of block plus 1,000 of question are past
        // 4096 - 384, while the block still fits the byte budget.
        askmdb::schema::Collection c{"c", {}};
        for (int i = 0; i < 40; ++i) {
            askmdb::schema::Path p{"p" + std::to_string(10 + i), {"str"}, false, {}};
            for (int v = 0; v < 12; ++v) p.values.push_back(std::to_string(1000000 + i * 100 + v) + "1357924680");
            c.paths.push_back(p);
        }
        const askmdb::schema::Fit fit = askmdb::schema::render(askmdb::schema::Schema{"grande", {c}},
                                                               askmdb::schema::kSchemaBudgetBytes);
        REQUIRE(fit.ok);
        REQUIRE(fit.block.size() > askmdb::schema::kSchemaBudgetBytes - 300);
        const std::string block = sb.file("grande.txt", fit.block);
        Term t;
        CHECK(t.run({"--json", "--mode", "suggest", "--device", device, "--schema-from", block, "--model", model,
                     std::string(1000, '7')}) == 1);
        const Json j = one_object(t.out);
        CHECK(j["error"] == "too_long");
        CHECK(j["executed"] == false);
        CHECK(j["model"] == std::filesystem::path(model).filename().string());
    }
    SUBCASE("timeout: el gancho de test baja el límite a 1 ms") {
        sb.timeout.set("1");
        Term t;
        CHECK(t.run({"--json", "--mode", "suggest", "--device", device, "--schema-from", sb.file("s.txt", kTiendaBlock),
                     "--model", model, "pedidos pendientes de Madrid"}) == 1);
        const Json j = one_object(t.out);
        CHECK(j["error"] == "timeout");
        CHECK(j["executed"] == false);
        CHECK(j["rows"].is_null());
        CHECK(has(t.err, "ha tardado demasiado"));
    }
}

}  // TEST_SUITE("model")
