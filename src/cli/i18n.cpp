#include "cli/i18n.hpp"

#include <cstdlib>

namespace askmdb::cli {

namespace {

struct Entry {
    Msg msg;
    const char* es;
    const char* en;
};

// One row per message, in the same order as the enum.
constexpr Entry kTable[] = {
    {Msg::Help,
     "Uso: askmdb [opciones] <pregunta…>\n"
     "\n"
     "Convierte una pregunta en lenguaje natural en un pipeline de aggregate de MongoDB,\n"
     "lo explica, comprueba que solo lee y lo ejecuta si lo confirmas.\n"
     "\n"
     "Opciones:\n"
     "  --uri URI            URI de MongoDB (si no: ASKMDB_URI o «uri» de la configuración)\n"
     "  --db BASE            base de datos (si no: la de la URI)\n"
     "  -c, --collections a,b  colecciones que se dan al modelo (si no: todas, hasta 50)\n"
     "  --refresh            vuelve a muestrear el esquema aunque la caché esté al día\n"
     "  --print-schema       escribe el bloque de esquema y termina (sin pregunta)\n"
     "  --print-prompt       escribe el prompt exacto que vería el modelo y termina\n"
     "  --schema-from FICHERO  usa un bloque guardado con --print-schema en vez de muestrear;\n"
     "                       solo conecta si hay que ejecutar\n"
     "  --mode MODO          suggest | confirm | auto\n"
     "  --json               un objeto JSON en la salida; nunca pregunta; ejecuta solo en auto\n"
     "  --model RUTA         usa este fichero GGUF en lugar del modelo descargado\n"
     "  --device MODO        auto | cpu | gpu: dónde corre el modelo (auto: lo elegido en el\n"
     "                       primer inicio)\n"
     "  --detect             vuelve a detectar el hardware y a elegir dispositivo y modelo\n"
     "  --lang es|en         idioma de la interfaz\n"
     "  -h, --help           muestra esta ayuda\n"
     "  --version            muestra la versión\n"
     "\n"
     "Respuestas: s = ejecutar, n = no, c = copiar la consulta (sintaxis mongosh).\n"
     "Configuración: ~/.config/askmdb/config, «clave = valor», con las claves mode, lang,\n"
     "model, threads, device, uri, collections, max_rows y max_time_ms. Con contraseña en la\n"
     "uri, el fichero tiene que tener permisos 0600. La elección del primer inicio se guarda en\n"
     "~/.local/state/askmdb/choice; la configuración y las opciones mandan sobre ella.\n"
     "\n"
     "Códigos de salida: 0 bien, 1 error de askmdb, 2 uso incorrecto, 3 bloqueada por\n"
     "seguridad, 4 error de MongoDB, 130 interrumpido.\n",
     "Usage: askmdb [options] <question…>\n"
     "\n"
     "Turns a natural language question into a MongoDB aggregate pipeline, explains it,\n"
     "checks that it only reads and runs it if you confirm.\n"
     "\n"
     "Options:\n"
     "  --uri URI            MongoDB URI (otherwise: ASKMDB_URI or \"uri\" in the config)\n"
     "  --db DB              database (otherwise: the one in the URI)\n"
     "  -c, --collections a,b  collections given to the model (otherwise: all, up to 50)\n"
     "  --refresh            sample the schema again even if the cache is fresh\n"
     "  --print-schema       write the schema block and exit (no question)\n"
     "  --print-prompt       write the exact prompt the model would see and exit\n"
     "  --schema-from FILE   use a block saved with --print-schema instead of sampling;\n"
     "                       only connects if something has to run\n"
     "  --mode MODE          suggest | confirm | auto\n"
     "  --json               one JSON object as output; never asks; runs only in auto\n"
     "  --model PATH         use this GGUF file instead of the downloaded model\n"
     "  --device MODE        auto | cpu | gpu: where the model runs (auto: what was chosen on the\n"
     "                       first start)\n"
     "  --detect             detect the hardware again and choose device and model\n"
     "  --lang es|en         interface language\n"
     "  -h, --help           show this help\n"
     "  --version            show the version\n"
     "\n"
     "Answers: y = run, n = no, c = copy the query (mongosh syntax).\n"
     "Configuration: ~/.config/askmdb/config, \"key = value\", with the keys mode, lang,\n"
     "model, threads, device, uri, collections, max_rows and max_time_ms. With a password in\n"
     "the uri, the file must have mode 0600. The choice of the first start is saved in\n"
     "~/.local/state/askmdb/choice; the config and the options take precedence over it.\n"
     "\n"
     "Exit codes: 0 ok, 1 askmdb error, 2 usage error, 3 blocked for safety,\n"
     "4 MongoDB error, 130 interrupted.\n"},
    {Msg::UnknownFlag, "askmdb: opción desconocida: ", "askmdb: unknown option: "},
    {Msg::MissingValue, "askmdb: falta el valor de la opción: ", "askmdb: missing value for option: "},
    {Msg::BadValue, "askmdb: valor no válido para {}: {}", "askmdb: invalid value for {}: {}"},
    {Msg::Conflict, "askmdb: estas opciones no se pueden combinar: ", "askmdb: these options cannot be combined: "},
    {Msg::QuestionTooLong, "askmdb: pregunta demasiado larga (máximo {} bytes)",
     "askmdb: question too long (at most {} bytes)"},
    {Msg::NoUri, "askmdb: falta la URI de MongoDB: usa --uri, ASKMDB_URI o «uri = …» en la configuración",
     "askmdb: missing MongoDB URI: use --uri, ASKMDB_URI or \"uri = …\" in the config"},
    {Msg::InvalidUri, "askmdb: URI de MongoDB no válida: ", "askmdb: invalid MongoDB URI: "},
    {Msg::UriPasswordWarning,
     "askmdb: aviso: la contraseña de --uri queda en el historial de la shell; mejor ASKMDB_URI o la configuración (0600)",
     "askmdb: warning: the password in --uri stays in the shell history; prefer ASKMDB_URI or the config (0600)"},
    {Msg::NoDb, "askmdb: falta la base de datos: ponla en la URI (…/base) o usa --db",
     "askmdb: missing database: put it in the URI (…/db) or use --db"},
    {Msg::DbBlocked, "askmdb: base «{}» bloqueada: {}", "askmdb: database \"{}\" blocked: {}"},
    {Msg::TooManyCollections,
     "askmdb: la base tiene {} colecciones (más de {}); elige cuáles con -c a,b o con «collections = a,b» en la "
     "configuración. Colecciones:",
     "askmdb: the database has {} collections (more than {}); choose some with -c a,b or with \"collections = a,b\" "
     "in the config. Collections:"},
    {Msg::SchemaTooLarge, "askmdb: el esquema no cabe en el presupuesto del modelo. Coste por colección:",
     "askmdb: the schema does not fit the model's budget. Cost per collection:"},
    {Msg::CostLine, "  {} ~{} tokens (estimado)", "  {} ~{} tokens (estimated)"},
    {Msg::CollectionsHint, "Elige menos colecciones con -c a,b o con «collections = a,b» en la configuración.",
     "Choose fewer collections with -c a,b or with \"collections = a,b\" in the config."},
    {Msg::CacheWrite, "askmdb: no se ha podido guardar la caché del esquema: ",
     "askmdb: could not write the schema cache: "},
    {Msg::SchemaFromRead, "askmdb: no se puede leer el esquema {}: {}", "askmdb: cannot read the schema {}: {}"},
    {Msg::SchemaFromTooLarge, "askmdb: el esquema {} pasa de {} bytes", "askmdb: the schema {} is larger than {} bytes"},
    {Msg::SchemaFromInvalid, "askmdb: el esquema {} no es un bloque válido: {}",
     "askmdb: the schema {} is not a valid block: {}"},
    {Msg::SchemaFromRender,
     "askmdb: el esquema {} no está escrito exactamente como lo escribe askmdb; vuelve a sacarlo con --print-schema",
     "askmdb: the schema {} is not written exactly as askmdb writes it; produce it again with --print-schema"},
    {Msg::MongoConnect, "askmdb: no se puede conectar con MongoDB: ", "askmdb: cannot connect to MongoDB: "},
    {Msg::MongoAuth, "askmdb: error de autenticación en MongoDB: ", "askmdb: MongoDB authentication failed: "},
    {Msg::MongoTimeout, "askmdb: MongoDB ha tardado demasiado: ", "askmdb: MongoDB took too long: "},
    {Msg::MongoServer, "askmdb: error de MongoDB: ", "askmdb: MongoDB error: "},
    {Msg::ModelMissing, "askmdb: no existe el fichero del modelo: ", "askmdb: model file not found: "},
    {Msg::NoModelsDir,
     "askmdb: no sé dónde guardar el modelo: define HOME o XDG_DATA_HOME, o usa «--model RUTA»",
     "askmdb: don't know where to store the model: set HOME or XDG_DATA_HOME, or use \"--model PATH\""},
    {Msg::DownloadOffer,
     "askmdb necesita descargar el modelo (solo la primera vez):\n"
     "  fichero: {}\n"
     "  tamaño:  {} MB\n"
     "  origen:  {}\n",
     "askmdb needs to download the model (only the first time):\n"
     "  file:   {}\n"
     "  size:   {} MB\n"
     "  source: {}\n"},
    {Msg::DownloadPrompt, "¿Descargarlo ahora? [S/n] ", "Download it now? [Y/n] "},
    {Msg::DownloadDeclined,
     "askmdb: no se ha descargado el modelo. Puedes descargarlo tú y usar «askmdb --model RUTA …».",
     "askmdb: the model was not downloaded. You can download it yourself and use \"askmdb --model PATH …\"."},
    {Msg::DownloadNoTty,
     "askmdb: falta el modelo {} ({} MB). Ejecuta askmdb en un terminal (sin --json) para descargarlo,\n"
     "o descárgalo de {} y usa «askmdb --model RUTA …».",
     "askmdb: the model {} ({} MB) is missing. Run askmdb in a terminal (without --json) to download it,\n"
     "or download it from {} and use \"askmdb --model PATH …\"."},
    {Msg::DownloadDone, "Modelo descargado y verificado: ", "Model downloaded and verified: "},
    {Msg::EngineError, "askmdb: error del motor: ", "askmdb: engine error: "},
    {Msg::InvalidResponse, "askmdb: no he podido generar una respuesta válida",
     "askmdb: I could not generate a valid response"},
    {Msg::Incomplete, " (respuesta incompleta)", " (incomplete response)"},
    {Msg::Timeout, "askmdb: ha tardado demasiado; prueba a reformular la pregunta",
     "askmdb: it took too long; try rephrasing the question"},
    {Msg::LoadFailed, "askmdb: no se ha podido cargar el modelo: ", "askmdb: could not load the model: "},
    {Msg::Refuse, "No puedo: ", "I can't: "},
    {Msg::Unsure, "No lo sé: ", "I don't know: "},
    {Msg::LevelRead, "solo lectura", "read only"},
    {Msg::LevelWarn, "aviso", "warning"},
    {Msg::LevelBlocked, "bloqueada", "blocked"},
    {Msg::NotExecuted, "No se ejecutará.", "It will not be run."},
    {Msg::PromptRead, "¿Ejecutar? [S/n/c(opiar)] ", "Run it? [Y/n/c(opy)] "},
    {Msg::PromptWarn, "¿Ejecutar? [s/N/c(opiar)] ", "Run it? [y/N/c(opy)] "},
    {Msg::Cancelled, "Cancelado", "Cancelled"},
    {Msg::Copied, "Copiado al portapapeles", "Copied to clipboard"},
    {Msg::CopyFailed, "No se ha podido copiar al portapapeles", "Could not copy to the clipboard"},
    {Msg::MoreRows, "hay más filas (tope {})", "there are more rows (limit {})"},
    {Msg::NoRows, "sin resultados", "no results"},
    {Msg::DetectQuestion, "askmdb: --detect no lleva pregunta", "askmdb: --detect takes no question"},
    {Msg::GpuUnavailable, "askmdb: no hay ninguna GPU disponible con Vulkan; usa --device cpu",
     "askmdb: no GPU with Vulkan is available; use --device cpu"},
    {Msg::HwHeader, "Hardware detectado:\n", "Detected hardware:\n"},
    {Msg::HwVulkanYes, "  Vulkan: sí\n", "  Vulkan: yes\n"},
    {Msg::HwVulkanNo, "  Vulkan: no\n", "  Vulkan: no\n"},
    {Msg::HwVulkanMissing,
     "  Vulkan: no se encuentra libvulkan.so.1. Para usar la GPU, instala el cargador de Vulkan\n"
     "  (paquete libvulkan1 en Debian/Ubuntu, vulkan-loader en Fedora/Arch) y ejecuta «askmdb --detect».\n",
     "  Vulkan: libvulkan.so.1 not found. To use the GPU, install the Vulkan loader\n"
     "  (package libvulkan1 on Debian/Ubuntu, vulkan-loader on Fedora/Arch) and run \"askmdb --detect\".\n"},
    {Msg::HwGpu, "  GPU {}: {} ({}), {} MiB libres de {} MiB\n", "  GPU {}: {} ({}), {} MiB free of {} MiB\n"},
    {Msg::HwNoGpu, "  GPU: ninguna\n", "  GPU: none\n"},
    {Msg::HwRam, "  RAM: {} MiB disponibles de {} MiB; {} núcleos\n", "  RAM: {} MiB available of {} MiB; {} cores\n"},
    {Msg::KindDiscrete, "dedicada", "discrete"},
    {Msg::KindIntegrated, "integrada", "integrated"},
    {Msg::KindSoftware, "software", "software"},
    {Msg::OnCpu, "la CPU", "the CPU"},
    {Msg::OnGpu, "la GPU {}", "the GPU {}"},
    {Msg::Proposal, "Recomendado: {} en {} ({} MB)\n", "Recommended: {} on {} ({} MB)\n"},
    {Msg::ChoiceLine, "Elección: {} en {}\n", "Choice: {} on {}\n"},
    {Msg::GpuPickHeader, "La GPU {} tiene memoria para los dos modelos de GPU. ¿Cuál quieres instalar?\n",
     "The GPU {} has memory for both GPU models. Which one do you want to install?\n"},
    {Msg::GpuPickOption, "  {}) {} ({} MB): {}\n", "  {}) {} ({} MB): {}\n"},
    {Msg::GpuPick4b, "rápido y preciso", "fast and accurate"},
    {Msg::GpuPick9b, "un pelín más lento, pero más preciso", "a little slower, but more accurate"},
    {Msg::GpuPickPrompt, "Elige 1 o 2 (Intro = 1, n = ninguno): ", "Choose 1 or 2 (Enter = 1, n = none): "},
    {Msg::ModelSource, "  fichero: {}\n  origen:  {}\n", "  file:   {}\n  source: {}\n"},
    {Msg::ModelPresent, "  ya descargado: {}\n", "  already downloaded: {}\n"},
    {Msg::UsePrompt, "¿Usarlo? [S/n] ", "Use it? [Y/n] "},
    {Msg::OfferCpu, "¿Prefieres {} en la CPU ({} MB)? [S/n] ", "Use {} on the CPU instead ({} MB)? [Y/n] "},
    {Msg::FirstStartDeclined,
     "askmdb: no se ha elegido ningún modelo. Puedes volver a elegir con «askmdb --detect», o forzarlo con "
     "--device y --model.",
     "askmdb: no model was chosen. You can choose again with \"askmdb --detect\", or force it with --device and "
     "--model."},
    {Msg::SelfCheck, "Comprobando que el modelo funciona en {}…\n", "Checking that the model works on {}…\n"},
    {Msg::SelfCheckGpuFailed, "askmdb: el modelo no funciona en la GPU ({}); se usa la CPU\n",
     "askmdb: the model does not work on the GPU ({}); falling back to the CPU\n"},
    {Msg::SelfCheckFailed, "askmdb: el modelo no ha pasado la comprobación ({})",
     "askmdb: the model failed the check ({})"},
    {Msg::ChoiceSaved, "Elección guardada: {} en {}. Para cambiarla: «askmdb --detect» o --device.\n",
     "Choice saved: {} on {}. To change it: \"askmdb --detect\" or --device.\n"},
    {Msg::GpuWarmup,
     "La GPU ya está preparada. La primera respuesta después de encender el equipo puede tardar unos\n"
     "segundos más (lee el modelo del disco), y también la primera tras actualizar el driver de la GPU\n"
     "(la vuelve a preparar). Después va más rápido.\n",
     "The GPU is ready. The first answer after the computer starts may take a few seconds longer (the\n"
     "model is read from disk), and so may the first one after a GPU driver update (the GPU is\n"
     "prepared again). After that it is faster.\n"},
    {Msg::SelfCheckGpuNote, "  La primera vez puede tardar unos 15 segundos: se prepara la GPU (solo una vez).\n",
     "  The first time it may take about 15 seconds: the GPU is being prepared (only once).\n"},
    {Msg::ChoiceNotSaved, "askmdb: aviso: no se ha podido guardar la elección: ",
     "askmdb: warning: could not save the choice: "},
    {Msg::ConfigOverrides, "Nota: «device» o «model» de la configuración mandan sobre esta elección.\n",
     "Note: \"device\" or \"model\" in the config take precedence over this choice.\n"},
    {Msg::GpuFallback, "askmdb: aviso: la GPU elegida ({}) no está disponible; se usa la CPU con {}\n",
     "askmdb: warning: the chosen GPU ({}) is not available; using the CPU with {}\n"},
};

constexpr bool table_in_order() {
    constexpr std::size_t n = sizeof(kTable) / sizeof(kTable[0]);
    if (n != static_cast<std::size_t>(Msg::Count_)) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<std::size_t>(kTable[i].msg) != i) return false;
    return true;
}
static_assert(table_in_order(), "the i18n table must have every message, in the order of the enum");

}  // namespace

