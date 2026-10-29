#include "doctest.h"

#include "config/config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace askmdb::config;

namespace {

const char* const kFile = "~/.config/askmdb/config";

ConfigResult parse(std::string_view text, std::string_view home = "/home/ana") {
    return parse_config(text, kFile, home);
}

// Checks that it fails on the given line and returns the result to inspect the messages.
ConfigResult parse_err(std::string_view text, int line) {
    ConfigResult r = parse(text);
    CHECK_FALSE(r.config.has_value());
    const std::string prefix = std::string(kFile) + ":" + std::to_string(line) + ": ";
    CHECK(r.error.rfind(prefix, 0) == 0);
    CHECK(r.error_en.rfind(prefix, 0) == 0);
    return r;
}

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

// Own temporary directory, removed on exit.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("askmdb_config_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& p, const std::string& text, mode_t mode = 0600) {
    std::filesystem::create_directories(p.parent_path());
    {
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    ::chmod(p.c_str(), mode);
}

}  // namespace

TEST_SUITE("config") {

TEST_CASE("texto vacío y solo comentarios: valores por defecto") {
    for (const char* text : {"", "\n\n", "# comentario\n", "   # con espacios\n\t# con tab\n", "\r\n# x\r\n"}) {
        CAPTURE(text);
        const ConfigResult r = parse(text);
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Confirm);
        CHECK(r.config->lang == Lang::Auto);
        CHECK(r.config->model.empty());
        CHECK(r.config->threads == 0);
        CHECK(r.config->device == DeviceMode::Auto);
        CHECK(r.config->uri.empty());
        CHECK(r.config->collections.empty());
        CHECK(r.config->max_rows == 50);
        CHECK(r.config->max_time_ms == 10000);
        CHECK(r.error.empty());
        CHECK(r.error_en.empty());
    }
}

TEST_CASE("mode y lang") {
    CHECK(parse("mode = suggest").config->mode == Mode::Suggest);
    CHECK(parse("mode = confirm").config->mode == Mode::Confirm);
    CHECK(parse("mode = auto").config->mode == Mode::Auto);
    const ConfigResult r = parse("# askmdb\n\nmode = autom\n");
    CHECK(r.error == "~/.config/askmdb/config:3: valor no válido para mode: \"autom\" (válidos: suggest, confirm, auto)");
    CHECK(r.error_en == "~/.config/askmdb/config:3: invalid value for mode: \"autom\" (valid: suggest, confirm, auto)");
    for (const char* text : {"mode = Auto", "mode = auto auto", "mode = 1"}) parse_err(text, 1);

    CHECK(parse("lang = auto").config->lang == Lang::Auto);
    CHECK(parse("lang = es").config->lang == Lang::Es);
    CHECK(parse("lang = en").config->lang == Lang::En);
    for (const char* text : {"lang = ES", "lang = fr", "lang = es_ES"}) parse_err(text, 1);
}

TEST_CASE("model y threads") {
    CHECK(parse("model = /opt/m.gguf").config->model == "/opt/m.gguf");
    CHECK(parse("model = ~/models/m.gguf").config->model == "/home/ana/models/m.gguf");
    CHECK(parse_config("model = ~/m.gguf", kFile, "/home/ana/").config->model == "/home/ana/m.gguf");
    CHECK(parse("model = /a/m.gguf # no es comentario").config->model == "/a/m.gguf # no es comentario");
    CHECK(parse("model = ~user/m.gguf").config->model == "~user/m.gguf");
    const ConfigResult nohome = parse_config("model = ~/m.gguf", kFile, "");
    CHECK(nohome.error == "~/.config/askmdb/config:1: no se puede expandir \"~/\" en model: HOME no está definida");

    CHECK(parse("threads = 1").config->threads == 1);
    CHECK(parse("threads = 256").config->threads == 256);
    CHECK(parse("threads = 004").config->threads == 4);
    for (const char* text : {"threads = 0", "threads = 257", "threads = -1", "threads = +4", "threads = 4x",
                             "threads = 99999999999999999999", "threads = 1.5"})
        parse_err(text, 1);
    const ConfigResult r = parse("mode = auto\nthreads = 257\n");
    CHECK(r.error == "~/.config/askmdb/config:2: valor no válido para threads: \"257\" (válidos: entero de 1 a 256)");
    CHECK(r.error_en == "~/.config/askmdb/config:2: invalid value for threads: \"257\" (valid: integer from 1 to 256)");
}

