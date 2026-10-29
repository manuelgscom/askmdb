// The verifier's reasons: stable ids and their texts. This table is the one place where they are
// listed. B-* make a pipeline blocked, W-* a warning. Ids never change meaning; new ones are added.

#include <algorithm>
#include <iterator>

#include "verify/internal.hpp"

namespace askmdb::verify::detail {

namespace {

struct Text {
    std::string_view id, es, en;
};

constexpr Text kReasons[] = {
    // Text and structure
    {"B-PARSE", "El pipeline no es JSON válido", "The pipeline is not valid JSON"},
    {"B-NOT-ARRAY", "El pipeline no es un array", "The pipeline is not an array"},
    {"B-TOO-LONG", "El pipeline pasa de 8 KiB", "The pipeline is longer than 8 KiB"},
    {"B-CONTROL-CHAR", "Hay caracteres de control", "It contains control characters"},
    {"B-UTF8", "Hay UTF-8 no válido", "It contains invalid UTF-8"},
    {"B-DUP-KEY", "Clave repetida en un objeto", "Repeated key in an object"},
    {"B-STAGE-SHAPE", "Cada etapa tiene que ser un objeto con una sola clave",
     "Each stage must be an object with exactly one key"},
    {"B-TOO-MANY-STAGES", "Más de 20 etapas en un mismo pipeline", "More than 20 stages in one pipeline"},
    {"B-TOO-DEEP", "Anidamiento de más de 16 niveles", "Nesting deeper than 16 levels"},
    {"B-BSON-TYPE", "Tipo de dato no permitido", "Data type not allowed"},
    {"B-SYNTAX", "Estructura que el verificador no reconoce", "Structure the verifier does not recognize"},
    // Stages, operators, variables
    {"B-WRITE-STAGE", "Etapa que escribe en la base", "Stage that writes to the database"},
    {"B-SERVER-STAGE", "Etapa que no lee de la colección o lee del servidor",
     "Stage that does not read from the collection or reads server state"},
    {"B-UNSUPPORTED-STAGE", "Etapa no admitida en esta versión", "Stage not supported in this version"},
    {"B-UNKNOWN-STAGE", "Etapa desconocida", "Unknown stage"},
    {"B-UNKNOWN-OPERATOR", "Operador desconocido o no permitido aquí", "Unknown operator or not allowed here"},
    {"B-JS", "Ejecuta JavaScript en el servidor", "Runs JavaScript on the server"},
    {"B-UNKNOWN-VARIABLE", "Variable desconocida", "Unknown variable"},
    {"B-BAD-VARIABLE", "Nombre de variable no válido", "Invalid variable name"},
    {"B-REGEX-OPTIONS", "Opciones de regex no permitidas (solo i, m, s, x)",
     "Regex options not allowed (only i, m, s, x)"},
    // Database and collections
    {"B-SYSTEM-DB", "Base de datos del sistema", "System database"},
    {"B-BAD-DB", "Nombre de base de datos no válido", "Invalid database name"},
    {"B-SYSTEM-COLLECTION", "Colección del sistema", "System collection"},
    {"B-BAD-COLLECTION", "Nombre de colección no válido", "Invalid collection name"},
    {"B-JOIN-TARGET", "from/coll tiene que ser el nombre de una colección de la misma base",
     "from/coll must name a collection of the same database"},
    // askmdb_verify input
    {"B-INPUT", "Línea de entrada no válida", "Invalid input line"},
    // Warnings
    {"W-JOIN", "Une colecciones y puede ser cara", "Joins collections and can be expensive"},
    {"W-SAMPLE", "El resultado cambia en cada ejecución", "The result changes on every run"},
    {"W-RANDOM", "Resultado no determinista", "Non-deterministic result"},
    {"W-REGEX-LONG", "Regex de más de 128 caracteres", "Regex longer than 128 characters"},
    {"W-REGEX-NESTED", "Regex con cuantificadores anidados o alternativas repetidas",
     "Regex with nested quantifiers or repeated alternatives"},
    {"W-REGEX-COMPLEX", "Regex que no se puede analizar con seguridad (opción x, flags en línea, \\Q, \\c...)",
     "Regex that cannot be analysed reliably (option x, inline flags, \\Q, \\c...)"},
    {"W-REGEX-DYNAMIC", "Regex calculada en la consulta, no se puede revisar", "Regex computed in the query, cannot be checked"},
    {"W-UNKNOWN-COLLECTION", "Colección que no está en el esquema", "Collection not in the schema"},
    {"W-UNKNOWN-FIELD", "Campo que no está en el esquema", "Field not in the schema"},
};

// Details come from the pipeline: short and printable.
std::string clean(std::string_view detail) {
    constexpr std::size_t kMax = 60;
    std::size_t cut = std::min(kMax, detail.size());
    while (cut > 0 && cut < detail.size() && (static_cast<unsigned char>(detail[cut]) & 0xC0) == 0x80) --cut;
    std::string out;
    for (char c : detail.substr(0, cut)) {
        auto u = static_cast<unsigned char>(c);
        out += (u < 0x20 || u == 0x7F) ? '?' : c;
    }
    if (cut < detail.size()) out += "...";
    if (!valid_utf8(out) || has_control(out)) out = "?";
    return out;
}

}  // namespace

Reason make_reason(std::string_view id, std::string_view detail) {
    auto it = std::find_if(std::begin(kReasons), std::end(kReasons), [&](const Text& t) { return t.id == id; });
    Reason r;
    r.id = std::string(id);
    if (it == std::end(kReasons)) {
        r.es = r.en = std::string(id);
    } else {
        r.es = std::string(it->es);
        r.en = std::string(it->en);
    }
    if (!detail.empty()) {
        auto d = clean(detail);
        r.es += ": " + d;
        r.en += ": " + d;
    }
    return r;
}

Level level_of(std::string_view id) { return id.starts_with("W-") ? Level::Warn : Level::Blocked; }

}  // namespace askmdb::verify::detail