std::string_view tr(UiLang lang, Msg msg) {
    const Entry& e = kTable[static_cast<std::size_t>(msg)];
    return lang == UiLang::Es ? e.es : e.en;
}

std::string fill(std::string_view pattern, std::string_view value) {
    std::string out(pattern);
    const auto pos = out.find("{}");
    if (pos != std::string::npos) out.replace(pos, 2, value);
    return out;
}

std::string fill_all(std::string_view pattern, std::initializer_list<std::string_view> values) {
    std::string out;
    auto next = values.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            if (next != values.end()) out += *next++;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

UiLang ui_lang(config::Lang lang, const char* lc_all, const char* lc_messages, const char* lang_env) {
    if (lang == config::Lang::Es) return UiLang::Es;
    if (lang == config::Lang::En) return UiLang::En;
    for (const char* v : {lc_all, lc_messages, lang_env}) {
        if (v == nullptr || *v == '\0') continue;
        return std::string_view(v).substr(0, 2) == "es" ? UiLang::Es : UiLang::En;
    }
    return UiLang::En;
}

UiLang ui_lang_from_env(config::Lang lang) {
    return ui_lang(lang, std::getenv("LC_ALL"), std::getenv("LC_MESSAGES"), std::getenv("LANG"));
}

}  // namespace askmdb::cli