TEST_CASE("device") {
    CHECK(parse("device = auto").config->device == DeviceMode::Auto);
    CHECK(parse("device = cpu").config->device == DeviceMode::Cpu);
    CHECK(parse("device = gpu").config->device == DeviceMode::Gpu);
    for (const char* text : {"device = GPU", "device = vulkan", "device = cuda"}) parse_err(text, 1);
    const ConfigResult r = parse("device = tpu\n");
    CHECK(r.error == "~/.config/askmdb/config:1: valor no válido para device: \"tpu\" (válidos: auto, cpu, gpu)");
    CHECK(r.error_en == "~/.config/askmdb/config:1: invalid value for device: \"tpu\" (valid: auto, cpu, gpu)");
}

TEST_CASE("max_rows y max_time_ms: rangos") {
    CHECK(parse("max_rows = 1").config->max_rows == 1);
    CHECK(parse("max_rows = 1000").config->max_rows == 1000);
    CHECK(parse("max_time_ms = 100").config->max_time_ms == 100);
    CHECK(parse("max_time_ms = 300000").config->max_time_ms == 300000);
    for (const char* text : {"max_rows = 0", "max_rows = 1001", "max_rows = -5", "max_rows = 10 filas",
                             "max_time_ms = 99", "max_time_ms = 300001", "max_time_ms = 1e4"})
        parse_err(text, 1);
    const ConfigResult r = parse("max_rows = 0");
    CHECK(r.error == "~/.config/askmdb/config:1: valor no válido para max_rows: \"0\" (válidos: entero de 1 a 1000)");
    CHECK(r.error_en == "~/.config/askmdb/config:1: invalid value for max_rows: \"0\" (valid: integer from 1 to 1000)");
}

TEST_CASE("collections") {
    CHECK(parse("collections = pedidos").config->collections == std::vector<std::string>{"pedidos"});
    CHECK(parse("collections = pedidos, clientes ,,pedidos,").config->collections ==
          std::vector<std::string>{"pedidos", "clientes"});
    parse_err("collections = , ,", 1);
    CHECK(split_list(" a , b,a ") == std::vector<std::string>{"a", "b"});
    CHECK(split_list("").empty());
}

TEST_CASE("uri: esquema y sin eco del valor") {
    CHECK(parse("uri = mongodb://h:27017/db").config->uri == "mongodb://h:27017/db");
    CHECK(parse("uri = mongodb+srv://ana:x@c.example.net/db?tls=true").config->uri ==
          "mongodb+srv://ana:x@c.example.net/db?tls=true");
    const ConfigResult r = parse_err("uri = http://ana:secreto@h/db", 1);
    CHECK(r.error == "~/.config/askmdb/config:1: valor no válido para uri (tiene que empezar por mongodb:// o "
                     "mongodb+srv://)");
    CHECK(r.error.find("secreto") == std::string::npos);
    CHECK(r.error_en.find("secreto") == std::string::npos);
}

TEST_CASE("uri_has_password") {
    CHECK(uri_has_password("mongodb://ana:secreto@h/db"));
    CHECK(uri_has_password("mongodb+srv://ana:s%40c@cluster.example.net/"));
    CHECK(uri_has_password("mongodb://ana:p@ss@h1,h2/db"));     // last "@" ends the userinfo
    CHECK(uri_has_password("mongodb://ana:a/b@h/db"));          // unencoded "/" still counts
    CHECK_FALSE(uri_has_password("mongodb://ana@h/db"));        // user only
    CHECK_FALSE(uri_has_password("mongodb://ana:@h/db"));       // empty password
    CHECK_FALSE(uri_has_password("mongodb://h:27017/db"));
    CHECK_FALSE(uri_has_password("mongodb://h/db?authMechanismProperties=a:b@c"));  // "@" in the options
    CHECK_FALSE(uri_has_password("http://ana:x@h/"));
    CHECK_FALSE(uri_has_password(""));
}

