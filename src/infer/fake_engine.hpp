#pragma once
// Engine driven by environment variables, for tests (only reachable with ASKMDB_TEST_HOOKS).
//   ASKMDB_FAKE_RAW          raw model output; goes through the real parser (parse_response) and
//                            takes precedence over every other variable (end-to-end JSON errors)
//   ASKMDB_FAKE_TYPE         query | refuse | unsure (default query)
//   ASKMDB_FAKE_COLLECTION   collection (query only)
//   ASKMDB_FAKE_PIPELINE     JSON text of the pipeline; goes through verify::load_pipeline
//                            (query only; refuse/unsure always get "[]")
//   ASKMDB_FAKE_EXPLANATION  explanation
//   ASKMDB_FAKE_ERROR        error code (no suggestion); ASKMDB_FAKE_ERROR_DETAIL, its detail
// An unknown ASKMDB_FAKE_TYPE is an engine error ("invalid_schema"). Only ASKMDB_FAKE_RAW fills
// EngineResult.raw; the other modes leave it empty.

#include <memory>

#include "infer/engine.hpp"

namespace askmdb::infer {

// nullptr unless ASKMDB_FAKE_RAW, ASKMDB_FAKE_TYPE, ASKMDB_FAKE_PIPELINE or ASKMDB_FAKE_ERROR is set.
std::unique_ptr<Engine> fake_engine_from_env();

}  // namespace askmdb::infer
