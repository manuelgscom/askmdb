# askmdb

[![Release](https://img.shields.io/github/v/release/manuelgscom/askmdb?logo=github&color=blue)](https://github.com/manuelgscom/askmdb/releases) [![License: MIT](https://img.shields.io/badge/license-MIT-blue)](https://github.com/manuelgscom/askmdb/blob/main/LICENSE) [![Languages: English · Spanish](https://img.shields.io/badge/languages-English%20%C2%B7%20Spanish-informational)](#features) [![llama.cpp](https://img.shields.io/badge/built%20with-llama.cpp-informational)](https://github.com/ggml-org/llama.cpp) [![Runs on: CPU · GPU (Vulkan)](https://img.shields.io/badge/runs%20on-CPU%20%C2%B7%20GPU%20%28Vulkan%29-informational)](#the-models) [![Hugging Face](https://img.shields.io/badge/Hugging%20Face-askmdb%20models-FFD21E?logo=huggingface)](https://huggingface.co/collections/manuelgscom/askmdb-6ae398e10b4817ed0e7431f1)

**Ask your MongoDB in plain language. 100% local. One native binary.**

```bash
$ askmdb "top 5 customers by spending this year"

  ❯ db.orders.aggregate([
      { $match: { $expr: { $gte: ["$created_at", { $dateTrunc: { date: "$$NOW", unit: "year" } }] } } },
      { $group: { _id: "$customer_id", total: { $sum: "$total" } } },
      { $sort: { total: -1 } },
      { $limit: 5 }
    ])
    Adds up this year's order totals per customer and shows the top 5.

  ● read only  ·  Run it? [Y/n/c(opy)] y

   _id      total
     7   2,841.50
     3   2,310.00
    12   1,987.25
     5   1,742.90
     1   1,655.40
```

> **You ask. It writes the query, checks it and only reads.**

`askmdb` turns a question in **English or Spanish** into a MongoDB `aggregate` pipeline. It explains the pipeline, checks with deterministic rules that it only reads, and runs it only if you confirm. Results come back as a table or as JSON. Everything happens on your machine: a small language model fine-tuned for this single task, run by a native C++ binary with [llama.cpp](https://github.com/ggml-org/llama.cpp) and the official MongoDB C driver built in. A small model on the CPU by default; a larger, more accurate one on the GPU (through Vulkan) when you have one. No accounts, no API keys, no telemetry. The only connection it makes is to your own MongoDB.

https://github.com/user-attachments/assets/21ef19b5-4a81-4f07-9409-41b8608ac391

*`askmdb` live: questions in plain English, with relative dates, a join and a grouping. It writes the pipeline, explains it, checks that it only reads and runs it (in `auto` mode a join asks first). Real, unedited recording on the CPU with `askmdb-0.8b`.*

## Features

- **English and Spanish.** The explanation comes in the language of the question.
- **Aware of your data.** It samples your collections and gives the model their fields, types and most frequent values, so it writes `"Madrid"` when that is what your documents say. Relative dates ("last month", "this year") are resolved by the server with `$$NOW`.
- **Read-only, decided by code, not by the model.** A deterministic verifier written in C++ walks the whole pipeline, sub-pipelines included, against an allowlist of stages and operators. Each pipeline gets one of three levels:
  - **read only**: known read stages and operators, on collections and fields of the schema;
  - **warning**: it reads, but may be expensive or surprising (joins with `$lookup`, risky regular expressions, fields not in the schema, `$sample`). It never runs without an explicit yes;
  - **blocked**: it writes, administers, runs JavaScript, leaves the database or is not understood (`$out`, `$merge`, `$function`, unknown operators…). It is shown with the reason and **never runs**, in any mode, with any option.
- **Only `aggregate`, always with limits.** It is the only MongoDB operation in the code. Every run has a server time limit, `allowDiskUse: false` and a row cap.
- **Honest when unsure.** When a question asks to change data, `askmdb` says it can't. When the answer is not in the schema, it says it doesn't know instead of guessing.
- **Three modes:**
  - `suggest` shows the pipeline and never runs it.
  - `confirm` (the default) asks before running it.
  - `auto` runs read-only pipelines directly and asks for everything else.
- **Table or JSON.** A table in the terminal, or one JSON object with `--json` for scripts. Copy the query in `mongosh` syntax with `c`.
- **Local.** The schema and the sample values are seen only by the local model. Credentials never enter the prompt, the errors or the JSON output.
- **CPU or GPU, one binary.** On the first run `askmdb` detects your GPU and offers a larger model if it fits; otherwise it stays on the CPU. See [First run](#4-first-run).

https://github.com/user-attachments/assets/ca19f68f-965e-4f6b-9887-f01f3e3a4266

*Safety is decided by code, not by the model: a request to delete data gets an honest "I can't", and a write hidden inside a `$facet` is blocked by the verifier and never runs. Real, unedited recording.*

## Install

The quickest way is the prebuilt binary from the [latest release](https://github.com/manuelgscom/askmdb/releases/latest). It is one file for both CPU and GPU, and there is nothing else to install: no Python, no `mongosh`, no root access, no GPU drivers for the CPU. To compile it yourself instead, see [Build from source](#build-from-source).

### 1. Check the requirements

- Linux on x86_64 with a CPU that supports AVX2.
- glibc 2.29 or later (Ubuntu 20.04, Debian 11, Fedora 30 or newer). <!-- glibc -->
- `curl` or `wget`, for the first model download.
- MongoDB 5.0 or later (self-hosted or Atlas).
- On Windows, use it through WSL 2.
- **Optional, for the GPU:** a dedicated GPU with at least 4 GiB of free VRAM, its driver, and the Vulkan loader `libvulkan.so.1` (`libvulkan1` on Debian and Ubuntu, `vulkan-loader` on Fedora and Arch). Almost every desktop already has the loader. It may be missing on a server with only the NVIDIA driver; then `askmdb` uses the CPU and tells you what to install.

```bash
grep -q avx2 /proc/cpuinfo && echo "AVX2: OK" || echo "AVX2: not supported"
ldd --version | head -n 1     # the last number is the glibc version
ldconfig -p | grep -q libvulkan.so.1 && echo "Vulkan loader: OK" || echo "Vulkan loader: missing (CPU only)"
```

The binary does not link the Vulkan library: it starts on any machine, and opens the loader only when it is going to use the GPU. TLS (OpenSSL) and the MongoDB driver are built in.

### 2. Download, verify and install

```bash
curl -LO https://github.com/manuelgscom/askmdb/releases/latest/download/askmdb-linux-x86_64
curl -LO https://github.com/manuelgscom/askmdb/releases/latest/download/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS     # must print: askmdb-linux-x86_64: OK
chmod +x askmdb-linux-x86_64
mkdir -p ~/.local/bin
mv askmdb-linux-x86_64 ~/.local/bin/askmdb
```

This installs `askmdb` for your user only, in `~/.local/bin`.

### 3. Check that your shell finds it

```bash
askmdb --version
```

If you get `askmdb: command not found`, open a new terminal. Most distributions add `~/.local/bin` to your `PATH` at login, but only once the folder exists. If a new terminal does not help, add it yourself:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### 4. First run

```bash
export ASKMDB_URI='mongodb://localhost:27017/shop'
askmdb "how many orders were cancelled last month?"
```

The first time, `askmdb` looks for a GPU and picks a model:

```
first run ─► dedicated GPU, free VRAM?
               ├─ 8 GiB or more ─► choose: 1) askmdb-4b (Enter)  2) askmdb-9b  n) CPU
               ├─ 4 GiB or more ─► offer askmdb-4b                  [Y/n]
               └─ less, or no GPU ─► askmdb-0.8b on the CPU
```

It always asks before downloading a model from Hugging Face (529 MB, 2.7 GB or 5.6 GB; see [The models](#the-models)). It checks its SHA-256 and stores it in `~/.local/share/askmdb/models/` (or `$XDG_DATA_HOME/askmdb/models/`). After that, the only network connection it makes is to your MongoDB.

- The choice is saved in `~/.local/state/askmdb/choice` (or `$XDG_STATE_HOME/askmdb/choice`). `askmdb --detect` detects the hardware and asks again.
- On the GPU, the first run takes about 11 s<!-- fig:speed.gpu_warmup_s --> longer while the driver prepares the GPU. This happens once; the first answer after the computer starts, or after a GPU driver update, may also take a few seconds longer.
- Only dedicated GPUs are used; integrated and software ones are listed but never chosen.
- If the chosen GPU is no longer available or fails, `askmdb` warns and answers on the CPU with `askmdb-0.8b`. With `--device gpu` (or `device = gpu` in the [configuration](#configuration)) there is no fallback: you see the error.
- Without a terminal, or with `--json`, `askmdb` neither asks nor downloads.

If you already have a GGUF file, use `--model PATH` or the `model` key in the [configuration](#configuration) instead.

### Update and uninstall

- **Update:** repeat step 2. The models you already downloaded are kept, and one is downloaded again only if the new version uses a different one.
- **Uninstall:**

  ```bash
  rm ~/.local/bin/askmdb
  rm -rf ~/.local/share/askmdb ~/.cache/askmdb ~/.local/state/askmdb   # models, schema cache, first-run choice
  rm -rf ~/.config/askmdb                                              # your configuration, if you made one
  ```

  `~/.cache/askmdb` holds the sampled schemas and the saved model states (directory 0700, files 0600). With the NVIDIA driver, `askmdb` also creates `~/.cache/nvidia/GLCache` if it is missing; that is the driver's own shader cache, shared with other programs.

## Usage

```bash
askmdb <question…>
askmdb --db shop "average order value by month this year"
askmdb -c orders,customers "customers from Spain with more than 3 orders"
askmdb --mode suggest "the 10 most expensive products"    # show the pipeline, never run it
askmdb --json --mode auto "orders per status"             # one JSON object, for scripts
askmdb --refresh "…"          # sample the schema again (it is cached for 7 days)
askmdb --print-schema         # show exactly what the model is told about your database
askmdb --detect               # detect the GPU and choose the model again
askmdb --help
askmdb --version
```

Answers at the prompt: `y` runs the pipeline, `n` cancels and `c` copies it to the clipboard in `mongosh` syntax. For a **read only** pipeline Enter means yes; for a **warning** Enter means no. In tmux, copying to the clipboard needs `set-clipboard on`.

Without a terminal `askmdb` runs nothing, except with `--json` in `auto` mode, and then only read-only pipelines. The exit code tells a script what happened: 0 ok (also for "I can't" and "I don't know"), 1 askmdb error, 2 usage error, **3 blocked for safety**, 4 MongoDB error, 130 interrupted.

### Connecting

`askmdb` takes the connection string from, in this order:

1. `--uri URI` (avoid it with a password: it stays in your shell history, and `askmdb` warns about it);
2. the `ASKMDB_URI` environment variable;
3. the `uri` key in the [configuration file](#configuration), which must then have mode 0600 if the URI carries a password (`chmod 600 ~/.config/askmdb/config`), or `askmdb` refuses to start.

The database comes from the URI (`…/shop`) or from `--db`. `mongodb+srv://` (MongoDB Atlas included), TLS and SCRAM authentication work out of the box; the CA certificates are found in the usual places of each distribution, or with `tlsCAFile` in the URI.

```bash
export ASKMDB_URI='mongodb+srv://reader:<password>@cluster0.example.mongodb.net/shop'
```

**Use a read-only user.** The verifier is one layer; the real guarantee is the database's own access control. In `mongosh`, as an administrator:

```js
use shop
db.createUser({ user: "askmdb", pwd: passwordPrompt(), roles: [{ role: "read", db: "shop" }] })
```

### Configuration

`~/.config/askmdb/config` (or `$XDG_CONFIG_HOME/askmdb/config`) takes `key = value` lines, and every key is optional. `askmdb` never creates or edits it.

```ini
# suggest | confirm | auto
mode = confirm
# auto | en | es (language of the interface)
lang = auto
# connection string; with a password, the file must have mode 0600
uri = mongodb://askmdb:secret@db.example.com:27017/shop?authSource=shop
# collections given to the model (default: all, up to 50)
collections = orders, customers, products
# row cap (1-1000) and server time limit in ms (100-300000)
max_rows = 50
max_time_ms = 10000
# auto | cpu | gpu (auto follows the first-run choice)
device = auto
# a GGUF file instead of the downloaded model
model = ~/models/askmdb-0.8b-1.0.0-Q4_K_M.gguf
# inference threads (1-256; default: physical cores, up to 8)
threads = 4
```

Command-line options take precedence over the configuration, and both over the first-run choice. `device = cpu` never touches the GPU.

## How it works

```
 question ─► schema ─────────────► prompt ───────────► local model ─► verifier ─────────► mode ──────► MongoDB
 (en/es)     (sample of documents:  (instructions +     (proposes a     (C++ rules on the   (suggest /   aggregate with
              collections, fields,   schema block,       pipeline, an    BSON that will run: confirm /   a time limit, no
              types, frequent        cached on disk;     explanation,    read only, warning   auto)       disk use and a
              values; cached)        question)           or a refusal)   or blocked)                      row cap ─► table / JSON
```

1. **Schema sampler.** For each collection (up to 50, or the ones you choose with `-c`) it reads 200 documents (the latest by `_id`) and derives field paths (`customer.city`, `items[].sku`), their types and, for short text fields with few distinct values, the most frequent values. Values that look like an email address or a phone number are never listed. The result is a compact block, trimmed to fit the model's budget, cached in `~/.cache/askmdb/` and refreshed with `--refresh` or after 7 days.
2. **Inference engine.** llama.cpp is linked into the binary through `llama.h`, with no server, daemon or port. It runs on the CPU or, through Vulkan, on the GPU. It decodes greedily, caps the output and stops as soon as the JSON object is closed. The state of the instructions and the schema is saved on disk, so only the first question against a database pays for reading the schema.
3. **Verifier.** The pipeline is parsed with libbson into exactly the BSON that would be sent to the server, and a table of C++ rules walks it, including the sub-pipelines of `$facet`, `$lookup` and `$unionWith`. The highest level wins. Duplicate keys, unknown operators and anything it does not understand are blocked. This is the most critical piece of the project and the one with the most tests.
4. **Executor.** `aggregate` with `maxTimeMS`, `allowDiskUse: false` and a row cap. No other MongoDB operation appears in the code.
5. **Model download.** It uses the system's `curl` or `wget`, verifies the SHA-256 and renames the file atomically.

**What the model sees:** collection names, field paths, types and frequent values from the sample, plus your question. Never the connection string, user names or passwords. The model answers with one JSON object, `{"type":"query|refuse|unsure","collection":"…","pipeline":[…],"explanation":"…"}`; anything else is rejected as invalid, and nothing runs.

## The models

The same task, contract and prompt in four sizes, all fine-tunes of Qwen3.5 quantized to Q4_K_M. Scores are on the public test set [`manuelgscom/askmdb-test`](https://huggingface.co/datasets/manuelgscom/askmdb-test): 500 questions in English and Spanish over synthetic databases that the training never saw. A question counts as right when the predicted pipeline, **run against the test database, returns the same result** as the reference pipeline; there are no human or model graders involved.

| Model | Used by `askmdb` | Download | Test accuracy |
|---|---|---|---|
| [`askmdb-0.8b`](https://huggingface.co/manuelgscom/askmdb-0.8b-GGUF) | CPU (default) | 529 MB | **83.6 %**<!-- fig:acc.askmdb-0.8b-v2.pct --> (418<!-- fig:acc.askmdb-0.8b-v2.k -->/500) |
| [`askmdb-2b`](https://huggingface.co/manuelgscom/askmdb-2b-GGUF) | not used; for llama.cpp directly | 1.3 GB | **86.8 %**<!-- fig:acc.askmdb-2b-v2.pct --> (434<!-- fig:acc.askmdb-2b-v2.k -->/500) |
| [`askmdb-4b`](https://huggingface.co/manuelgscom/askmdb-4b-GGUF) | GPU, ≥ 4 GiB free | 2.7 GB | **89.6 %**<!-- fig:acc.askmdb-4b-v2.pct --> (448<!-- fig:acc.askmdb-4b-v2.k -->/500) |
| [`askmdb-9b`](https://huggingface.co/manuelgscom/askmdb-9b-GGUF) | GPU, ≥ 8 GiB free, if you choose it | 5.6 GB | **91.6 %**<!-- fig:acc.askmdb-9b-lora.pct --> (458<!-- fig:acc.askmdb-9b-lora.k -->/500) |

For comparison, the same test with general models that were not fine-tuned (each given worked examples in its prompt, which the askmdb models do not need):

| Model | Test accuracy | Invalid JSON |
|---|---|---|
| Qwen3.5-9B | 47.8 %<!-- fig:acc.Qwen3.5-9B.pct --> | 61<!-- fig:acc.Qwen3.5-9B.json_invalid --> of 500 |
| Gemma 4 E4B-it | 45.4 %<!-- fig:acc.Gemma 4 E4B-it.pct --> | 112<!-- fig:acc.Gemma 4 E4B-it.json_invalid --> of 500 |
| Gemma 4 E2B-it | 44.2 %<!-- fig:acc.Gemma 4 E2B-it.pct --> | 43<!-- fig:acc.Gemma 4 E2B-it.json_invalid --> of 500 |
| Qwen3.5-4B | 37.4 %<!-- fig:acc.Qwen3.5-4B.pct --> | 65<!-- fig:acc.Qwen3.5-4B.json_invalid --> of 500 |
| Qwen3.5-2B | 20.4 %<!-- fig:acc.Qwen3.5-2B.pct --> | 72<!-- fig:acc.Qwen3.5-2B.json_invalid --> of 500 |
| Qwen3.5-0.8B | 6.6 %<!-- fig:acc.Qwen3.5-0.8B.pct --> | 258<!-- fig:acc.Qwen3.5-0.8B.json_invalid --> of 500 |

`askmdb-0.8b` gives invalid JSON in 3<!-- fig:acc.askmdb-0.8b-v2.json_invalid --> of 500 questions and refuses all 31<!-- fig:acc.askmdb-0.8b-v2.expected_type.refuse.k --> requests to change data in the test; it wrongly refuses only 1<!-- fig:acc.askmdb-0.8b-v2.false_refusals --> answerable question. A wrong answer can still be a valid, read-only pipeline that returns the wrong rows, so always read the explanation.

**Speed** (one reference desktop with a 16 GB GPU and a CPU with 8 threads; one `askmdb` process per question, model loading included; median of 30 questions):

| Model | Device | Next questions | First question against a database | VRAM in use |
|---|---|---|---|---|
| `askmdb-0.8b` | CPU | 1.18 s<!-- fig:speed.askmdb-0.8b.cpu.warm_ms --> | 3.02 s<!-- fig:speed.askmdb-0.8b.cpu.first_ms --> | — |
| `askmdb-4b` | GPU | 1.34 s<!-- fig:speed.askmdb-4b.gpu.warm_ms --> | 1.54 s<!-- fig:speed.askmdb-4b.gpu.first_ms --> | 3.3 GiB<!-- fig:speed.askmdb-4b.gpu.vram_mib --> |
| `askmdb-9b` | GPU | 1.85 s<!-- fig:speed.askmdb-9b.gpu.warm_ms --> | 2.14 s<!-- fig:speed.askmdb-9b.gpu.first_ms --> | 5.5 GiB<!-- fig:speed.askmdb-9b.gpu.vram_mib --> |

The first question against a database reads the whole schema block; the next ones reuse its saved state. Times do not include MongoDB itself.

The models are fine-tunes of Qwen3.5-0.8B, Qwen3.5-2B, Qwen3.5-4B and Qwen3.5-9B (the last one with a merged LoRA). The model cards, in the [collection](https://huggingface.co/collections/manuelgscom/askmdb-6ae398e10b4817ed0e7431f1), describe the evaluation, the prompt contract and how to use each model with `llama-server`.

## Build from source

Use this if you want to read or change the code. For everyday use, the [prebuilt binary](#install) is simpler. llama.cpp and the MongoDB C driver are compiled along with `askmdb` (git submodules); OpenSSL comes from your distribution. The default build is CPU-only; the GPU is optional, see below.

### 1. Install the build tools

You need git, CMake 3.28 or later, GCC 13 or later and the OpenSSL headers.

| Distribution | Command |
|---|---|
| Ubuntu 24.04 or later, Debian 13 or later | `sudo apt install build-essential cmake git libssl-dev` |
| Fedora | `sudo dnf install gcc-c++ make cmake git openssl-devel` |
| Arch Linux | `sudo pacman -S --needed base-devel cmake git openssl` |

Ubuntu 22.04 and Debian 12 ship an older CMake and GCC. There, use the prebuilt binary. Without the OpenSSL headers the build still works, but without TLS and SCRAM (only a local `mongod` without authentication).

### 2. Get the code

```bash
git clone --recursive https://github.com/manuelgscom/askmdb.git
cd askmdb
```

`--recursive` also fetches llama.cpp, the MongoDB C driver and doctest, which are git submodules. If you cloned without it, run `git submodule update --init`.

### 3. Build and install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DASKMDB_TEST_HOOKS=OFF
cmake --build build -j"$(nproc)" --target askmdb
install -Dm755 build/askmdb ~/.local/bin/askmdb
askmdb --version
```

The binary is optimised for the CPU of the machine where you build it (`-DASKMDB_NATIVE=OFF` turns that off).

To include the GPU, add `-DASKMDB_VULKAN=ON` to the first command. It needs the Vulkan headers, SPIRV-Headers and the `glslc` shader compiler, from the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) or your distribution (on Debian and Ubuntu: `sudo apt install libvulkan-dev spirv-headers glslc`). The result still starts without a Vulkan loader, like the release binary.

From here on, everything works as in [Install](#install): if `askmdb` is not found, see [step 3](#3-check-that-your-shell-finds-it); the first run downloads the model.

### Development build and tests

```bash
cmake -B build-dev
cmake --build build-dev -j"$(nproc)"
ctest --test-dir build-dev
```

The development build keeps the test hooks that the tests need, so do not use it as your everyday binary.

- `ctest` runs the unit tests (doctest), the verifier cases and the end-to-end tests in a pseudo-terminal. A fake engine stands in for the model, so these tests need no model file.
- The verifier cases in `tests/verify/` (`lectura.jsonl`, `aviso.jsonl`, `bloqueada.jsonl`: read only, warning and blocked) fix the expected level of each pipeline.
- Three suites need something external, and their cases are skipped without it:

  ```bash
  # mongo: a mongod (5.0 or later) with the test databases
  docker run -d --name askmdb-mongo -p 27018:27017 mongo:8.0
  docker cp tests/mongo/humo.js askmdb-mongo:/humo.js
  docker exec askmdb-mongo mongosh --quiet /humo.js
  ASKMDB_E2E_URI=mongodb://127.0.0.1:27018/humo ctest --test-dir build-dev -L mongo

  # tls: a mongod with TLS and the test CA from tests/mongo/gen-tls.sh
  ASKMDB_E2E_TLS_URI='mongodb://…/humo?tls=true&tlsCAFile=…/tests/mongo/tls/ca.pem' ctest --test-dir build-dev -L tls

  # model: a real GGUF
  ASKMDB_TEST_MODEL=/path/to/askmdb-0.8b-1.0.0-Q4_K_M.gguf ctest --test-dir build-dev -L model
  ```

### Repository layout

```
CMakeLists.txt
src/
  main.cpp
  cli/        options, modes, prompts, output, first-run flow, i18n (en/es)
  config/     ~/.config/askmdb/config parser, saved first-run choice
  mongo/      connection (URI, TLS, CA search), sampling, aggregate executor, table and mongosh output
  schema/     schema inference, the block the model sees, cache
  infer/      prompt, llama.cpp engine, JSON parser, GPU detection, Vulkan loader shim
  verify/     read-only verifier (plus the askmdb_verify helper)
  exec/       terminal, clipboard
  fetch/      model download and SHA-256
tests/        doctest unit tests, verifier cases, golden prompt files, e2e tests in a pty, test databases
third_party/  llama.cpp, mongo-c-driver, doctest (submodules)
```

## Limitations

- Only `aggregate`: no writes, no indexes, no administration, by design. No Atlas Search or vector search (`$search`, `$vectorSearch`).
- MongoDB 5.0 or later.
- Linux on x86_64 only (WSL 2 works). No macOS or ARM for now.
- The model can be wrong, in about 1 of 6 questions on the test set with `askmdb-0.8b` and 1 of 12 with `askmdb-9b`, and a wrong pipeline can look plausible. **Read the pipeline and its explanation before trusting the result.**
- The schema comes from a sample of 200 documents per collection: fields that appear only in older documents may be missing. Use `--refresh` after the data changes shape.
- The GPU is used only through Vulkan (no CUDA or ROCm builds), and only one GPU at a time. It has been tested on NVIDIA; Vulkan is standard, but AMD and Intel GPUs have not been tested yet.
- `askmdb` is not a security tool. Its safety comes from the deterministic verifier, the confirmations and, above all, a read-only database user.

## License

The code is under the [MIT](LICENSE) license. The models are under Apache-2.0, the same license as their base models. The release binary also contains llama.cpp/ggml, the MongoDB C driver (libmongoc and libbson), OpenSSL, utf8proc, nlohmann/json, code from the Khronos Vulkan and SPIR-V headers and the GCC runtime; their licenses and notices are in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), along with doctest, which only the tests use.

MongoDB is a trademark of MongoDB, Inc. askmdb is an independent project, not affiliated with or endorsed by MongoDB, Inc.

---

Author: [Manuel García](https://manuelgs.com)