TEST_CASE("todas las claves juntas, con espacios, tabuladores, CRLF y comentarios") {
    const ConfigResult r = parse("# config de askmdb\r\n"
                                 "\r\n"
                                 "  mode\t=\tauto  \r\n"
                                 "lang=en\r\n"
                                 "\t model =  ~/m/q.gguf \t\r\n"
                                 "threads   =   12\r\n"
                                 "device = gpu\r\n"
                                 "uri = mongodb://localhost/tienda\r\n"
                                 "collections = pedidos,clientes\r\n"
                                 "max_rows = 20\r\n"
                                 "max_time_ms = 5000\r\n"
                                 "   \t  \r\n");
    REQUIRE(r.config.has_value());
    CHECK(r.config->mode == Mode::Auto);
    CHECK(r.config->lang == Lang::En);
    CHECK(r.config->model == "/home/ana/m/q.gguf");
    CHECK(r.config->threads == 12);
    CHECK(r.config->device == DeviceMode::Gpu);
    CHECK(r.config->uri == "mongodb://localhost/tienda");
    CHECK(r.config->collections == std::vector<std::string>{"pedidos", "clientes"});
    CHECK(r.config->max_rows == 20);
    CHECK(r.config->max_time_ms == 5000);
}

TEST_CASE("BOM, clave desconocida, repetida, sin = y valor vacío") {
    CHECK(parse("\xEF\xBB\xBFmode = suggest\n").config->mode == Mode::Suggest);

    const ConfigResult unknown = parse_err("mode = auto\n\nmodo = auto\n", 3);
    CHECK(unknown.error == "~/.config/askmdb/config:3: clave desconocida: \"modo\" (válidas: mode, lang, model, "
                           "threads, device, uri, collections, max_rows, max_time_ms)");
    CHECK(unknown.error_en == "~/.config/askmdb/config:3: unknown key: \"modo\" (valid: mode, lang, model, threads, "
                              "device, uri, collections, max_rows, max_time_ms)");
    parse_err("MODE = auto", 1);
    parse_err("= auto", 1);

    const ConfigResult dup = parse_err("uri = mongodb://a/b\nlang = es\nuri = mongodb://c/d\n", 3);
    CHECK(dup.error == "~/.config/askmdb/config:3: clave repetida: uri (ya definida en la línea 1)");
    CHECK(dup.error_en == "~/.config/askmdb/config:3: duplicate key: uri (already set on line 1)");

    const ConfigResult noeq = parse_err("mode = auto\nlang es\n", 2);
    CHECK(noeq.error == "~/.config/askmdb/config:2: falta \"=\" (formato: clave = valor)");
    CHECK(noeq.error_en == "~/.config/askmdb/config:2: missing \"=\" (format: key = value)");

    const ConfigResult empty = parse_err("lang = es\nuri =   \n", 2);
    CHECK(empty.error == "~/.config/askmdb/config:2: valor vacío para uri");
    CHECK(empty.error_en == "~/.config/askmdb/config:2: empty value for uri");

    const ConfigResult first = parse("mode = mal\nlang = mal\n");
    CHECK(first.error.find(":1: ") != std::string::npos);
}

TEST_CASE("seguridad: el valor citado no lleva caracteres de control") {
    const ConfigResult r = parse("mode = \x1b[31mrojo\x07\x7f\"\\\n");
    CHECK(r.error == "~/.config/askmdb/config:1: valor no válido para mode: \"\\x1b[31mrojo\\x07\\x7f\\\"\\\\\" "
                     "(válidos: suggest, confirm, auto)");
    for (const std::string* msg : {&r.error, &r.error_en})
        for (char c : *msg) {
            const auto u = static_cast<unsigned char>(c);
            CHECK((u >= 0x20 && u != 0x7f));
        }
    const ConfigResult c = parse("lang = \xC2\x9B" "31m\xFF\xC3 añ\n");
    CHECK(c.error.find("\"\\xc2\\x9b31m\\xff\\xc3 añ\"") != std::string::npos);
}

TEST_CASE("config_path: XDG_CONFIG_HOME definida, vacía y sin definir") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    home.set("/home/ana");
    xdg.set("/tmp/xdg/");
    CHECK(config_path() == "/tmp/xdg/askmdb/config");
    xdg.set("");
    CHECK(config_path() == "/home/ana/.config/askmdb/config");
    xdg.unset();
    CHECK(config_path() == "/home/ana/.config/askmdb/config");
    home.unset();
    CHECK(config_path().empty());
    xdg.set("/tmp/xdg");
    CHECK(config_path() == "/tmp/xdg/askmdb/config");  // XDG does not need HOME
}

TEST_CASE("load_config: HOME temporal") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    TempDir tmp;
    xdg.unset();
    home.set(tmp.path.string());
    const auto file = tmp.path / ".config" / "askmdb" / "config";

    SUBCASE("fichero ausente: valores por defecto y no se crea") {
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Confirm);
        CHECK_FALSE(std::filesystem::exists(tmp.path / ".config"));
    }
    SUBCASE("fichero válido, ~/ se expande con HOME") {
        write_file(file, "mode = suggest\nmodel = ~/m.gguf\nmax_rows = 7\n", 0644);
        const ConfigResult r = load_config();
        REQUIRE(r.config.has_value());
        CHECK(r.config->mode == Mode::Suggest);
        CHECK(r.config->model == (tmp.path / "m.gguf").string());
        CHECK(r.config->max_rows == 7);
    }
    SUBCASE("fichero inválido: la ruta se muestra con ~/") {
        write_file(file, "mode = auto\n\nmode = suggest\n");
        const ConfigResult r = load_config();
        CHECK_FALSE(r.config.has_value());
        CHECK(r.error.rfind("~/.config/askmdb/config:3: ", 0) == 0);
        CHECK(r.error_en.rfind("~/.config/askmdb/config:3: ", 0) == 0);
    }
    SUBCASE("la ruta es un directorio: error de lectura") {
        std::filesystem::create_directories(file);
        const ConfigResult r = load_config();
        CHECK_FALSE(r.config.has_value());
        CHECK(r.error == "~/.config/askmdb/config: no se puede leer: es un directorio");
        CHECK(r.error_en == "~/.config/askmdb/config: cannot read: is a directory");
    }
    SUBCASE("uri con contraseña: solo con 0600") {
        const std::string text = "uri = mongodb://ana:secreto@h/db\n";
        write_file(file, text, 0600);
        CHECK(load_config().config.has_value());
        write_file(file, text, 0400);
        CHECK(load_config().config.has_value());
        for (mode_t mode : {0644, 0640, 0604, 0660, 0606}) {
            CAPTURE(mode);
            write_file(file, text, mode);
            const ConfigResult r = load_config();
            CHECK_FALSE(r.config.has_value());
            CHECK(r.error.rfind("~/.config/askmdb/config: la uri lleva contraseña y el fichero tiene permisos 0", 0) == 0);
            CHECK(r.error_en.rfind("~/.config/askmdb/config: the uri has a password and the file has mode 0", 0) == 0);
            CHECK(r.error.find("chmod 600") != std::string::npos);
            CHECK(r.error.find("secreto") == std::string::npos);
            CHECK(r.error_en.find("secreto") == std::string::npos);
        }
        write_file(file, text, 0644);
        CHECK(load_config().error == "~/.config/askmdb/config: la uri lleva contraseña y el fichero tiene permisos "
                                     "0644; tiene que ser 0600 (chmod 600 ~/.config/askmdb/config)");
    }
    SUBCASE("uri sin contraseña: cualquier modo") {
        write_file(file, "uri = mongodb://ana@h/db\n", 0644);
        CHECK(load_config().config.has_value());
    }
}

TEST_CASE("load_config: XDG_CONFIG_HOME temporal") {
    EnvGuard xdg("XDG_CONFIG_HOME");
    EnvGuard home("HOME");
    TempDir tmp;
    home.set("/nonexistent-home-askmdb");
    xdg.set((tmp.path / "xdg").string());
    const auto file = tmp.path / "xdg" / "askmdb" / "config";

    CHECK(load_config().config.has_value());
    CHECK_FALSE(std::filesystem::exists(file));
    write_file(file, "lang = en\r\n");
    CHECK(load_config().config->lang == Lang::En);
    write_file(file, "threads = 0\n");
    const ConfigResult r = load_config();
    CHECK_FALSE(r.config.has_value());
    CHECK(r.error.rfind(file.string() + ":1: ", 0) == 0);  // outside HOME: full path
}

}  // TEST_SUITE("config")
